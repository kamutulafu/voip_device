#ifndef APP_TIME_H
#define APP_TIME_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* 出厂默认时区：中国标准时间 (UTC+8)。POSIX TZ 的符号与 UTC 偏移相反，
 * 所以 UTC+8 写成 "CST-8"。 */
#define APP_TIME_DEFAULT_TZ     "CST-8"

/**
 * @brief 从 NVS 载入并应用时区，必须在任何 localtime()/strftime() 之前调用一次
 *
 * 之前系统从未调用 tzset()，libc 默认按 UTC 解释本地时间，
 * 于是手机配网写入的 UTC 时间戳被当成本地时间显示，小时数整体偏 8 小时。
 */
void app_time_init(void);

/**
 * @brief 设置并持久化时区（以相对 UTC 的分钟数表示，东为正）
 *
 * @param minutes_east 例如北京时间为 +480；取值范围 -840 ~ +840
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 偏移非法
 */
esp_err_t app_time_set_utc_offset_minutes(int minutes_east);

/**
 * @brief 用 UTC 纪元秒设置系统 RTC 时间
 *
 * @param epoch_seconds UTC 纪元秒（必须晚于 2024 年，用于过滤未初始化的值）
 * @return ESP_OK 成功
 */
esp_err_t app_time_set_epoch(int64_t epoch_seconds);

/**
 * @brief 格式化当前本地时间为 "YYYY-MM-DD HH:MM:SS"
 */
void app_time_local_str(char *out, size_t out_size);

#endif // APP_TIME_H
