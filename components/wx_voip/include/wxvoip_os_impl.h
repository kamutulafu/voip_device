#ifndef _WXVOIP_OS_IMPL_H
#define _WXVOIP_OS_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef struct wxvoip_os_impl {
    /* 堆内存分配实现 */
    void* (*os_malloc)(size_t size);
    void  (*os_free)(void *ptr);

    /* 标准输出 */
    void (*os_printf)(const char *format, va_list ap);

    /* 当前时间，毫秒单位，从 1970-1-1:00:00:00 到现在 */
    uint32_t (*os_gettime_ms)(void);

    /* 当时时间，秒单位, 从 1970-1-1:00:00:00 到现在  */
    uint32_t (*os_gettime_second)(void);

    /* Sleep 实现，毫秒单位 */
    void (*os_msleep)(uint32_t ms);

    /* 设备在用的网络设备 MAC 地址 */
    void (*os_getmac)(uint8_t *mac, size_t mac_len);

    /* 根分区大小，单位 kB */
    size_t (*os_get_disksize)(void);

    /* 内存大小，单位 kB */
    size_t (*os_get_memsize)(void);

    /* 返回平台类型，比如 Linux、RTOS 等 */
    const char * (*os_get_platform)(void);

    /* 创建文件：若文件不存在，则创建，若文件存在，则打开，返回句柄 */
    int (*os_create_file)(const char *path);

    /* 读文件，返回读到的字节 */
    size_t (*os_read_file)(int fd, void *buf, size_t count);

    /* 写文件，返回写入的字节 */
    size_t (*os_write_file)(int fd, const void *buf, size_t count);

    /* 关闭文件句柄 */
    int (*os_close_file)(int fd);

    /* 随机数据生成 */
    long int (*os_random)(void);

} wxvoip_os_impl_t;


#ifdef __cplusplus
}
#endif

#endif
