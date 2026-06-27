# ESP32-P4 UVC 摄像头流媒体传输项目

[English Version](./README.md)

本项目基于 ESP32-P4 芯片和 OV5647 (MIPI-CSI) 摄像头传感器，实现了通过 USB-OTG 接口进行 UVC 视频流传输（MJPEG 格式）的功能。针对早期芯片版本（v3.0 以下）的硬件裁剪限制，本项目实现了传感器与 UVC 栈的 1:1 分辨率直通握手，规避了格式不匹配引发的系统崩溃问题。

---

## 1. 硬件连接与引脚分配

### 1.1 摄像头接口 (OV5647 MIPI-CSI)
* **I2C 控制总线 (SCCB)**：SDA = GPIO7, SCL = GPIO8
* **MIPI CSI 数据线**：CLK P/N, DATA0 P/N, DATA1 P/N

### 1.2 USB-OTG 接口
本工程通过调用 TinyUSB 设备协议栈，将 USB-OTG 信号路由至外部 GPIO 引脚，运行于全速模式（Full-Speed, 12Mbps）：
* **USB D-**：GPIO24
* **USB D+**：GPIO25

---

## 2. 支持的分辨率与直通配置 (1:1 Mode)

由于 ESP32-P4 v3.0 以下版本芯片暂不支持硬件裁剪（ISP Crop），为了防止 PC 请求分辨率与传感器输出不匹配而触发驱动断言崩溃，本项目采用了 **1:1 物理直通配置**。

目前在 `sdkconfig` 中预设并测试通过的模式有：

| 分辨率 | 宽高比 | 传感器帧率 (Max) | 适用场景与带宽表现 |
| :--- | :--- | :--- | :--- |
| **800x640 (当前默认)** | 5:4 | 50 fps | **极速流畅推荐**：在全速 USB (12Mbps) 带宽下传输极其轻快，跑满 50fps，延迟极低。 |
| **1280x960** | 4:3 | 45 fps | **高清推荐**：数据量比 1080P 减少约 40%，兼顾清晰度与总线开销，适合绝大多数测试。 |
| **1920x1080** | 16:9 | 30 fps | **宽屏全高清**：画面细节最好，但在全速 12Mbps 带宽下接近总线负荷上限，高帧率下可能卡顿。 |

> [!IMPORTANT]
> 项目中必须在 `sdkconfig` 里禁用多分辨率广播选项 `CONFIG_UVC_CAM1_MULTI_FRAMESIZE`，只允许广播当前配置的唯一分辨率。PC 播放器连接时，必须选择与设备配置严格相同的分辨率进行拉流，否则将触发 V4L2 校验失败。

---

## 3. 构建与烧录

### 3.1 环境要求
* ESP-IDF SDK 5.5
* 建议使用 VS Code ESP-IDF 插件或命令行开发环境。

### 3.2 构建命令
```bash
# 设置编译目标为 esp32p4
idf.py set-target esp32p4

# 编译项目
idf.py build

# 烧录固件并开启监视器
idf.py -p <PORT> flash monitor
```

---

## 4. 运行与测试步骤

1. 将编译好的固件通过 UART 烧录至开发板。
2. 连接串口终端，在交互控制台（Console）输入命令启动 UVC 服务：
   ```bash
   child_help> uvc_init
   ```
3. 将开发板的 **GPIO24 (D-)** 和 **GPIO25 (D+)** 通过 USB 数据线连接到 PC。
4. PC 设备管理器将识别到名为 `ESP UVC Device` 的图像设备。
5. 打开 PC 端视频播放/拉流软件（推荐使用 **PotPlayer** 或 **OBS**）。
6. 在播放器中开启摄像头，并**确保手动将分辨率格式设置为设备当前配置的直通分辨率**（如默认的 `800x640`），即可流畅显示实时摄像头画面。

---

## 5. 项目核心配置文件

* [sdkconfig.defaults](./sdkconfig.defaults)：项目基础编译配置模板（已包含 UVC 及摄像头默认分辨率）。
* [sdkconfig](./sdkconfig)：本地当前编译活动配置。
* [main/main.c](./main/main.c)：应用程序主入口，包含 USB-OTG 信号重路由、CSI 与 JPEG 编码器初始化、以及 UVC 回调控制逻辑。
* [components/ov5647/](./components/ov5647/)：OV5647 摄像头的底层 MIPI 传感器驱动组件。
