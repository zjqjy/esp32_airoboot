/*
 * wifi_link.c — WiFi STA 连接 + HTTP POST (S6 语音识别链路)
 * SSID/密码/服务地址来自 Kconfig (CONFIG_S6_WIFI_*, 写在 sdkconfig, 不入库).
 * 断线自动重连; 拿到 IP 前 wifi_link_post 返回 ESP_ERR_INVALID_STATE.
 */
#include <string.h>
#include "wifi_link.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_link";
static EventGroupHandle_t s_evt;
#define EVT_GOT_IP BIT0

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_evt, EVT_GOT_IP);
        ESP_LOGW(TAG, "WiFi 断开, 重连...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi OK, IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_evt, EVT_GOT_IP);
    }
}

void wifi_link_start(void)
{
    static bool s_started;
    if (s_started) {
        return;
    }
    s_started = true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_evt = xEventGroupCreate();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        event_handler, NULL, NULL);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, CONFIG_S6_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_S6_WIFI_PASS, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi 连接中: SSID=%s", CONFIG_S6_WIFI_SSID);
}

bool wifi_link_got_ip(void)
{
    return s_evt && (xEventGroupGetBits(s_evt) & EVT_GOT_IP);
}

esp_err_t wifi_link_post(const char *url, const char *body, size_t len,
                         const char *hdr_name, const char *hdr_val,
                         char *resp, size_t resp_sz, int timeout_ms)
{
    if (!wifi_link_got_ip()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    if (hdr_name && hdr_val) {
        esp_http_client_set_header(client, hdr_name, hdr_val);
    }
    esp_err_t err = esp_http_client_open(client, (int)len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    int w = esp_http_client_write(client, body, (int)len);
    if (w < 0 || (size_t)w != len) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (resp && resp_sz) {
        int total = 0, r;
        while ((r = esp_http_client_read(client, resp + total,
                                         (int)resp_sz - 1 - total)) > 0) {
            total += r;
            if (total >= (int)resp_sz - 1) break;
        }
        resp[total > 0 ? total : 0] = 0;
    }
    esp_http_client_cleanup(client);
    return ESP_OK;
}
