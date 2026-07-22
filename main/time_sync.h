#pragma once

/**
 * @brief 异步启动一次"蹭 HTTP Date 响应头"的开机校时任务。
 *
 * 使用明文 HTTP（非 HTTPS）向若干备选站点发起一次 HEAD 请求，
 * 从响应头的 Date 字段解析出当前 UTC 时间并写入系统时钟。
 *
 * 之所以用明文 HTTP 而不是 HTTPS，是为了避开"系统时间未校准
 * 导致 TLS 证书有效期校验失败"的先有鸡还是先有蛋问题：
 * ESP32 上电后系统时间从 1970 年开始，如果直接发起 HTTPS 请求，
 * mbedTLS 会因为证书"尚未生效"而拒绝握手。
 *
 * 该函数会创建一个短生命周期的 FreeRTOS 任务并立即返回，不会
 * 阻塞调用者。任务完成校时（或重试耗尽失败）后会自行退出。
 *
 * 建议在确认设备已具备真实互联网连通性之后调用一次即可
 * （例如 wifi_manager.c 中 DNS 连通性检测通过之后）。
 * 校时结果会直接体现在系统时钟里，tts_xfyun.c / asr_xfyun.c
 * 里现有的 ensure_time_synced() 无需任何改动即可自动受益：
 * 只要系统时间的年份 >= 2024，它们就会判定"已同步"并跳过
 * 原有的 SNTP 逻辑。
 */
void time_sync_start(void);

/**
 * @brief 调试用控制台命令：打印当前系统时间，用于验证开机校时
 * 是否生效（仅在 APP_BUILD_DEBUG 下注册）。
 */
int cmd_date(int argc, char **argv);
