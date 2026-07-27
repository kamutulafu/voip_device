#include "audio_driver.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "uvc_stream.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "audio_driver";

#define AUDIO_NVS_NAMESPACE     "audio_cfg"
#define AUDIO_NVS_KEY_VOL       "volume"

#define ES8311_I2C_ADDR         0x18
#define GPIO_OUTPUT_PA          -1      /* 外接 ES8311 模块 PA-EN 已硬件上拉 3.3V 打开，不使用 GPIO53 */

#define I2S_NUM                 0
#define I2S_MCK_IO              I2S_GPIO_UNUSED /* ES8311 3-Wire 模式（完全无需 MCK 引脚，由 BCK 自供给内部主时钟） */
#define I2S_BCK_IO              21      /* 位时钟 BCK -> GPIO 21 (32bit slot: 16k*32*2 = 1.024MHz) */
#define I2S_WS_IO               20      /* 帧时钟 WS -> GPIO 20 (避开 USB-OTG GPIO 24) */
#define I2S_DO_IO               33      /* ESP32 DOUT -> 芯片 DI (SDIN/扬声器播放数据) */
#define I2S_DI_IO               22      /* ESP32 DIN  -> 芯片 DO (SDOUT/麦克风录音数据，避开 USB-OTG GPIO 25) */

#define AUDIO_SAMPLE_RATE       16000   /* 16 kHz 语音通讯标准模式 */
#define AUDIO_MCLK_MULTIPLE     256     /* 256x Fs = 4.096 MHz (较低辐射、边沿干净的方波) */

/* ES8311 DAC volume register (0x32): 0 = mute, 0xFF = max (~0 dB scale). */
#define ES8311_REG_DAC_VOLUME   0x32
#define ES8311_REG_DAC_MUTE     0x31
/* 0x31 的 bit6|bit5 是 DAC 静音位，与官方 es8311_voice_mute() 一致 */
#define ES8311_DAC_MUTE_BITS    0x60

/* BCK(= 内部 MCLK) 起来后 ES8311 的时钟/模拟通路建立时间，实测留足余量 */
#define ES8311_CLOCK_SETTLE_MS  20
#define ES8311_OUTPUT_SETTLE_MS 60

static i2c_master_dev_handle_t s_es8311_i2c_handle = NULL;
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;
static bool s_audio_initialized = false;
static bool s_shutdown_handler_registered = false;
static uint8_t s_current_volume = AUDIO_DEFAULT_VOLUME;
static volatile bool s_play_abort_flag = false;

void audio_play_abort(void)
{
    s_play_abort_flag = true;
}

static uint8_t audio_load_saved_volume(void)
{
    nvs_handle_t handle;
    uint8_t vol = AUDIO_DEFAULT_VOLUME;
    if (nvs_open(AUDIO_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, AUDIO_NVS_KEY_VOL, &vol) != ESP_OK || vol > 100) {
            vol = AUDIO_DEFAULT_VOLUME;
        }
        nvs_close(handle);
    }
    return vol;
}

void audio_play_clear_abort(void)
{
    s_play_abort_flag = false;
}

bool audio_play_is_aborted(void)
{
    return s_play_abort_flag;
}

#pragma pack(push, 1)
typedef struct {
    char chunk_id[4];          // "RIFF"
    uint32_t chunk_size;       // file_size - 8
    char format[4];            // "WAVE"
    char subchunk1_id[4];      // "fmt "
    uint32_t subchunk1_size;   // 16 for PCM
    uint16_t audio_format;     // 1 for PCM
    uint16_t num_channels;     // 2 (Stereo)
    uint32_t sample_rate;      // 16000
    uint32_t byte_rate;        // sample_rate * num_channels * bits_per_sample / 8
    uint16_t block_align;      // num_channels * bits_per_sample / 8
    uint16_t bits_per_sample;  // 16
    char subchunk2_id[4];      // "data"
    uint32_t subchunk2_size;   // data_size
} wav_header_t;
#pragma pack(pop)

static esp_err_t es8311_write_reg(uint8_t reg, uint8_t val)
{
    if (s_es8311_i2c_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_es8311_i2c_handle, buf, 2, pdMS_TO_TICKS(1000));
}

esp_err_t audio_es8311_read_reg(uint8_t reg, uint8_t *val)
{
    if (s_es8311_i2c_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_es8311_i2c_handle, &reg, 1, val, 1, pdMS_TO_TICKS(1000));
}

static esp_err_t es8311_init_internal(void)
{
    esp_err_t ret = ESP_OK;

    // Wait 20ms for ES8311 chip 3.3V power domain to stabilize upon cold boot
    vTaskDelay(pdMS_TO_TICKS(20));

    // Reset ES8311
    ret |= es8311_write_reg(0x00, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(20));
    ret |= es8311_write_reg(0x00, 0x00);
    ret |= es8311_write_reg(0x00, 0x80); // Power on
    vTaskDelay(pdMS_TO_TICKS(10));

    // Clock configuration: ES8311 3-Wire I2S mode (Uses BCK pin as internal MCLK source)
    // I2S 侧使用 32bit slot，因此 BCK = Fs * 32 * 2 = 64 x Fs (16kHz -> 1.024 MHz)
    // Register 01: 0xBF (Bit 7=1: Use BCK pin as MCLK source, all clocks enabled)
    ret |= es8311_write_reg(0x01, 0xBF);

    // Register 02: pre_div=1, pre_multi=x4 -> (1-1)<<5 | 0x02<<3 = 0x10
    // 内部时钟 = 64 x Fs x 4 = 256 x Fs (4.096 MHz @16kHz)，与官方 coeff 表
    // {1024000, 16000, pre_div 1, pre_multi 0x02} 一致；若 pre_multi 配成 x1/x2
    // 则内部时钟不足 256Fs，DAC 不出声。
    ret |= es8311_write_reg(0x02, 0x10);

    // Register 03: fs_mode=0, adc_osr=16 -> 0x10
    ret |= es8311_write_reg(0x03, 0x10);

    // Register 04: dac_osr=16 -> 0x10
    ret |= es8311_write_reg(0x04, 0x10);

    // Register 05: adc_div=1, dac_div=1 -> 0x00
    ret |= es8311_write_reg(0x05, 0x00);

    // Register 06: bclk_div=4 -> clear bit 5 for sclk non-inverted, bclk_div-1=3 -> 0x03
    ret |= es8311_write_reg(0x06, 0x03);

    // Register 07: lrck_h=0 -> 0x00
    ret |= es8311_write_reg(0x07, 0x00);

    // Register 08: lrck_l=255 -> 0xFF (since LRCK divider = 256)
    ret |= es8311_write_reg(0x08, 0xFF);

    // Format configuration: Slave mode, 16-bit I2S
    // Register 00: Bit 6 is 0 (Slave mode)
    // Register 09: SDP In 16-bit -> 0x0C (3 << 2)
    ret |= es8311_write_reg(0x09, 0x0C);
    // Register 0A: SDP Out 16-bit -> 0x0C (3 << 2)
    ret |= es8311_write_reg(0x0A, 0x0C);

    // System configuration (from standard initialization sequence)
    ret |= es8311_write_reg(0x0D, 0x01); // Power up analog circuitry
    ret |= es8311_write_reg(0x0E, 0x02); // Enable analog PGA, enable ADC modulator
    ret |= es8311_write_reg(0x12, 0x00); // Power up DAC
    ret |= es8311_write_reg(0x13, 0x10); // Enable output to HP drive (必须为 0x10，写 0x00 会完全没声音)
    ret |= es8311_write_reg(0x1C, 0x6A); // ADC Equalizer bypass, cancel DC offset
    ret |= es8311_write_reg(0x37, 0x08); // Bypass DAC equalizer

    // Microphone configuration
    // NOTE: the capture chain was previously running with a very high total
    // input gain (mic +18dB + digital ADC +4.5dB on top of the analog PGA).
    // For near-field speech this clips the ADC, which the far end (WeChat
    // mini-program) hears as loud "static / current" noise riding on the voice.
    // Keep the gain moderate to leave headroom and avoid clipping.
    ret |= es8311_write_reg(0x17, 0xC8); // Restore ADC digital volume to +4.5 dB (was 0 dB)
    ret |= es8311_write_reg(0x14, 0x1A); // Enable analog MIC (recommended analog init)
    ret |= es8311_write_reg(0x16, 0x03); // Restore Mic gain to +18 dB (was +12 dB)

    /* 此刻 BCK 还没有输出，而 3-Wire 模式下 BCK 就是 ES8311 的内部 MCLK，
     * 也就是说 codec 现在完全没有时钟，CSM 上电 / 时钟管理器 / DAC 的 DC offset
     * 抵消都还没真正开始建立。因此这里保持 DAC 静音，等 I2S 使能、BCK 稳定之后
     * 再由 es8311_enable_output() 解除静音，避免第一段语音落在建立期里出现沙哑失真。 */
    ret |= es8311_write_reg(ES8311_REG_DAC_MUTE, ES8311_DAC_MUTE_BITS);
    {
        uint8_t vol = audio_load_saved_volume();
        if (vol > 100) {
            vol = 100;
        }
        s_current_volume = vol;
    }

    /* Power on (CSM) last, after clock / format / analog config - 与官方驱动顺序一致 */
    ret |= es8311_write_reg(0x00, 0x80);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 register init failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ES8311 Codec register init successful (DAC still muted)");
    return ESP_OK;
}

/**
 * @brief I2S 使能、BCK 稳定之后再打开 DAC 输出。
 *
 * 必须在 i2s_channel_enable() 之后调用：3-Wire 模式下 BCK 就是 ES8311 的内部
 * MCLK 源，只有 BCK 在跑，CSM 上电和时钟管理器才会真正开始建立。这期间
 * DMA 因为 auto_clear = true 送的是全 0，喇叭上是静音，等建立完成再解除静音。
 */
static esp_err_t es8311_enable_output(void)
{
    esp_err_t ret = ESP_OK;

    vTaskDelay(pdMS_TO_TICKS(ES8311_CLOCK_SETTLE_MS));

    /* 在有时钟的条件下重新执行一次 CSM 上电 */
    ret |= es8311_write_reg(0x00, 0x80);
    vTaskDelay(pdMS_TO_TICKS(ES8311_OUTPUT_SETTLE_MS));

    /* Same mapping as es8311_voice_volume_set(): 0..100 -> 0x00..0xFF */
    uint8_t reg32 = (s_current_volume == 0) ? 0 : (uint8_t)((s_current_volume * 256 / 100) - 1);
    ret |= es8311_write_reg(ES8311_REG_DAC_VOLUME, reg32);
    ret |= es8311_write_reg(ES8311_REG_DAC_MUTE, 0x00);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 output enable failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ES8311 DAC unmuted, volume %u%% (reg 0x32=0x%02X)",
             (unsigned)s_current_volume, reg32);
    return ESP_OK;
}

esp_err_t audio_init(void)
{
    if (s_audio_initialized) {
        ESP_LOGI(TAG, "Audio already initialized");
        return ESP_OK;
    }

    // 1. Initialize PA GPIO (if configured)
    if (GPIO_OUTPUT_PA >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << (GPIO_OUTPUT_PA >= 0 ? GPIO_OUTPUT_PA : 0)),
            .mode = GPIO_MODE_OUTPUT,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&io_conf);
        gpio_set_level(GPIO_OUTPUT_PA, 1); // Enable PA
    }

    // 2. Initialize dedicated I2C Bus for Audio (SDA=2, SCL=3, I2C Port 1)
    static i2c_master_bus_handle_t s_audio_i2c_bus = NULL;
    if (s_audio_i2c_bus == NULL) {
        ESP_LOGI(TAG, "Initializing dedicated I2C Master Bus (SDA=2, SCL=3, Port 1) for Audio...");
        i2c_master_bus_config_t i2c_bus_cfg = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .i2c_port = 1,
            .scl_io_num = 3,
            .sda_io_num = 2,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t err = i2c_new_master_bus(&i2c_bus_cfg, &s_audio_i2c_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create Audio I2C bus: %s", esp_err_to_name(err));
            return err;
        }
    }
    i2c_master_bus_handle_t i2c_bus = s_audio_i2c_bus;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = 20000, /* 20 kHz I2C speed for maximum noise immunity */
    };
    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_es8311_i2c_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ES8311 device to I2C: %s", esp_err_to_name(err));
        return err;
    }

    // 3. Initialize ES8311 Codec via I2C (Before I2S clocks start)
    err = es8311_init_internal();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 codec internal init failed: %s", esp_err_to_name(err));
        return err;
    }

    // 4. Initialize & Enable I2S Channels (Audio data transmission)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    err = i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channels: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    /* 32-bit slot: BCK = Fs * 64 (16kHz -> 1.024 MHz)，ES8311 以 BCK 作为内部 MCLK
     * 时必须 >= 64 x Fs，官方 coeff 表也只支持到 64 x Fs。数据仍为 16bit，
     * 高 16bit 有效、低 16bit 补 0，ES8311 SDP 配 16bit 可正确锁存。 */
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    /* 关键：ws_width 必须跟着 slot 宽度一起改成 32。
     * I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG() 把 ws_width 初始化为 data_bit_width(=16)，
     * 而 i2s_hal 用 slot_bit_width 设置 half_sample_bit(=32)。二者不一致时，
     * 一帧 64 个 BCK、半帧 32 个 BCK，但 WS 只维持 16 个 BCK ——
     * LRCK 变成 16/48 的畸形波形而不是标准的 50% 方波。ES8311 按 LRCK 边沿划分声道，
     * 右声道边沿提前 16 个 BCK，锁存到的是填充位，表现为声音沙哑失真、声道错位。 */
    std_cfg.slot_cfg.ws_width = I2S_SLOT_BIT_WIDTH_32BIT;

    std_cfg.clk_cfg.mclk_multiple = AUDIO_MCLK_MULTIPLE;

    err = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S TX: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S RX: %s", esp_err_to_name(err));
        return err;
    }

    /* 杜邦线连接外部 ES8311 模块时，飞线无阻抗控制、地回流路径长，
     * 默认驱动能力下 ns 级的边沿会在接收端产生过冲/振铃，并通过线间耦合
     * 在相邻的 BCK 上打出毛刺，导致 ES8311 逐位锁存错位（表现为声音沙哑失真）。
     * 这里把三根输出降到最弱驱动档，作用等同于在线上并一颗小电容 —— 放缓边沿。
     * BCK 只有 1.024 MHz（16 kHz x 64），bit 周期约 500 ns，余量足够。
     * 注意：这只是缓解手段，正解是源端串 33~100Ω 电阻并给每根信号配紧邻地线。 */
    gpio_set_drive_capability(I2S_BCK_IO, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(I2S_WS_IO,  GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(I2S_DO_IO,  GPIO_DRIVE_CAP_0);

    err = i2s_channel_enable(tx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S TX: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S RX: %s", esp_err_to_name(err));
        return err;
    }

    /* BCK 现在才开始输出，等 ES8311 建立完成后再解除 DAC 静音 */
    err = es8311_enable_output();
    if (err != ESP_OK) {
        return err;
    }

    s_audio_initialized = true;

    /* 关键：I2S 的 TX/RX DMA 一旦使能就会持续读写内存，而 esp_restart() 触发的是
     * SW_CPU_RESET —— 它复位 CPU，但不会复位所有外设/DMA。DMA 在复位后继续搬运，
     * 会覆盖 ROM loader 刚刚装载到 HP SRAM 里的二级 bootloader，导致
     * "Illegal instruction" panic，再由 LP_WDT 复位一次才能正常启动。
     * 注册 shutdown handler，让任何 esp_restart() 路径都先关掉 I2S 通道。 */
    if (!s_shutdown_handler_registered) {
        esp_err_t sh = esp_register_shutdown_handler(audio_deinit);
        if (sh == ESP_OK) {
            s_shutdown_handler_registered = true;
        } else {
            ESP_LOGW(TAG, "Failed to register audio shutdown handler: %s", esp_err_to_name(sh));
        }
    }

    if (GPIO_OUTPUT_PA >= 0) {
        gpio_set_level(GPIO_OUTPUT_PA, 1);
    }
    ESP_LOGI(TAG, "Audio Driver initialized successfully (volume=%u%%, PA=on)",
             (unsigned)s_current_volume);
    return ESP_OK;
}

void audio_deinit(void)
{
    if (!s_audio_initialized) {
        return;
    }

    if (GPIO_OUTPUT_PA >= 0) {
        gpio_set_level(GPIO_OUTPUT_PA, 0); // Disable PA
    }

    if (tx_handle) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
    }

    if (rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }

    if (s_es8311_i2c_handle) {
        i2c_master_bus_rm_device(s_es8311_i2c_handle);
        s_es8311_i2c_handle = NULL;
    }

    s_audio_initialized = false;
    ESP_LOGI(TAG, "Audio Driver deinitialized");
}

esp_err_t audio_record_to_file(const char *filename, uint32_t duration_sec)
{
    if (!s_audio_initialized) {
        esp_err_t err = audio_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    FILE *f = fopen(filename, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file %s for writing", filename);
        return ESP_FAIL;
    }

    // Write placeholder WAV header
    wav_header_t header;
    memcpy(header.chunk_id, "RIFF", 4);
    header.chunk_size = 0; // Update later
    memcpy(header.format, "WAVE", 4);
    memcpy(header.subchunk1_id, "fmt ", 4);
    header.subchunk1_size = 16;
    header.audio_format = 1; // PCM
    header.num_channels = 2; // Stereo
    header.sample_rate = AUDIO_SAMPLE_RATE;
    header.bits_per_sample = 16;
    header.byte_rate = AUDIO_SAMPLE_RATE * 2 * 16 / 8;
    header.block_align = 2 * 16 / 8;
    memcpy(header.subchunk2_id, "data", 4);
    header.subchunk2_size = 0; // Update later

    if (fwrite(&header, 1, sizeof(header), f) != sizeof(header)) {
        ESP_LOGE(TAG, "Failed to write WAV header");
        fclose(f);
        return ESP_FAIL;
    }

    // Allocate read buffer
    const uint32_t buf_size = 4096;
    uint8_t *buf = malloc(buf_size);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for record buffer");
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    uint32_t bytes_to_record = duration_sec * header.byte_rate;
    uint32_t total_recorded_bytes = 0;
    size_t bytes_read = 0;

    ESP_LOGI(TAG, "Recording started... saving to %s (%d seconds)", filename, (int)duration_sec);

    while (total_recorded_bytes < bytes_to_record) {
        uint32_t chunk = bytes_to_record - total_recorded_bytes;
        if (chunk > buf_size) {
            chunk = buf_size;
        }

        esp_err_t ret = i2s_channel_read(rx_handle, buf, chunk, &bytes_read, pdMS_TO_TICKS(1000));
        if (ret == ESP_OK && bytes_read > 0) {
            /* 麦克风只占一个声道，写文件前复制到左右两声道，回放时不会掉 6 dB */
            int16_t *samples = (int16_t *)buf;
            size_t frames = bytes_read / (2 * sizeof(int16_t));
            for (size_t i = 0; i < frames; i++) {
                int16_t mic = mic_pick_channel(samples[2 * i], samples[2 * i + 1]);
                samples[2 * i]     = mic;
                samples[2 * i + 1] = mic;
            }
            size_t written = fwrite(buf, 1, bytes_read, f);
            if (written != bytes_read) {
                ESP_LOGE(TAG, "File write failed. Disk full?");
                break;
            }
            total_recorded_bytes += bytes_read;
        } else {
            ESP_LOGW(TAG, "I2S read timeout or error: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    free(buf);

    // Update WAV header with actual size
    header.chunk_size = total_recorded_bytes + sizeof(wav_header_t) - 8;
    header.subchunk2_size = total_recorded_bytes;

    if (fseek(f, 0, SEEK_SET) == 0) {
        fwrite(&header, 1, sizeof(header), f);
    }
    fclose(f);

    ESP_LOGI(TAG, "Recording stopped. Recorded %d bytes to %s", (int)total_recorded_bytes, filename);
    return ESP_OK;
}

esp_err_t audio_read_raw(void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms)
{
    if (dest == NULL || size == 0 || bytes_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_audio_initialized) {
        esp_err_t err = audio_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    return i2s_channel_read(rx_handle, dest, size, bytes_read, pdMS_TO_TICKS(timeout_ms));
}

void audio_flush_rx_warmup(uint32_t warmup_ms)
{
    if (!s_audio_initialized || rx_handle == NULL) {
        return;
    }
    char dummy[1024];
    size_t bytes_read = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t max_ticks = pdMS_TO_TICKS(warmup_ms > 0 ? warmup_ms : 50);
    while ((xTaskGetTickCount() - start) < max_ticks) {
        if (i2s_channel_read(rx_handle, dummy, sizeof(dummy), &bytes_read, pdMS_TO_TICKS(10)) != ESP_OK || bytes_read == 0) {
            break;
        }
    }
}

esp_err_t audio_record_mono_pcm(int16_t **out_buf, size_t *out_num_samples, uint32_t duration_sec)
{
    if (out_buf == NULL || out_num_samples == NULL || duration_sec == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_audio_initialized) {
        esp_err_t err = audio_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    // The I2S channel is configured as 16-bit stereo. We capture stereo frames
    // and down-mix to mono by keeping the left channel only, which is what the
    // iFlytek IAT service expects (16 kHz / 16-bit / mono PCM).
    const size_t mono_samples = (size_t)AUDIO_SAMPLE_RATE * duration_sec;
    int16_t *mono = heap_caps_malloc(mono_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mono == NULL) {
        mono = malloc(mono_samples * sizeof(int16_t));
    }
    if (mono == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for mono PCM buffer",
                 (unsigned)(mono_samples * sizeof(int16_t)));
        return ESP_ERR_NO_MEM;
    }

    const size_t stereo_buf_bytes = 4096; // 1024 stereo frames per read
    int16_t *stereo = malloc(stereo_buf_bytes);
    if (stereo == NULL) {
        free(mono);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Recording mono PCM for %u second(s)...", (unsigned)duration_sec);
    audio_flush_rx_warmup(50); // Flush initial 50ms warm-up noise

    size_t mono_written = 0;
    while (mono_written < mono_samples) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx_handle, stereo, stereo_buf_bytes, &bytes_read, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK || bytes_read == 0) {
            ESP_LOGW(TAG, "I2S read timeout or error: %s", esp_err_to_name(ret));
            continue;
        }

        size_t stereo_frames = bytes_read / (2 * sizeof(int16_t)); // L+R per frame
        for (size_t i = 0; i < stereo_frames && mono_written < mono_samples; i++) {
            mono[mono_written++] = mic_pick_channel(stereo[2 * i], stereo[2 * i + 1]);
        }
    }

    free(stereo);

    *out_buf = mono;
    *out_num_samples = mono_written;
    ESP_LOGI(TAG, "Recording done. Captured %u mono samples", (unsigned)mono_written);
    return ESP_OK;
}

esp_err_t audio_play_from_file(const char *filename)
{
    audio_play_clear_abort();

    if (!s_audio_initialized) {
        esp_err_t err = audio_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    FILE *f = fopen(filename, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file %s for reading", filename);
        return ESP_FAIL;
    }

    // Read WAV header
    wav_header_t header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header)) {
        ESP_LOGE(TAG, "Failed to read WAV header");
        fclose(f);
        return ESP_FAIL;
    }

    // Validate WAV header
    if (memcmp(header.chunk_id, "RIFF", 4) != 0 || memcmp(header.format, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAV file format");
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Playing WAV: sample_rate=%d, channels=%d, bits=%d",
             (int)header.sample_rate, (int)header.num_channels, (int)header.bits_per_sample);

    if (header.bits_per_sample != 16 || header.num_channels < 1 || header.num_channels > 2) {
        ESP_LOGE(TAG, "Unsupported WAV format (only 16-bit mono/stereo is supported)");
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    /* 必须按文件真实采样率重配 I2S 时钟。之前这里直接沿用当前时钟(默认 16 kHz)，
     * 播放非 16 kHz 的 WAV 会整体变速，听起来就是含糊不清的"沙哑"声。 */
    if (audio_play_pcm_begin(header.sample_rate) != ESP_OK) {
        fclose(f);
        return ESP_FAIL;
    }

    // Allocate play buffers
    const uint32_t read_buf_size = 2048;
    uint8_t *read_buf = malloc(read_buf_size);
    if (read_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate read buffer");
        audio_play_pcm_end();
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    // Since I2S is configured as stereo, 16-bit, if WAV is mono, we need to convert it on the fly.
    uint8_t *play_buf = read_buf;
    uint32_t play_buf_size = read_buf_size;
    uint8_t *mono_to_stereo_buf = NULL;

    if (header.num_channels == 1) {
        play_buf_size = read_buf_size * 2;
        mono_to_stereo_buf = malloc(play_buf_size);
        if (mono_to_stereo_buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate mono-to-stereo buffer");
            free(read_buf);
            audio_play_pcm_end();
            fclose(f);
            return ESP_ERR_NO_MEM;
        }
        play_buf = mono_to_stereo_buf;
    }

    size_t bytes_read = 0;
    size_t bytes_written = 0;
    ESP_LOGI(TAG, "Playback started...");

    while ((bytes_read = fread(read_buf, 1, read_buf_size, f)) > 0) {
        if (s_play_abort_flag) {
            ESP_LOGI(TAG, "Playback aborted by flag");
            break;
        }
        uint32_t bytes_to_write = bytes_read;

        /* ES8311 是单声道 codec，DAC 只取其中一个声道。这里把同一路数据复制到
         * 左右两个 slot，无论 codec 取哪一侧都能拿到完整信号；之前把右声道写 0
         * (以及立体声时直接丢弃右声道) 会造成半边通道无数据、音量与音质异常。 */
        if (header.num_channels == 1) {
            const int16_t *mono_samples = (const int16_t *)read_buf;
            int16_t *stereo_samples = (int16_t *)mono_to_stereo_buf;
            uint32_t num_samples = bytes_read / 2;
            for (uint32_t i = 0; i < num_samples; i++) {
                stereo_samples[2 * i]     = mono_samples[i];
                stereo_samples[2 * i + 1] = mono_samples[i];
            }
            bytes_to_write = num_samples * 2 * sizeof(int16_t);
        } else {
            /* 立体声下混为单声道后再复制到左右 slot */
            int16_t *stereo_samples = (int16_t *)read_buf;
            uint32_t frames = bytes_read / 4;
            for (uint32_t i = 0; i < frames; i++) {
                int32_t mix = ((int32_t)stereo_samples[2 * i] + (int32_t)stereo_samples[2 * i + 1]) / 2;
                stereo_samples[2 * i]     = (int16_t)mix;
                stereo_samples[2 * i + 1] = (int16_t)mix;
            }
            bytes_to_write = frames * 2 * sizeof(int16_t);
        }

        esp_err_t ret = i2s_channel_write(tx_handle, play_buf, bytes_to_write, &bytes_written, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
            break;
        }
        if (bytes_written != bytes_to_write) {
            /* 写不完会在音频流里留下空洞，听起来就是断续/沙哑 */
            ESP_LOGW(TAG, "I2S short write: %u/%u bytes",
                     (unsigned)bytes_written, (unsigned)bytes_to_write);
        }
    }

    free(read_buf);
    if (mono_to_stereo_buf) {
        free(mono_to_stereo_buf);
    }
    audio_play_pcm_end();
    fclose(f);

    ESP_LOGI(TAG, "Playback finished.");
    return ESP_OK;
}

esp_err_t audio_record_to_mem(uint8_t **out_buf, size_t *out_len, uint32_t duration_sec)
{
    if (out_buf == NULL || out_len == NULL || duration_sec == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_audio_initialized) {
        esp_err_t err = audio_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    wav_header_t header;
    memcpy(header.chunk_id, "RIFF", 4);
    header.chunk_size = 0; // Update later
    memcpy(header.format, "WAVE", 4);
    memcpy(header.subchunk1_id, "fmt ", 4);
    header.subchunk1_size = 16;
    header.audio_format = 1; // PCM
    header.num_channels = 2; // Stereo
    header.sample_rate = AUDIO_SAMPLE_RATE;
    header.bits_per_sample = 16;
    header.byte_rate = AUDIO_SAMPLE_RATE * 2 * 16 / 8;
    header.block_align = 2 * 16 / 8;
    memcpy(header.subchunk2_id, "data", 4);
    header.subchunk2_size = 0; // Update later

    uint32_t data_bytes_to_record = duration_sec * header.byte_rate;
    uint32_t total_size = sizeof(wav_header_t) + data_bytes_to_record;

    uint8_t *buf = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = malloc(total_size);
    }
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for record buffer");
        return ESP_ERR_NO_MEM;
    }

    // Write template header to buffer
    memcpy(buf, &header, sizeof(wav_header_t));

    uint32_t total_recorded_bytes = 0;
    size_t bytes_read = 0;
    uint8_t *write_ptr = buf + sizeof(wav_header_t);

    ESP_LOGI(TAG, "Recording started to memory... (%d seconds)", (int)duration_sec);

    while (total_recorded_bytes < data_bytes_to_record) {
        uint32_t chunk = data_bytes_to_record - total_recorded_bytes;
        if (chunk > 4096) {
            chunk = 4096;
        }

        esp_err_t ret = i2s_channel_read(rx_handle, write_ptr + total_recorded_bytes, chunk, &bytes_read, pdMS_TO_TICKS(1000));
        if (ret == ESP_OK && bytes_read > 0) {
            // Put the microphone signal on both channels of the recorded stereo frame.
            int16_t *samples = (int16_t *)(write_ptr + total_recorded_bytes);
            size_t frames = bytes_read / (2 * sizeof(int16_t));
            for (size_t i = 0; i < frames; i++) {
                int16_t mic = mic_pick_channel(samples[2 * i], samples[2 * i + 1]);
                samples[2 * i]     = mic;
                samples[2 * i + 1] = mic;
            }
            total_recorded_bytes += bytes_read;
        } else {
            ESP_LOGW(TAG, "I2S read timeout or error: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // Update WAV header inside buffer with actual sizes
    wav_header_t *final_hdr = (wav_header_t *)buf;
    final_hdr->chunk_size = total_recorded_bytes + sizeof(wav_header_t) - 8;
    final_hdr->subchunk2_size = total_recorded_bytes;

    *out_buf = buf;
    *out_len = sizeof(wav_header_t) + total_recorded_bytes;

    ESP_LOGI(TAG, "Recording stopped. Recorded %d bytes to memory", (int)total_recorded_bytes);
    return ESP_OK;
}

esp_err_t audio_play_from_mem(const uint8_t *buf, size_t len)
{
    audio_play_clear_abort();

    if (buf == NULL || len < sizeof(wav_header_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_audio_initialized) {
        esp_err_t err = audio_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    wav_header_t header;
    memcpy(&header, buf, sizeof(wav_header_t));

    // Validate WAV header
    if (memcmp(header.chunk_id, "RIFF", 4) != 0 || memcmp(header.format, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAV memory format");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Playing WAV from memory: sample_rate=%d, channels=%d, bits=%d",
             (int)header.sample_rate, (int)header.num_channels, (int)header.bits_per_sample);

    const uint8_t *data_ptr = buf + sizeof(wav_header_t);
    size_t data_len = len - sizeof(wav_header_t);

    const uint32_t write_chunk_size = 2048;
    uint8_t *mono_to_stereo_buf = NULL;
    uint8_t *play_buf = NULL;

    if (header.num_channels == 1) {
        mono_to_stereo_buf = malloc(write_chunk_size * 2);
        if (mono_to_stereo_buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate mono-to-stereo buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    size_t offset = 0;
    size_t bytes_written = 0;
    ESP_LOGI(TAG, "Playback from memory started...");

    while (offset < data_len) {
        if (s_play_abort_flag) {
            ESP_LOGI(TAG, "Playback aborted by flag");
            break;
        }
        uint32_t chunk = data_len - offset;
        if (chunk > write_chunk_size) {
            chunk = write_chunk_size;
        }

        uint32_t bytes_to_write = chunk;
        if (header.num_channels == 1) {
            // Convert mono to stereo (16-bit)
            int16_t *mono_samples = (int16_t *)(data_ptr + offset);
            int16_t *stereo_samples = (int16_t *)mono_to_stereo_buf;
            uint32_t num_samples = chunk / 2;
            for (uint32_t i = 0; i < num_samples; i++) {
                stereo_samples[2 * i] = mono_samples[i];
                stereo_samples[2 * i + 1] = mono_samples[i];
            }
            bytes_to_write = chunk * 2;
            play_buf = mono_to_stereo_buf;
        } else {
            play_buf = (uint8_t *)(data_ptr + offset);
        }

        esp_err_t ret = i2s_channel_write(tx_handle, play_buf, bytes_to_write, &bytes_written, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
            break;
        }
        offset += chunk;
    }

    if (mono_to_stereo_buf) {
        free(mono_to_stereo_buf);
    }

    ESP_LOGI(TAG, "Playback from memory finished.");
    return ESP_OK;
}

esp_err_t audio_play_pcm_begin(uint32_t sample_rate)
{
    if (!s_audio_initialized) {
        esp_err_t err = audio_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (sample_rate == 0) {
        sample_rate = AUDIO_SAMPLE_RATE;
    }

    i2s_channel_disable(tx_handle);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    clk.mclk_multiple = AUDIO_MCLK_MULTIPLE;
    esp_err_t err = i2s_channel_reconfig_std_clock(tx_handle, &clk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconfig I2S clock to %u Hz failed: %s",
                 (unsigned)sample_rate, esp_err_to_name(err));
    }
    i2s_channel_enable(tx_handle);
    ESP_LOGI(TAG, "PCM playback begin @ %u Hz", (unsigned)sample_rate);
    return err;
}

esp_err_t audio_play_pcm_write(const int16_t *pcm, size_t num_samples, int channels)
{
    if (!pcm || num_samples == 0) {
        return ESP_OK;
    }
    size_t written = 0;

    if (channels == 1) {
        /* Duplicate mono sample to both Left and Right I2S slots. */
        static int16_t stereo[1024];
        const size_t CHUNK = 512; /* mono samples per pass */
        size_t i = 0;
        while (i < num_samples) {
            size_t n = num_samples - i;
            if (n > CHUNK) n = CHUNK;
            for (size_t k = 0; k < n; k++) {
                stereo[2 * k]     = pcm[i + k]; // Left channel
                stereo[2 * k + 1] = pcm[i + k]; // Right channel (duplicate for ES8311 DAC)
            }
            esp_err_t err = i2s_channel_write(tx_handle, stereo, n * 2 * sizeof(int16_t),
                                              &written, pdMS_TO_TICKS(1000));
            if (err != ESP_OK) {
                return err;
            }
            i += n;
        }
    } else {
        /* Play stereo PCM to both Left and Right I2S slots. */
        static int16_t stereo_play[1024];
        const size_t CHUNK = 512; /* stereo samples (frames) per pass */
        size_t i = 0;
        while (i < num_samples) {
            size_t n = num_samples - i;
            if (n > CHUNK) n = CHUNK;
            for (size_t k = 0; k < n; k++) {
                stereo_play[2 * k]     = pcm[2 * (i + k)];     // Left channel
                stereo_play[2 * k + 1] = pcm[2 * (i + k) + 1]; // Right channel
            }
            esp_err_t err = i2s_channel_write(tx_handle, stereo_play, n * 2 * sizeof(int16_t),
                                              &written, pdMS_TO_TICKS(1000));
            if (err != ESP_OK) {
                return err;
            }
            i += n;
        }
    }
    return ESP_OK;
}

void audio_play_pcm_end(void)
{
    i2s_channel_disable(tx_handle);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE);
    clk.mclk_multiple = AUDIO_MCLK_MULTIPLE;
    i2s_channel_reconfig_std_clock(tx_handle, &clk);
    i2s_channel_enable(tx_handle);
    ESP_LOGI(TAG, "PCM playback end, clock restored to %d Hz", AUDIO_SAMPLE_RATE);
}

typedef struct {
    int16_t *data;
    size_t num_samples;
    bool is_last;
} audio_queue_item_t;

static QueueHandle_t s_audio_queue = NULL;
static SemaphoreHandle_t s_audio_queue_done_sem = NULL;
static TaskHandle_t s_audio_consumer_task_handle = NULL;

static void audio_queue_consumer_task(void *arg)
{
    uint32_t sample_rate = (uint32_t)(uintptr_t)arg;
    bool pcm_started = false;
    bool finished = false;

    /* Pre-buffering: wait until queue has 2-3 chunks OR a chunk with is_last == true */
    TickType_t pre_start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - pre_start) < pdMS_TO_TICKS(1000)) {
        if (audio_play_is_aborted()) {
            break;
        }
        UBaseType_t count = s_audio_queue ? uxQueueMessagesWaiting(s_audio_queue) : 0;
        if (count >= 2) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    while (!finished) {
        if (audio_play_is_aborted()) {
            ESP_LOGI(TAG, "Audio Queue consumer aborted by play_abort flag");
            break;
        }

        audio_queue_item_t item;
        /* Wait up to 3000ms for next chunk (Jitter buffer timeout per Requirement 1.5/1.11) */
        if (s_audio_queue && xQueueReceive(s_audio_queue, &item, pdMS_TO_TICKS(3000)) == pdTRUE) {
            if (item.data && item.num_samples > 0) {
                if (!pcm_started) {
                    audio_play_pcm_begin(sample_rate);
                    pcm_started = true;
                }
                if (!audio_play_is_aborted()) {
                    audio_play_pcm_write(item.data, item.num_samples, 1);
                }
                free(item.data);
            }
            if (item.is_last) {
                finished = true;
            }
        } else {
            ESP_LOGW(TAG, "Audio Queue consumer timed out waiting for audio chunk");
            break;
        }
    }

    /* Drain any remaining queue items if aborted */
    if (s_audio_queue) {
        audio_queue_item_t item;
        while (xQueueReceive(s_audio_queue, &item, 0) == pdTRUE) {
            if (item.data) free(item.data);
        }
    }

    if (pcm_started) {
        audio_play_pcm_end();
    }

    if (s_audio_queue_done_sem) {
        xSemaphoreGive(s_audio_queue_done_sem);
    }
    s_audio_consumer_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_queue_start(uint32_t sample_rate)
{
    audio_play_clear_abort();

    if (s_audio_queue) {
        vQueueDelete(s_audio_queue);
        s_audio_queue = NULL;
    }
    if (s_audio_queue_done_sem) {
        vSemaphoreDelete(s_audio_queue_done_sem);
        s_audio_queue_done_sem = NULL;
    }

    s_audio_queue = xQueueCreate(32, sizeof(audio_queue_item_t)); // Max 32 chunks per Requirement 1.12
    s_audio_queue_done_sem = xSemaphoreCreateBinary();

    if (!s_audio_queue || !s_audio_queue_done_sem) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(audio_queue_consumer_task, "audio_consumer", 4096,
                                 (void *)(uintptr_t)sample_rate, 5, &s_audio_consumer_task_handle);
    if (ret != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t audio_queue_push(const int16_t *pcm, size_t num_samples, bool is_last)
{
    if (!s_audio_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (audio_play_is_aborted()) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_queue_item_t item = {0};
    item.is_last = is_last;
    if (pcm && num_samples > 0) {
        item.num_samples = num_samples;
        item.data = malloc(num_samples * sizeof(int16_t));
        if (!item.data) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(item.data, pcm, num_samples * sizeof(int16_t));
    }

    /* Requirement 1.12: WHILE Audio_Queue holds 32 chunks, block further enqueue up to 3000ms */
    if (xQueueSend(s_audio_queue, &item, pdMS_TO_TICKS(3000)) != pdTRUE) {
        if (item.data) free(item.data);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void audio_queue_finish(void)
{
    if (s_audio_queue && !audio_play_is_aborted()) {
        audio_queue_push(NULL, 0, true);
    }
}

esp_err_t audio_queue_wait_done(uint32_t timeout_ms)
{
    if (!s_audio_queue_done_sem) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_audio_queue_done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

int cmd_audio_record(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: audio_record <filename> [duration_sec]\n");
        printf("Example: audio_record rec.wav 5\n");
        return 1;
    }

    char filepath[256];
    if (argv[1][0] == '/') {
        snprintf(filepath, sizeof(filepath), "%s", argv[1]);
    } else {
        snprintf(filepath, sizeof(filepath), "/spiffs/%s", argv[1]);
    }

    uint32_t duration = 5; // Default 5 seconds
    if (argc >= 3) {
        duration = atoi(argv[2]);
        if (duration == 0 || duration > 3600) {
            printf("Invalid duration (1 to 3600 seconds)\n");
            return 1;
        }
    }

    esp_err_t err = audio_record_to_file(filepath, duration);
    if (err != ESP_OK) {
        printf("Recording failed with error: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Recorded successfully to %s\n", filepath);
    return 0;
}

esp_err_t audio_set_volume(uint8_t volume)
{
    if (!s_audio_initialized) {
        esp_err_t err = audio_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (volume > 100) {
        volume = 100;
    }

    /* Keep PA enabled whenever we raise playback level.
     * 外接模组的 PA-EN 硬件常开，GPIO_OUTPUT_PA = -1，必须判断后再调用，
     * 否则 gpio_set_level(-1) 会打印 "GPIO output gpio_num error"。 */
    if (GPIO_OUTPUT_PA >= 0) {
        gpio_set_level(GPIO_OUTPUT_PA, 1);
    }
    /* Clear soft-mute on DAC. */
    esp_err_t err = es8311_write_reg(ES8311_REG_DAC_MUTE, 0x00);
    if (err != ESP_OK) {
        return err;
    }

    /* Official ES8311 mapping used by espressif/es8311 driver. */
    uint8_t reg_val = (volume == 0) ? 0 : (uint8_t)((volume * 256 / 100) - 1);
    err = es8311_write_reg(ES8311_REG_DAC_VOLUME, reg_val);
    if (err == ESP_OK) {
        s_current_volume = volume;
        ESP_LOGI(TAG, "Speaker volume -> %u%% (DAC reg 0x32=0x%02X)",
                 (unsigned)volume, reg_val);
        nvs_handle_t handle;
        if (nvs_open(AUDIO_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_u8(handle, AUDIO_NVS_KEY_VOL, volume);
            nvs_commit(handle);
            nvs_close(handle);
        }
    }
    return err;
}

uint8_t audio_get_volume(void)
{
    return s_current_volume;
}

int cmd_set_volume(int argc, char **argv)
{
    if (argc < 2) {
        printf("Current speaker volume: %u%%\n", (unsigned)audio_get_volume());
        printf("Usage: set_volume <0-100>\n");
        return 0;
    }

    int vol = atoi(argv[1]);
    if (vol < 0 || vol > 100) {
        printf("Invalid volume (0 to 100)\n");
        return 1;
    }

    esp_err_t err = audio_set_volume((uint8_t)vol);
    if (err != ESP_OK) {
        printf("Failed to set volume: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Speaker volume set to %d%% and saved to NVS\n", vol);
    return 0;
}

int cmd_audio_play(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: audio_play <filename> [volume_percent]\n");
        printf("Example: audio_play rec.wav 80\n");
        return 1;
    }

    char filepath[256];
    if (argv[1][0] == '/') {
        snprintf(filepath, sizeof(filepath), "%s", argv[1]);
    } else {
        snprintf(filepath, sizeof(filepath), "/spiffs/%s", argv[1]);
    }

    if (argc >= 3) {
        int vol = atoi(argv[2]);
        if (vol < 0 || vol > 100) {
            printf("Invalid volume (0 to 100)\n");
            return 1;
        }
        esp_err_t err = audio_set_volume((uint8_t)vol);
        if (err != ESP_OK) {
            printf("Failed to set volume: %s\n", esp_err_to_name(err));
            return 1;
        }
    }

    esp_err_t err = audio_play_from_file(filepath);
    if (err != ESP_OK) {
        printf("Playback failed with error: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Played successfully from %s\n", filepath);
    return 0;
}

esp_err_t audio_es8311_dump_registers(void)
{
    static const uint8_t regs[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
        0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1C, 0x31, 0x32, 0x37,
        0xFD, 0xFE, 0xFF
    };
    printf("\n=== ES8311 Register Dump (Logic Analyzer Test @ 20kHz) ===\n");
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint8_t val = 0;
        esp_err_t err = audio_es8311_read_reg(regs[i], &val);
        if (err == ESP_OK) {
            printf("Reg 0x%02X: 0x%02X\n", regs[i], val);
        } else {
            printf("Reg 0x%02X: READ ERROR (%s / NACK)\n", regs[i], esp_err_to_name(err));
        }
    }
    printf("=========================================================\n\n");
    return ESP_OK;
}

int cmd_es8311_read(int argc, char **argv)
{
    if (argc < 2) {
        printf("Executing full ES8311 register dump (Logic Analyzer test)...\n");
        audio_es8311_dump_registers();
        printf("Usage: es8311_read [reg_hex_or_dec] (e.g. es8311_read 0x00 or es8311_read 50)\n");
        return 0;
    }

    uint32_t reg = strtoul(argv[1], NULL, 0);
    if (reg > 0xFF) {
        printf("Invalid register address: %s (must be 0x00 - 0xFF)\n", argv[1]);
        return 1;
    }

    uint8_t val = 0;
    esp_err_t err = audio_es8311_read_reg((uint8_t)reg, &val);
    if (err == ESP_OK) {
        printf("ES8311 Reg 0x%02X = 0x%02X (%u)\n", (unsigned)reg, val, val);
    } else {
        printf("Failed to read ES8311 Reg 0x%02X: %s (NACK/I2C Error)\n", (unsigned)reg, esp_err_to_name(err));
    }
    return (err == ESP_OK) ? 0 : 1;
}
