#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"
#include "usb_device_uvc.h"
#include "uvc_frame_config.h"
#include "console.h"
#include "hal/usb_serial_jtag_ll.h"

#define EXAMPLE_JPEG_COMPRESSION_QUALITY 80
#define BUFFER_COUNT        2

#ifndef EXAMPLE_CAM_DEV_PATH
#define EXAMPLE_CAM_DEV_PATH ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#endif

#ifndef ENCODE_DEV_PATH
#define ENCODE_DEV_PATH ESP_VIDEO_JPEG_DEVICE_NAME
#endif

typedef struct uvc {
    int cap_fd;
    uint32_t format;
    uint8_t *cap_buffer[BUFFER_COUNT];

    int m2m_fd;
    uint8_t *m2m_cap_buffer;

    uvc_fb_t fb;
} uvc_t;

static const char *TAG = "uvc_app";
static uvc_t *s_uvc = NULL;
i2c_master_bus_handle_t g_i2c_bus_handle = NULL;

static bool s_uvc_initialized = false;
static bool s_uvc_streaming = false;
static int s_stream_width = 0;
static int s_stream_height = 0;
static int s_stream_fps = 0;

static void print_video_device_info(const struct v4l2_capability *capability)
{
    ESP_LOGI(TAG, "version: %d.%d.%d", (uint16_t)(capability->version >> 16),
             (uint8_t)(capability->version >> 8),
             (uint8_t)capability->version);
    ESP_LOGI(TAG, "driver:  %s", capability->driver);
    ESP_LOGI(TAG, "card:    %s", capability->card);
    ESP_LOGI(TAG, "bus:     %s", capability->bus_info);
    ESP_LOGI(TAG, "capabilities:");
    if (capability->capabilities & V4L2_CAP_VIDEO_CAPTURE) {
        ESP_LOGI(TAG, "\tVIDEO_CAPTURE");
    }
    if (capability->capabilities & V4L2_CAP_READWRITE) {
        ESP_LOGI(TAG, "\tREADWRITE");
    }
    if (capability->capabilities & V4L2_CAP_ASYNCIO) {
        ESP_LOGI(TAG, "\tASYNCIO");
    }
    if (capability->capabilities & V4L2_CAP_STREAMING) {
        ESP_LOGI(TAG, "\tSTREAMING");
    }
    if (capability->capabilities & V4L2_CAP_META_OUTPUT) {
        ESP_LOGI(TAG, "\tMETA_OUTPUT");
    }
}

static esp_err_t init_capture_video(uvc_t *uvc)
{
    int fd;
    struct v4l2_capability capability;

    fd = open(EXAMPLE_CAM_DEV_PATH, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open capture device: %s", EXAMPLE_CAM_DEV_PATH);
        return ESP_FAIL;
    }

    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
        ESP_LOGE(TAG, "Failed to query capture device capability");
        close(fd);
        return ESP_FAIL;
    }
    print_video_device_info(&capability);

    uvc->cap_fd = fd;
    return ESP_OK;
}

static esp_err_t init_codec_video(uvc_t *uvc)
{
    int fd;
    const char *devpath = ENCODE_DEV_PATH;
    struct v4l2_capability capability;
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[1];

    fd = open(devpath, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open codec device: %s", devpath);
        return ESP_FAIL;
    }

    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
        ESP_LOGE(TAG, "Failed to query codec device capability");
        close(fd);
        return ESP_FAIL;
    }
    print_video_device_info(&capability);

    controls.ctrl_class = V4L2_CID_JPEG_CLASS;
    controls.count      = 1;
    controls.controls   = control;
    control[0].id       = V4L2_CID_JPEG_COMPRESSION_QUALITY;
    control[0].value    = EXAMPLE_JPEG_COMPRESSION_QUALITY;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "failed to set JPEG compression quality");
    }

    uvc->format = V4L2_PIX_FMT_JPEG;
    uvc->m2m_fd = fd;
    return ESP_OK;
}

static esp_err_t video_start_cb(uvc_format_t uvc_format, int width, int height, int rate, void *cb_ctx)
{
    int type;
    struct v4l2_buffer buf;
    struct v4l2_format format;
    struct v4l2_requestbuffers req;
    uvc_t *uvc = (uvc_t *)cb_ctx;
    uint32_t capture_fmt = 0;

    ESP_LOGI(TAG, "UVC Start requested: format=%d, width=%d, height=%d, rate=%d", uvc_format, width, height, rate);

    int fmt_index = 0;
    const uint32_t jpeg_input_formats[] = {
        V4L2_PIX_FMT_RGB565,
        V4L2_PIX_FMT_UYVY,
        V4L2_PIX_FMT_RGB24,
        V4L2_PIX_FMT_GREY
    };
    int jpeg_input_formats_num = sizeof(jpeg_input_formats) / sizeof(jpeg_input_formats[0]);

    while (!capture_fmt) {
        struct v4l2_fmtdesc fmtdesc = {
            .index = fmt_index++,
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        };

        if (ioctl(uvc->cap_fd, VIDIOC_ENUM_FMT, &fmtdesc) != 0) {
            break;
        }

        for (int i = 0; i < jpeg_input_formats_num; i++) {
            if (jpeg_input_formats[i] == fmtdesc.pixelformat) {
                capture_fmt = jpeg_input_formats[i];
                break;
            }
        }
    }

    if (!capture_fmt) {
        ESP_LOGE(TAG, "The camera sensor output pixel format is not supported by JPEG");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Configure camera interface capture stream */
    struct v4l2_format current_format;
    memset(&current_format, 0, sizeof(current_format));
    current_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(uvc->cap_fd, VIDIOC_G_FMT, &current_format) == 0) {
        ESP_LOGI(TAG, "Current sensor capture resolution: %dx%d", 
                 (int)current_format.fmt.pix.width, (int)current_format.fmt.pix.height);
        
        if (current_format.fmt.pix.width != width || current_format.fmt.pix.height != height) {
            struct v4l2_selection selection;
            memset(&selection, 0, sizeof(selection));
            selection.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            selection.target = V4L2_SEL_TGT_CROP;
            selection.r.width = width;
            selection.r.height = height;
            selection.r.left = (current_format.fmt.pix.width > width) ? (current_format.fmt.pix.width - width) / 2 : 0;
            selection.r.top = (current_format.fmt.pix.height > height) ? (current_format.fmt.pix.height - height) / 2 : 0;
            
            // Align to even boundary (helpful for ISP/Bayer patterns)
            selection.r.left &= ~1;
            selection.r.top &= ~1;

            ESP_LOGI(TAG, "Setting crop selection: left=%d, top=%d, width=%d, height=%d", 
                     (int)selection.r.left, (int)selection.r.top, (int)selection.r.width, (int)selection.r.height);
            
            if (ioctl(uvc->cap_fd, VIDIOC_S_SELECTION, &selection) != 0) {
                ESP_LOGW(TAG, "Failed to set selection (crop not supported or invalid)");
            }
        }
    } else {
        ESP_LOGE(TAG, "Failed to get current format");
    }

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = capture_fmt;
    ESP_ERROR_CHECK(ioctl(uvc->cap_fd, VIDIOC_S_FMT, &format));

    memset(&req, 0, sizeof(req));
    req.count  = BUFFER_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_ERROR_CHECK(ioctl(uvc->cap_fd, VIDIOC_REQBUFS, &req));

    for (int i = 0; i < BUFFER_COUNT; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory      = V4L2_MEMORY_MMAP;
        buf.index       = i;
        ESP_ERROR_CHECK(ioctl(uvc->cap_fd, VIDIOC_QUERYBUF, &buf));

        uvc->cap_buffer[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                             MAP_SHARED, uvc->cap_fd, buf.m.offset);
        assert(uvc->cap_buffer[i]);

        ESP_ERROR_CHECK(ioctl(uvc->cap_fd, VIDIOC_QBUF, &buf));
    }

    /* Configure codec output stream */
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = capture_fmt;
    ESP_ERROR_CHECK(ioctl(uvc->m2m_fd, VIDIOC_S_FMT, &format));

    memset(&req, 0, sizeof(req));
    req.count  = 1;
    req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_USERPTR;
    ESP_ERROR_CHECK(ioctl(uvc->m2m_fd, VIDIOC_REQBUFS, &req));

    /* Configure codec capture stream */
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = uvc->format;
    ESP_ERROR_CHECK(ioctl(uvc->m2m_fd, VIDIOC_S_FMT, &format));

    memset(&req, 0, sizeof(req));
    req.count  = 1;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_ERROR_CHECK(ioctl(uvc->m2m_fd, VIDIOC_REQBUFS, &req));

    memset(&buf, 0, sizeof(buf));
    buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory      = V4L2_MEMORY_MMAP;
    buf.index       = 0;
    ESP_ERROR_CHECK(ioctl(uvc->m2m_fd, VIDIOC_QUERYBUF, &buf));

    uvc->m2m_cap_buffer = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                          MAP_SHARED, uvc->m2m_fd, buf.m.offset);
    assert(uvc->m2m_cap_buffer);

    ESP_ERROR_CHECK(ioctl(uvc->m2m_fd, VIDIOC_QBUF, &buf));

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_ERROR_CHECK(ioctl(uvc->m2m_fd, VIDIOC_STREAMON, &type));
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ESP_ERROR_CHECK(ioctl(uvc->m2m_fd, VIDIOC_STREAMON, &type));

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_ERROR_CHECK(ioctl(uvc->cap_fd, VIDIOC_STREAMON, &type));

    s_uvc_streaming = true;
    s_stream_width = width;
    s_stream_height = height;
    s_stream_fps = rate;

    ESP_LOGI(TAG, "UVC Streaming Started successfully");
    return ESP_OK;
}

static void video_stop_cb(void *cb_ctx)
{
    int type;
    uvc_t *uvc = (uvc_t *)cb_ctx;

    ESP_LOGI(TAG, "UVC Stop requested");

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(uvc->cap_fd, VIDIOC_STREAMOFF, &type);

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(uvc->m2m_fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(uvc->m2m_fd, VIDIOC_STREAMOFF, &type);

    s_uvc_streaming = false;
    ESP_LOGI(TAG, "UVC Streaming Stopped");
}

static uvc_fb_t *video_fb_get_cb(void *cb_ctx)
{
    int64_t us;
    uvc_t *uvc = (uvc_t *)cb_ctx;
    struct v4l2_format format;
    struct v4l2_buffer cap_buf;
    struct v4l2_buffer m2m_out_buf;
    struct v4l2_buffer m2m_cap_buf;

    memset(&cap_buf, 0, sizeof(cap_buf));
    cap_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cap_buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(uvc->cap_fd, VIDIOC_DQBUF, &cap_buf) != 0) {
        return NULL;
    }

    memset(&m2m_out_buf, 0, sizeof(m2m_out_buf));
    m2m_out_buf.index  = 0;
    m2m_out_buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    m2m_out_buf.memory = V4L2_MEMORY_USERPTR;
    m2m_out_buf.m.userptr = (unsigned long)uvc->cap_buffer[cap_buf.index];
    m2m_out_buf.length = cap_buf.bytesused;
    if (ioctl(uvc->m2m_fd, VIDIOC_QBUF, &m2m_out_buf) != 0) {
        ioctl(uvc->cap_fd, VIDIOC_QBUF, &cap_buf);
        return NULL;
    }

    memset(&m2m_cap_buf, 0, sizeof(m2m_cap_buf));
    m2m_cap_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    m2m_cap_buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(uvc->m2m_fd, VIDIOC_DQBUF, &m2m_cap_buf) != 0) {
        ioctl(uvc->cap_fd, VIDIOC_QBUF, &cap_buf);
        return NULL;
    }

    ioctl(uvc->cap_fd, VIDIOC_QBUF, &cap_buf);
    ioctl(uvc->m2m_fd, VIDIOC_DQBUF, &m2m_out_buf);

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(uvc->m2m_fd, VIDIOC_G_FMT, &format);

    uvc->fb.buf = uvc->m2m_cap_buffer;
    uvc->fb.len = m2m_cap_buf.bytesused;
    uvc->fb.width = format.fmt.pix.width;
    uvc->fb.height = format.fmt.pix.height;
    uvc->fb.format = UVC_FORMAT_JPEG;

    us = esp_timer_get_time();
    uvc->fb.timestamp.tv_sec = us / 1000000UL;;
    uvc->fb.timestamp.tv_usec = us % 1000000UL;

    return &uvc->fb;
}

static void video_fb_return_cb(uvc_fb_t *fb, void *cb_ctx)
{
    struct v4l2_buffer m2m_cap_buf;
    uvc_t *uvc = (uvc_t *)cb_ctx;

    m2m_cap_buf.index  = 0;
    m2m_cap_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    m2m_cap_buf.memory = V4L2_MEMORY_MMAP;
    ioctl(uvc->m2m_fd, VIDIOC_QBUF, &m2m_cap_buf);
}

static esp_err_t init_uvc(uvc_t *uvc)
{
    int index = 0;
    uvc_device_config_t config = {
        .start_cb     = video_start_cb,
        .fb_get_cb    = video_fb_get_cb,
        .fb_return_cb = video_fb_return_cb,
        .stop_cb      = video_stop_cb,
        .cb_ctx       = (void *)uvc,
    };

    config.uvc_buffer_size = UVC_FRAMES_INFO[index][0].width * UVC_FRAMES_INFO[index][0].height;
    config.uvc_buffer = malloc(config.uvc_buffer_size);
    if (!config.uvc_buffer) {
        ESP_LOGE(TAG, "Failed to allocate UVC transfer buffer");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UVC Default Resolution: %d * %d @%dfps", 
             UVC_FRAMES_INFO[index][0].width, 
             UVC_FRAMES_INFO[index][0].height, 
             UVC_FRAMES_INFO[index][0].rate);

    ESP_ERROR_CHECK(uvc_device_config(index, &config));
    ESP_ERROR_CHECK(uvc_device_init());

    return ESP_OK;
}

static int cmd_uvc_init(int argc, char **argv)
{
    if (s_uvc_initialized) {
        printf("UVC is already initialized!\n");
        return 0;
    }

    printf("Starting UVC Camera initialization...\n");

    // Route USB-OTG Full-Speed to GPIO24/GPIO25, and USB-Serial-JTAG to GPIO26/GPIO27
    printf("Routing USB-OTG to GPIO24/GPIO25 (USB FS PHY 0)...\n");
    usb_serial_jtag_ll_phy_select(1);

    // Initialize I2C Bus
    printf("Initializing I2C Master Bus (SDA=7, SCL=8)...\n");
    i2c_master_bus_config_t i2c_bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = 0,
        .scl_io_num = 8,
        .sda_io_num = 7,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&i2c_bus_cfg, &g_i2c_bus_handle);
    if (err != ESP_OK) {
        printf("Failed to initialize I2C bus: %s\n", esp_err_to_name(err));
        return 1;
    }

    // Initialize Video Pipeline
    printf("Initializing esp_video (CSI, ISP, JPEG Enc)...\n");
    esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = g_i2c_bus_handle,
            .freq = 100000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
        .dont_init_ldo = false,
     };
     esp_video_init_config_t cam_config = {
         .csi = &csi_config,
     };
     err = esp_video_init(&cam_config);
     if (err != ESP_OK) {
         printf("Failed to initialize esp_video: %s\n", esp_err_to_name(err));
         return 1;
     }

     // Allocate UVC struct
     s_uvc = calloc(1, sizeof(uvc_t));
     if (!s_uvc) {
         printf("Failed to allocate memory for UVC struct\n");
         return 1;
     }

     // Open Video Devices
     printf("Opening capture device: %s...\n", EXAMPLE_CAM_DEV_PATH);
     err = init_capture_video(s_uvc);
     if (err != ESP_OK) {
         printf("Failed to init capture video\n");
         free(s_uvc);
         s_uvc = NULL;
         return 1;
     }

     printf("Opening codec device: %s...\n", ENCODE_DEV_PATH);
     err = init_codec_video(s_uvc);
     if (err != ESP_OK) {
         printf("Failed to init codec video\n");
         close(s_uvc->cap_fd);
         free(s_uvc);
         s_uvc = NULL;
         return 1;
     }

     // Initialize USB UVC Device
     printf("Initializing USB UVC Stack...\n");
     err = init_uvc(s_uvc);
     if (err != ESP_OK) {
         printf("Failed to init UVC USB stack\n");
         close(s_uvc->cap_fd);
         close(s_uvc->m2m_fd);
         free(s_uvc);
         s_uvc = NULL;
         return 1;
     }

     s_uvc_initialized = true;
     printf("UVC Camera System initialized successfully!\n");
     printf("Connect ESP32-P4 to PC via USB (GPIO24/25) to test streaming.\n");
     return 0;
}

static int cmd_uvc_status(int argc, char **argv)
{
    printf("UVC Camera Status:\n");
    printf("  Initialized: %s\n", s_uvc_initialized ? "Yes" : "No");
    printf("  Streaming:   %s\n", s_uvc_streaming ? "Yes (Active)" : "No (Idle)");
    if (s_uvc_streaming) {
        printf("  Resolution:  %d x %d\n", s_stream_width, s_stream_height);
        printf("  Frame Rate:  %d FPS\n", s_stream_fps);
    }
    return 0;
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initializing console...");
    ESP_ERROR_CHECK(console_init());

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

    ESP_LOGI(TAG, "Starting console REPL...");
    ESP_ERROR_CHECK(console_start());
}
