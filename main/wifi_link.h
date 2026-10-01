/*
 * wifi_link.h — WiFi STA 连接 + HTTP POST 帮手 (S6 语音识别链路)
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* 启动 WiFi STA 连接 (异步, NVS/事件循环内部初始化; 幂等). */
void wifi_link_start(void);

/* 是否已拿到 IP. */
bool wifi_link_got_ip(void);

/* POST body 到 url, 可带一个自定义头, 响应写入 resp (截断保护). 需已拿到 IP. */
esp_err_t wifi_link_post(const char *url, const char *body, size_t len,
                         const char *hdr_name, const char *hdr_val,
                         char *resp, size_t resp_sz, int timeout_ms);
