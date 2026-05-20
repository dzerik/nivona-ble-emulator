#include "nivona_wifi.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#error "Create main/wifi_secrets.h (copy from main/wifi_secrets.h.template) "\
       "with your WIFI_SSID / WIFI_PASS / MDNS_HOSTNAME before building."
#endif

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// (Removed dead `nvs_flash.h` include — NVS init lives in main.c.)

static const char *TAG = "nivona_wifi";
static volatile bool s_connected = false;
static uint8_t s_consecutive_failures = 0;

// Map ESP-IDF WIFI_REASON_* codes to a human-readable label for logs.
// Only the most common diagnostics — anything else falls through to a
// numeric print.
static const char *reason_label(uint8_t r) {
    switch (r) {
        case WIFI_REASON_NO_AP_FOUND:           return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_EXPIRE:           return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_FAIL:             return "AUTH_FAIL";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_BEACON_TIMEOUT:        return "BEACON_TIMEOUT";
        case WIFI_REASON_ASSOC_FAIL:            return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:     return "HANDSHAKE_TIMEOUT";
        default:                                return "OTHER";
    }
}

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        wifi_event_sta_disconnected_t *ev =
            (wifi_event_sta_disconnected_t *)data;
        uint8_t r = ev ? ev->reason : 0;
        s_consecutive_failures++;
        // Credential-class failures: log at ERROR so the developer
        // actually notices on the serial console instead of wondering
        // why telnet never comes up. We still retry, but with a back-
        // off so we don't burn CPU pummelling a dead AP.
        if (r == WIFI_REASON_AUTH_FAIL ||
            r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            r == WIFI_REASON_AUTH_EXPIRE) {
            ESP_LOGE(TAG, "WiFi credential failure reason=%u (%s); "
                          "check secrets.yaml — retrying with back-off",
                     r, reason_label(r));
        } else if (s_consecutive_failures % 10 == 1) {
            // Throttle the non-credential warning so a flapping AP
            // doesn't fill the log buffer.
            ESP_LOGW(TAG, "WiFi disconnected reason=%u (%s), retry %u",
                     r, reason_label(r), (unsigned)s_consecutive_failures);
        }
        // Back-off proportional to consecutive failures, capped at 30s.
        uint32_t delay_ms = 500u * (uint32_t)s_consecutive_failures;
        if (delay_ms > 30000u) delay_ms = 30000u;
        if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_consecutive_failures = 0;
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
    }
}

int nivona_wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               event_handler, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid,     WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_ERROR_CHECK(esp_wifi_start());

    // mDNS so host can find us by name
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    mdns_service_add(NULL, "_http",   "_tcp", 80, NULL, 0);
    mdns_service_add(NULL, "_telnet", "_tcp", 23, NULL, 0);

    ESP_LOGI(TAG, "STA init, connecting to '%s', hostname=%s.local",
             WIFI_SSID, MDNS_HOSTNAME);
    return 0;
}

bool nivona_wifi_connected(void) { return s_connected; }
