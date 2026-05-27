#include "wifi_ap.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "wifi_ap";

static int s_sta_count = 0;
static int s_restart_attempts = 0;
static const netlink_config_t *s_ap_cfg = NULL;

static void ap_restart_timer_cb(TimerHandle_t timer)
{
    ESP_LOGW(TAG, "Attempting AP restart (%d/3)", s_restart_attempts);
    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK && s_ap_cfg) {
        int8_t tx_power = s_ap_cfg->wifi_tx_power;
        if (tx_power < 8) tx_power = 8;
        if (tx_power > 80) tx_power = 80;
        esp_wifi_set_max_tx_power(tx_power);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP restart failed: %s", esp_err_to_name(err));
    }
    xTimerDelete(timer, 0);
}

static void wifi_ap_event_handler(void *arg, esp_event_base_t base,
                                   int32_t event_id, void *event_data)
{
    switch (event_id) {
    case WIFI_EVENT_AP_START:
        s_restart_attempts = 0;
        ESP_LOGI(TAG, "WiFi AP started");
        break;
    case WIFI_EVENT_AP_STOP:
        ESP_LOGW(TAG, "WiFi AP stopped unexpectedly");
        if (s_restart_attempts < 3) {
            s_restart_attempts++;
            TimerHandle_t t = xTimerCreate("ap_rst", pdMS_TO_TICKS(2000),
                                            pdFALSE, NULL, ap_restart_timer_cb);
            if (t) xTimerStart(t, 0);
        } else {
            ESP_LOGE(TAG, "AP restart limit reached, giving up");
        }
        break;
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
        s_sta_count++;
        ESP_LOGI(TAG, "Station " MACSTR " connected (aid=%d, total=%d)",
                 MAC2STR(e->mac), e->aid, s_sta_count);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)event_data;
        if (s_sta_count > 0) s_sta_count--;
        ESP_LOGI(TAG, "Station " MACSTR " disconnected (reason=%d, total=%d)",
                 MAC2STR(e->mac), e->reason, s_sta_count);
        break;
    }
    }
}

int wifi_ap_get_sta_count(void)
{
    return s_sta_count;
}

esp_netif_t *wifi_ap_create(void)
{
    esp_netif_inherent_config_t wifi_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    wifi_cfg.flags = ESP_NETIF_FLAG_AUTOUP;
    wifi_cfg.ip_info = NULL;
    wifi_cfg.route_prio = 0;

    esp_netif_t *netif = esp_netif_create_wifi(WIFI_IF_AP, &wifi_cfg);
    assert(netif);
    esp_wifi_set_default_wifi_ap_handlers();

    ESP_LOGI(TAG, "WiFi AP netif created (bridge mode)");
    return netif;
}

esp_err_t wifi_ap_start(const netlink_config_t *cfg)
{
    s_ap_cfg = cfg;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    uint8_t channel = cfg->wifi_channel;
    if (channel == 0 || channel > 13) channel = 1;

    wifi_config_t wifi_config = {
        .ap = {
            .channel = channel,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = true,
            },
        },
    };

    size_t ssid_len = strlen(cfg->wifi_ssid);
    memcpy(wifi_config.ap.ssid, cfg->wifi_ssid, ssid_len);
    wifi_config.ap.ssid_len = ssid_len;
    memcpy(wifi_config.ap.password, cfg->wifi_password, strlen(cfg->wifi_password));

    if (strlen(cfg->wifi_password) < 8) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGW(TAG, "Password too short, using open auth");
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_ap_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    int8_t tx_power = cfg->wifi_tx_power;
    if (tx_power < 8) tx_power = 8;
    if (tx_power > 80) tx_power = 80;
    esp_wifi_set_max_tx_power(tx_power);

    ESP_LOGI(TAG, "WiFi AP started: SSID=%s ch=%d tx=%.1fdBm",
             cfg->wifi_ssid, channel, tx_power * 0.25f);
    return ESP_OK;
}
