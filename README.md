# ESP32-P4 UVC Camera Video Streaming Project

[中文版本](./README_CN.md)

This project is built on the ESP32-P4 and the OV5647 (MIPI-CSI) camera sensor. It implements MJPEG video streaming over the USB-OTG interface using the UVC (USB Video Class) protocol. To bypass the hardware cropping limits of ESP32-P4 silicon versions below v3.0, the project implements a 1:1 resolution pass-through configuration between the camera sensor and the UVC stack.

---

## 1. Hardware Connections & Pin Allocations

### 1.1 Camera Sensor (OV5647 MIPI-CSI)
* **I2C Control Bus (SCCB)**: SDA = GPIO7, SCL = GPIO8
* **MIPI CSI Lanes**: CLK P/N, DATA0 P/N, DATA1 P/N

### 1.2 USB-OTG Interface
The project routes USB-OTG signals to external GPIO pins and forces the TinyUSB stack to run in Full-Speed mode (12Mbps) to match the external physical lines:
* **USB D-**: GPIO24
* **USB D+**: GPIO25

---

## 2. Supported Resolutions & 1:1 Pass-through Modes

Since runtime hardware cropping (ISP Crop) is not supported on ESP32-P4 silicon revisions below v3.0, **1:1 physical resolution matching** is mandatory. If there is a mismatch, the V4L2 format validation will trigger an assertion and crash the board.

The following configurations are tested and predefined in `sdkconfig`:

| Resolution | Aspect Ratio | Sensor FPS (Max) | Bandwidth & Smoothness Performance |
| :--- | :--- | :--- | :--- |
| **800x640 (Default)** | 5:4 | 50 fps | **Recommended for Speed**: Extremely light data overhead over USB FS (12Mbps), runs smoothly at 50fps with very low latency. |
| **1280x960** | 4:3 | 45 fps | **Recommended for HD**: 40% less data than 1080P, strikes a balance between clarity and USB bus overhead. |
| **1920x1080** | 16:9 | 30 fps | **Full HD**: Best image details, but approaches the USB 12Mbps bandwidth limit, which may cause stuttering at high frame rates. |

> [!IMPORTANT]
> The option `CONFIG_UVC_CAM1_MULTI_FRAMESIZE` must be disabled in Kconfig. The UVC stack only advertises the single configured resolution. The PC media player must select the exact matching resolution to start the video stream successfully.

---

## 3. Build & Flash

### 3.1 Requirements
* ESP-IDF SDK v5.5
* VS Code ESP-IDF extension or terminal.

### 3.2 CLI Commands
```bash
# Set target chip to esp32p4
idf.py set-target esp32p4

# Build the project
idf.py build

# Flash binary and monitor console
idf.py -p <PORT> flash monitor
```

---

## 4. Run & Test Steps

1. Flash the compiled firmware into the development board.
2. Open the serial monitor and type the initialization command in the console:
   ```bash
   child_help> uvc_init
   ```
3. Connect the board's **GPIO24 (D-)** and **GPIO25 (D+)** to your PC using a USB cable.
4. The PC Device Manager will list a camera device named `ESP UVC Device`.
5. Open your favorite media player (e.g., **PotPlayer** or **OBS Studio**).
6. Open the camera device, **ensuring the resolution setting matches the active configured pass-through resolution** (e.g., `800x640`). Enjoy the low-latency real-time video stream!

---

## 5. Core Configuration Files

* [sdkconfig.defaults](./sdkconfig.defaults): Project default configuration template containing UVC and camera options.
* [sdkconfig](./sdkconfig): Active compilation config file.
* [main/main.c](./main/main.c): Main application entry point, containing USB-OTG pin routing, CSI camera, JPEG encoder device initialization, and UVC stream control callbacks.
* [components/ov5647/](./components/ov5647/): The camera driver library for the OV5647 MIPI sensor.
