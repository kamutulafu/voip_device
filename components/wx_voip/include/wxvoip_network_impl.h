#ifndef _WXVOIP_NETWORK_IMPL_H
#define _WXVOIP_NETWORK_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define NETWORK_RET_SUCCESS          0
#define NETWORK_ERR_CONNECT         -1
#define NETWORK_ERR_WRITE_TIMEOUT   -2
#define NETWORK_ERR_WRITE_FAIL      -3
#define NETWORK_ERR_READ_TIMEOUT    -4
#define NETWORK_ERR_READ_FAIL       -5
#define NETWORK_ERR_PEER_SHUTDOWN   -6
#define NETWORK_ERR_NOTHING_TO_READ -7

typedef struct wxvoip_network_https_impl {   
    /* 
     * 发送 POST 请求至 https://host/path
     *
     * 参数
     *   stack: 自身对象
     *   host: 地址
     *   port: 端口
     *   path: 路径
     *   body: post 内容
     *   resp: 回复内容，实现里应该为 resp 分配 buffer，SDK 用完 resp 后会释放它.
     * 
     * 返回
     *   NETWORK_RET_SUCCESS: 正确请求并回复
     *   其它: 异常
     */
    int (*post_with_resp)(struct wxvoip_network_https_impl *stack, 
            const char *host, 
            int port,
            const char *path, 
            const char *body, 
            char **resp
            );

    /*
     * SDK 内部会分配 buffer_size 用来做一些临时缓冲区
     * 开发者传 0 的话则 SDK 内部会使用 1024 默认值。
     *   当开发者想节省堆内存时，可以尝试将此值改小。
     *   当开发者的 payload 值非常大导致内部错误时，可以尝试将此值加大。
     */
    size_t buffer_size;
} wxvoip_network_https_impl_t;

#ifdef __cplusplus
}
#endif

#endif
