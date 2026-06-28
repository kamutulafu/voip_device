# 摄像头彩带测试功能 (Camera Color Bar Test Pattern)

本项目基于 ESP32-P4 平台，使用 OV5647 摄像头传感器。为了方便产线测试及故障排查，我们添加了控制摄像头彩带（Test Pattern / Color Bar）测试的功能。

---

## 1. 技术背景与架构

OV5647 传感器内部集成了测试图案发生器。通过控制 ISP 控制寄存器 `0x503D`，可以开启或关闭彩带、彩格等测试图案：
* **寄存器地址**: `0x503D` (ISPCTRL3D)
* **位控制**: 
  * `Bit[7]`: 测试图案使能位 (1: 使能, 0: 关闭)
  * `Bit[1:0]`: 测试图案类型 (00: 八色彩条/彩带)

在 ESP-IDF 与 `esp_video` 框架中，此功能由标准 V4L2 控制指令 `V4L2_CID_TEST_PATTERN` 承载，底层会自动映射到摄像头驱动中的 `ESP_CAM_SENSOR_IOC_S_TEST_PATTERN` IOCTL，并向 OV5647 的 `0x503D` 寄存器写入对应控制位。

---

## 2. 代码实现细节

我们在 `uvc_stream.c`、`uvc_stream.h` 以及 `main.c` 中做了以下修改：

### 2.1 自动恢复状态机制
当摄像头启动流传输时，驱动层会调用 `ov5647_set_format` 重置摄像头寄存器，这将覆盖之前设置的彩带状态。为了确保彩带状态在流启动后依然生效，我们设计了状态自动应用机制：

1. 声明 `s_test_pattern_enabled` 用于保存当前的测试彩带状态：
   ```c
   static bool s_test_pattern_enabled = false;
   ```

2. 封装 `uvc_apply_test_pattern` 静态函数，通过标准 V4L2 API 将该状态直接写入活跃中的摄像头设备中：
   ```c
   static esp_err_t uvc_apply_test_pattern(bool enable) {
       struct v4l2_ext_controls controls;
       struct v4l2_ext_control control[1];
       // ...
       control[0].id = V4L2_CID_TEST_PATTERN;
       control[0].value = enable ? 1 : 0;
       return ioctl(s_uvc->cap_fd, VIDIOC_S_EXT_CTRLS, &controls);
   }
   ```

3. 在 `video_start_cb` 视频流配置完成、重置寄存器后，立即读取并应用之前的测试状态：
   ```c
   if (s_test_pattern_enabled) {
       uvc_apply_test_pattern(true);
   }
   ```

---

## 3. 控制与查询接口 (APIs)

我们在 `uvc_stream.h` 中导出了以下三个接口函数：

```c
/**
 * @brief 设置摄像头测试彩带使能状态
 * @param enable true为开启，false为关闭
 * @return esp_err_t ESP_OK 表示设置成功
 */
esp_err_t uvc_set_test_pattern(bool enable);

/**
 * @brief 查询当前测试彩带的使能状态
 * @return true为开启，false为关闭
 */
bool uvc_is_test_pattern_enabled(void);

/**
 * @brief 摄像头测试彩带控制的控制台指令回调
 */
int cmd_camera_test_pattern(int argc, char **argv);
```

---

## 4. 控制台指令使用指南

我们在控制台中注册了 `camera_test_pattern` 指令，用于动态开启/关闭或查询状态：

### 4.1 开启彩带测试
```bash
camera_test_pattern 1
# 或
camera_test_pattern on
```
* **说明**: 如果当前摄像头正在运行流传输（通过 UVC 连接到电脑，或者通过 `camera_capture` 拍照），彩带效果将立即投射到输出的视频帧中；如果当前处于闲置状态，此设置将被缓存，并在下一次启动传输时自动应用。

### 4.2 关闭彩带测试
```bash
camera_test_pattern 0
# 或
camera_test_pattern off
```
* **说明**: 恢复摄像头正常画面的实时采集输出。

### 4.3 查询当前设置状态
直接运行不带参数 of 命令，或者执行 `uvc_status`：
```bash
child_help> camera_test_pattern
Current status: Enabled
```

```bash
child_help> uvc_status
UVC Camera Status:
  Initialized:  Yes
  Streaming:    Yes (Active)
  Resolution:   800 x 640
  Frame Rate:   50 FPS
  Test Pattern: Enabled
```

---

## 5. 文件管理与简化输入功能 (File Management & Path Simplification)

为了提升串口控制台的使用体验，我们对文件管理命令做出了以下优化：

### 5.1 默认路径优化 (`/spiffs`)
对于所有的文件输入参数，默认路径都会被设为 `/spiffs`。用户无需输入完整的 `/spiffs/...` 绝对路径，直接输入文件名或相对路径即可。

系统会检测输入路径：
* 如果输入路径以 `/` 开头，则视为绝对路径，直接使用。
* 如果不以 `/` 开头，则自动在前端拼接 `/spiffs/`。

此优化适用于以下指令：
1. `ls [path]`（默认列出 `/spiffs` 的文件，支持 `ls subdir` 列出 `/spiffs/subdir` 目录）
2. `camera_capture [filepath]`（默认保存为 `photo.jpg`，支持直接输入 `test.jpg` 保存到 `/spiffs/test.jpg`）
3. `zmodem_send <filepath>`（支持直接输入文件名发送，如 `zmodem_send photo.jpg`）
4. `zmodem_recv [directory]`（默认接收至 `/spiffs` 目录）
5. `rm <filepath>`（支持直接输入文件名删除，如 `rm photo.jpg`）

---

### 5.2 `rm` 与 `rm all` 删除指令
新增 `rm` 串口指令用于删除 SPIFFS 文件系统中的文件：

* **删除单个文件**:
  ```bash
  rm photo.jpg
  ```
  * *说明*: 也可以使用绝对路径：`rm /spiffs/photo.jpg`。
* **删除所有文件**:
  ```bash
  rm all
  ```
  * *说明*: 此操作将遍历并删除 `/spiffs` 目录下所有的普通文件（产线或调试重置非常方便）。

