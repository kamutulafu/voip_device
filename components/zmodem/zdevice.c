/*
 * File      : zdevice.c
 * the implemention of zmodem protocol.
 * Change Logs:
 * Date           Author       Notes
 * 2011-03-29     itspy       
 */

#include "zdef.h"

#ifndef CONFIG_ESP_CONSOLE_UART_NUM
#define CONFIG_ESP_CONSOLE_UART_NUM 0
#endif

rt_uint32_t Line_left  = 0;          /* left number of data in the read line buffer*/ 
rt_uint32_t Left_sizes = 0;          /* left file sizes */
rt_uint32_t Baudrate   = BITRATE;    /* console baudrate */

rt_uint32_t get_device_baud(void)
{
    return(Baudrate);
}

rt_uint32_t get_sys_time(void)
{
    return(0L);
}

void zsend_byte(rt_uint16_t ch)
{
    uint8_t b = ch & 0xFF;
    uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &b, 1);
}

void zsend_line(rt_uint16_t c)
{
    uint8_t b = c & 0xFF;
    uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &b, 1);
}

rt_int16_t zread_line(rt_uint16_t timeout)
{
    uint8_t c;
    // timeout is in tenths of a second (100ms units)
    int timeout_ms = timeout * 100;
    if (timeout_ms == 0) {
        timeout_ms = 10;
    }
    int len = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &c, 1, pdMS_TO_TICKS(timeout_ms));
    if (len > 0) {
        return c & 0377;
    }
    return TIMEOUT;
}

/*
 * send a string to the modem, processing for \336 (sleep 1 sec)
 *   and \335 (break signal)
 */
void zsend_break(char *cmd)
{
    while (*cmd++) 
    {
        switch (*cmd) 
        {
        case '\336':
             continue;
        case '\335':
             rt_thread_delay(pdMS_TO_TICKS(1000));
             continue;
        default:
             zsend_line(*cmd);
             break;
        }
    }
}

/* send cancel string to get the other end to shut up */
void zsend_can(void)
{
    static char cmd[] = {24,24,24,24,24,24,24,24,24,24,0};

    zsend_break(cmd);
    rt_kprintf("\x0d");
    Line_left=0;           /* clear Line_left */

    return;
}

#include "driver/usb_serial_jtag.h"
#include <stdarg.h>

extern bool uvc_is_initialized(void);

static bool s_usb_jtag_debug_inited = false;

void usb_jtag_debug_init(void)
{
    if (s_usb_jtag_debug_inited) {
        return;
    }
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err == ESP_OK) {
        s_usb_jtag_debug_inited = true;
    }
}

void usb_jtag_debug_printf(const char *fmt, ...)
{
    if (uvc_is_initialized()) {
        return;
    }
    if (!s_usb_jtag_debug_inited) {
        usb_jtag_debug_init();
    }
    if (!s_usb_jtag_debug_inited) {
        return;
    }

    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0) {
        usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(100));
    }
}
