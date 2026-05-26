#include "wifi_ap.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wifi_ap";

esp_netif_t *wifi_ap_create(void)
{
    esp_netif_inherent_config_t wifi_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    wifi_cfg.flags = ESP_NETIF_FLAG_AUTOUP;
    wifi_cfg.ip_info = NULL;

    esp_netif_t *netif = esp_netif_create_wifi(WIFI_IF_AP, &wifi_cfg);
    assert(netif);
    esp_wifi_set_default_wifi_ap_handlers();

    ESP_LOGI(TAG, "WiFi AP netif created (bridge mode)");
    return netif;
}

esp_err_t wifi_ap_start(const netlink_config_t *cfg)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t wifi_config = {
        .ap = {
            .channel = 1,
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
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started: SSID=%s", cfg->wifi_ssid);
    return ESP_OK;
}
