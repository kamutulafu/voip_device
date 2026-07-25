> 记录日期：2026-07-25
> 关联模块：ESP32-P4 设备端固件 `main/audio_driver.c`（ES8311 codec + I2S 驱动）
> 最终结果：定位到外接 ES8311 模组后扬声器完全无声的根因（`REG13` 输出驱动被关闭 + 3-Wire 模式下 `REG02` 分频/倍频算错 + I2S slot 宽度只有 32×Fs），已修复并编译通过，待上板验证。

## 一、问题背景

上一版固件（`git HEAD = 38e77cc`）音频收发功能正常，使用的是 ESP32-P4 官方开发板板载 ES8311：

- MCLK=GPIO13、BCK=GPIO12、WS=GPIO10、DOUT=GPIO9、DIN=GPIO11
- I2C：SDA=GPIO7、SCL=GPIO8，100 kHz
- PA 使能：GPIO53

本次改为**外接 ES8311 模组**，代码同步做了如下修改（工作区未提交改动）：

- I2S：MCLK **不接**（`I2S_GPIO_UNUSED`，改用 3-Wire 模式，由 BCK 供给内部 MCLK）、BCK=GPIO21、WS=GPIO24、DOUT=GPIO33、DIN=GPIO25
- I2C：SDA=GPIO2、SCL=GPIO3，速率降到 20 kHz
- PA 使能：`GPIO_OUTPUT_PA = -1`（模组 PA-EN 已硬件上拉）
- ES8311 寄存器序列同步调整
- 新增 `es8311_read` 控制台命令用于寄存器 dump

**现象：播放声音时扬声器一直没有声音。**（录音/其他流程未报错）

---

## 二、排查方法

没有直接猜，而是用 `git diff` 把"能出声的版本"和"当前版本"的 ES8311 初始化序列逐寄存器对比，再用仓库里已有的官方驱动作为标准答案交叉验证：

- `managed_components/espressif__es8311/es8311.c`（项目里已经存在这个官方组件）
- esphome 的 `es8311.cpp` / `es8311_const.h`（第二来源，编码约定与官方一致）

对照点主要是两处：官方的 `coeff_div[]` 时钟系数表，以及 `es8311_init()` 里的模拟通路上电顺序。

---

## 三、根因（三处，按影响排序）

### 1. `REG13` 从 `0x10` 改成了 `0x00` —— 直接静音

```c
// 改动后（错误）
ret |= es8311_write_reg(0x13, 0x00); // 注释写的是 "Power UP all analog output drivers"
```

注释与实际含义相反。官方驱动 `es8311.c:343` 和 esphome 都明确写 `0x10`，注释为 *Enable output to HP drive*：

```c
es8311_write_reg(dev, ES8311_SYSTEM_REG13, 0x10); // Enable output to HP drive - NOT default
```

写 `0x00` 等于关掉 DAC → 输出驱动的通路，扬声器完全无声。这是本次无声的直接原因。

### 2. 3-Wire（BCK 当 MCLK）模式下 `REG02` 分频/倍频算错

ES8311 内部需要 **256×Fs** 的主时钟（16 kHz → 4.096 MHz）。

| 版本 | MCLK 来源 | REG02 | 实际内部时钟 |
|---|---|---|---|
| 旧（正常） | MCLK 引脚 6.144 MHz | `0x48` = pre_div 3 / pre_multi ×2 | 6.144M ÷3 ×2 = 4.096 MHz = 256×Fs ✅ |
| 新（无声） | BCK 512 kHz | `0x08` = pre_div 1 / pre_multi ×2 | 512k ×2 = 1.024 MHz = 64×Fs ❌ |

`REG02` 的位定义（官方驱动写法）：

```c
regv |= (pre_div - 1) << 5;      // bit[7:5] 预分频
regv |= pre_multi << 3;          // bit[4:3] 预倍频编码：0x00=×1, 0x01=×2, 0x02=×4, 0x03=×8
```

即使输出通路打开，内部时钟差 4 倍，DAC 也不会正常出声。

### 3. I2S slot 宽度被固定成 16 bit，BCK 只有 32×Fs

```c
std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT; // BCK = 16k×16×2 = 512 kHz
```

官方 `coeff_div[]` 表中 16 kHz 支持的**最低 MCLK 是 1.024 MHz（64×Fs）**，`512000 / 16000` 这一组根本不在表里——用官方 API 走这个配置会直接返回 `ESP_ERR_INVALID_ARG`。3-Wire 模式下 MCLK 就是 BCK，所以 slot 必须给到 32 bit。

参考表项（`managed_components/espressif__es8311/es8311.c`）：

```
/*  mclk     rate   pre_div  mult  adc_div dac_div fs_mode lrch  lrcl  bckdiv  osr */
{1024000, 16000,   0x01,   0x02,   0x01,   0x01,   0x00,  0x00, 0xff,  0x04, 0x10, 0x10}
```

---

## 四、修复内容（`main/audio_driver.c`）

1. `REG13` 恢复 `0x10`（Enable output to HP drive）
2. `REG02` 改为 `0x10`：pre_div=1、pre_multi=×4 → BCK 1.024 MHz ×4 = 4.096 MHz = 256×Fs，与官方表项 `{1024000, 16000, 0x01, 0x02, …}` 完全一致
3. I2S slot 宽度改为 `I2S_SLOT_BIT_WIDTH_32BIT`：BCK = Fs×64（16 kHz → 1.024 MHz）。数据位宽仍是 16 bit，硬件自动在低 16 bit 补 0，**DMA 缓冲区格式不变**，所以录音/播放的声道拆分与混音代码无需改动
4. 把 `REG00 = 0x80`（CSM 上电）移到时钟/格式/模拟通路配置**之后**再写一次，与官方 `es8311_init()` 顺序一致
5. `audio_deinit()` 补上 PA 引脚判断，`GPIO_OUTPUT_PA = -1` 时不再调用 `gpio_set_level(-1, 0)`

关键代码：

```c
/* 3-Wire：BCK 作内部 MCLK，BCK = Fs*32*2 = 64 x Fs */
ret |= es8311_write_reg(0x01, 0xBF); // bit7=1 选 BCK 作 MCLK 源，全部时钟使能
ret |= es8311_write_reg(0x02, 0x10); // pre_div=1, pre_multi=x4 -> 256 x Fs
...
ret |= es8311_write_reg(0x13, 0x10); // Enable output to HP drive（写 0x00 会完全没声音）
...
ret |= es8311_write_reg(0x00, 0x80); // 配置完成后再 CSM 上电

std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
```

**附带好处**：3-Wire 模式下 BCK 恒为 64×Fs，MCLK/Fs 比例与采样率无关，因此 `audio_play_pcm_begin()` 切到 8 k（VoIP）/ 24 k（TTS）时 ES8311 寄存器**不需要重配**。

---

## 五、验证

- `idf.py build` 通过（ESP-IDF v5.5，esp32p4，`HowToCreateProject.bin` 0x1783d0 bytes）
- 上板验证待做

上板后用新增的控制台命令 `es8311_read` dump 寄存器，正常应为：

| 寄存器 | 期望值 | 含义 |
|---|---|---|
| 0x00 | 0x80 | CSM 上电、Slave 模式 |
| 0x01 | 0xBF | BCK 作 MCLK 源 + 全时钟使能 |
| 0x02 | 0x10 | pre_div 1 / pre_multi ×4 |
| 0x03 | 0x10 | fs_mode 0, adc_osr 16 |
| 0x04 | 0x10 | dac_osr 16 |
| 0x09 | 0x0C | SDP In 16 bit |
| 0x0A | 0x0C | SDP Out 16 bit |
| 0x13 | 0x10 | 输出驱动使能 |
| 0x31 | 0x00 | DAC 不静音 |
| 0x32 | 0x8B | 音量 55%（55×256/100−1 = 139） |

分支判断：

- 读回全 `0xFF` 或 NACK → 问题在 I2C 走线/器件地址（部分模组是 `0x19`，不是 `0x18`），不是寄存器配置
- 读回值都正确但仍无声 → 查 PA-EN 是否真被拉高、DOUT(GPIO33) 是否接到模组 **DSDIN** 而不是 ASDOUT、以及扬声器/功放供电

---

## 六、经验记录

- ES8311 的"无声"绝大多数是 `REG13`（输出驱动）与时钟系数两类问题，先 dump 寄存器再猜比改代码快得多。
- 换硬件形态（板载 → 外接、有 MCLK → 3-Wire）时，**时钟系数必须整套重算**，不能只改引脚号：`REG02` 的倍频、I2S slot 宽度、MCLK/Fs 比例三者是绑定的。
- 项目里已经引入了 `managed_components/espressif__es8311`，它的 `coeff_div[]` 表就是标准答案；自己手写寄存器序列时应逐项对照该表，而不是凭注释。
- 寄存器注释写错（把 `0x00` 注释成"上电所有输出驱动"）会把后续排查带偏，注释应引用数据手册/官方驱动的原文表述。
