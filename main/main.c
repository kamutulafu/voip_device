#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "app_build.h"
#include "spiffs_storage.h"
#include "voice_flow.h"
#include "wifi_manager.h"

#if APP_BUILD_DEBUG
#include "console.h"
#include "camera_capture.h"
#include "uvc_stream.h"
#include "audio_driver.h"
#include "asr_xfyun.h"
#include "tts_xfyun.h"
#include "voip_client.h"
#include "api_test.h"

#include "dialogue.h"
#include "time_sync.h"

int cmd_zmodem_send(int argc, char **argv);
int cmd_zmodem_recv(int argc, char **argv);
#endif

static const char *TAG = "main";

#if APP_BUILD_DEBUG
static void register_debug_console_commands(void)
{
    // Register UVC commands
    const esp_console_cmd_t cmd_uvc_init_cfg = {
        .command = "uvc_init",
        .help = "Initialize the I2C bus, esp_video pipeline, and USB UVC stack",
        .hint = NULL,
        .func = &cmd_uvc_init,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_uvc_init_cfg));

    const esp_console_cmd_t cmd_uvc_status_cfg = {
        .command = "uvc_status",
        .help = "Get the current initialization and streaming status of the UVC camera",
        .hint = NULL,
        .func = &cmd_uvc_status,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_uvc_status_cfg));

    const esp_console_cmd_t cmd_camera_test_pattern_cfg = {
        .command = "camera_test_pattern",
        .help = "Enable or disable the camera test pattern (color bar)",
        .hint = "<1|0|on|off>",
        .func = &cmd_camera_test_pattern,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_camera_test_pattern_cfg));

    // Register Camera Capture command
    const esp_console_cmd_t cmd_camera_capture_cfg = {
        .command = "camera_capture",
        .help = "Capture a photo from the camera and save it to the SPIFFS filesystem",
        .hint = "[filepath]",
        .func = &cmd_camera_capture,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_camera_capture_cfg));

    // Register Zmodem Send command
    const esp_console_cmd_t cmd_zmodem_send_cfg = {
        .command = "zmodem_send",
        .help = "Send a file using Zmodem protocol",
        .hint = "<filepath>",
        .func = &cmd_zmodem_send,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_zmodem_send_cfg));

    // Register Zmodem Receive command
    const esp_console_cmd_t cmd_zmodem_recv_cfg = {
        .command = "zmodem_recv",
        .help = "Receive files using Zmodem protocol",
        .hint = "[directory]",
        .func = &cmd_zmodem_recv,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_zmodem_recv_cfg));

    // Register ls command
    const esp_console_cmd_t cmd_ls_cfg = {
        .command = "ls",
        .help = "List files in the SPIFFS filesystem",
        .hint = "[directory]",
        .func = &cmd_ls,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_ls_cfg));

    // Register rm command
    const esp_console_cmd_t cmd_rm_cfg = {
        .command = "rm",
        .help = "Remove a file or all files in SPIFFS",
        .hint = "<filename|all>",
        .func = &cmd_rm,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_rm_cfg));

    // Register audio record command
    const esp_console_cmd_t cmd_audio_record_cfg = {
        .command = "audio_record",
        .help = "Record audio from microphone and save to SPIFFS",
        .hint = "<filename> [duration_sec]",
        .func = &cmd_audio_record,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_audio_record_cfg));

    // Register audio play command
    const esp_console_cmd_t cmd_audio_play_cfg = {
        .command = "audio_play",
        .help = "Play audio from SPIFFS using the speaker",
        .hint = "<filename> [volume_percent]",
        .func = &cmd_audio_play,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_audio_play_cfg));

    // Register speech recognition (ASR) test command
    const esp_console_cmd_t cmd_asr_test_cfg = {
        .command = "asr_test",
        .help = "Record from the microphone and convert speech to text using iFlytek (requires WiFi)",
        .hint = "[duration_sec]",
        .func = &cmd_asr_test,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_asr_test_cfg));

    // Register text-to-speech (TTS) test command
    const esp_console_cmd_t cmd_tts_test_cfg = {
        .command = "tts_test",
        .help = "Synthesize text to speech using iFlytek and play it on the speaker (requires WiFi)",
        .hint = "<text...>",
        .func = &cmd_tts_test,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_tts_test_cfg));

    // Register text-to-speech (TTS) streaming playback test command
    const esp_console_cmd_t cmd_tts_test_stream_cfg = {
        .command = "tts_test_stream",
        .help = "Stream TTS playback test (plays as audio arrives). Defaults to '你好，这是TTS功能测试'",
        .hint = "[text...]",
        .func = &cmd_tts_test_stream,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_tts_test_stream_cfg));

    // Register WeChat Cloud VoIP test call command
    const esp_console_cmd_t cmd_voip_call_cfg = {
        .command = "voip_call",
        .help = "Place a WeChat Cloud VoIP call (snTicket auto-fetched; uses built-in defaults)",
        .hint = "[openid] [device_id] [model_id] [appid] [payload]",
        .func = &cmd_voip_call,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_voip_call_cfg));

    // Register wifi_join command
    const esp_console_cmd_t cmd_wifi_join_cfg = {
        .command = "wifi_join",
        .help = "Join a WiFi network",
        .hint = "<ssid> <password>",
        .func = &cmd_wifi_join,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_wifi_join_cfg));

    // Register API test command
    register_api_test_cmd();

    // Console equivalent of the physical wake button
    register_voice_wake_cmd();

    // Register voice_update command
    const esp_console_cmd_t cmd_voice_update_cfg = {
        .command = "voice_update",
        .help = "Update local voice files by synthesizing and saving them to SPIFFS",
        .hint = NULL,
        .func = &cmd_voice_update,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_voice_update_cfg));

    // Register date command (debug helper to verify boot time sync)
    const esp_console_cmd_t cmd_date_cfg = {
        .command = "date",
        .help = "Print current system time (UTC) to verify boot time sync",
        .hint = NULL,
        .func = &cmd_date,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_date_cfg));
}
#endif /* APP_BUILD_DEBUG */

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Netif and Default Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize SPIFFS
    ESP_ERROR_CHECK(init_spiffs());

    ESP_LOGI(TAG, "Build type: %s", APP_BUILD_TYPE_STR);

#if APP_BUILD_DEBUG
    ESP_LOGI(TAG, "Initializing console (msh)...");
    ESP_ERROR_CHECK(console_init());
    register_debug_console_commands();
#endif

    // Voice subsystem + GPIO45 wake button (both Debug and Release)
    ESP_ERROR_CHECK(voice_flow_init());

    // Trigger auto-connection to saved WiFi in background
    wifi_manager_start_autoconnect();

#if APP_BUILD_DEBUG
    ESP_LOGI(TAG, "Starting console REPL...");
    ESP_ERROR_CHECK(console_start());
#else
    ESP_LOGI(TAG, "Release: msh console disabled (use wake button GPIO45)");
#endif
}
