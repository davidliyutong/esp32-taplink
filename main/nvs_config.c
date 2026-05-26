#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <arpa/inet.h>

static const char *TAG = "config";
static const char *NVS_NAMESPACE = "netlink";

void config_get_defaults(netlink_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->wifi_ssid, "ESP32-NetLink");
    strcpy(cfg->wifi_password, "12345678");
    strcpy(cfg->admin_password, "admin");
    cfg->dhcp_subnet = inet_addr("192.168.4.0");
    cfg->dhcp_prefix_len = 24;
    cfg->dhcp_gw_enabled = 1;
    cfg->dhcp_dns_enabled = 0;
}

esp_err_t config_load(netlink_config_t *cfg)
{
    config_get_defaults(cfg);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved config, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    size_t len;

    len = sizeof(cfg->wifi_ssid);
    nvs_get_str(handle, "wifi_ssid", cfg->wifi_ssid, &len);

    len = sizeof(cfg->wifi_password);
    nvs_get_str(handle, "wifi_pass", cfg->wifi_password, &len);

    len = sizeof(cfg->admin_password);
    nvs_get_str(handle, "admin_pass", cfg->admin_password, &len);

    nvs_get_u32(handle, "dhcp_ip", &cfg->dhcp_subnet);
    nvs_get_u8(handle, "dhcp_pfx", &cfg->dhcp_prefix_len);
    nvs_get_u8(handle, "dhcp_gw", &cfg->dhcp_gw_enabled);
    nvs_get_u8(handle, "dhcp_dns", &cfg->dhcp_dns_enabled);

    nvs_close(handle);

    uint8_t *ip = (uint8_t *)&cfg->dhcp_subnet;
    ESP_LOGI(TAG, "Config loaded: SSID=%s subnet=%d.%d.%d.%d/%d",
             cfg->wifi_ssid, ip[0], ip[1], ip[2], ip[3], cfg->dhcp_prefix_len);
    return ESP_OK;
}

esp_err_t config_save(const netlink_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_str(handle, "wifi_ssid", cfg->wifi_ssid);
    nvs_set_str(handle, "wifi_pass", cfg->wifi_password);
    nvs_set_str(handle, "admin_pass", cfg->admin_password);
    nvs_set_u32(handle, "dhcp_ip", cfg->dhcp_subnet);
    nvs_set_u8(handle, "dhcp_pfx", cfg->dhcp_prefix_len);
    nvs_set_u8(handle, "dhcp_gw", cfg->dhcp_gw_enabled);
    nvs_set_u8(handle, "dhcp_dns", cfg->dhcp_dns_enabled);

    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Config saved");
    return err;
}
