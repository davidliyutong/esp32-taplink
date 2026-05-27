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
    cfg->usb_subnet = inet_addr("192.168.5.0");
    cfg->usb_prefix_len = 24;
    cfg->wifi_subnet = inet_addr("192.168.4.0");
    cfg->wifi_prefix_len = 24;
    cfg->dhcp_gw_enabled = 1;
    cfg->dhcp_dns_enabled = 0;
    cfg->wifi_tx_power = 80;
    cfg->wifi_channel = 0;
}

static uint8_t sanitize_prefix(uint8_t prefix_len)
{
    return (prefix_len >= 8 && prefix_len <= 29) ? prefix_len : 24;
}

static uint32_t normalize_subnet(uint32_t subnet, uint8_t prefix_len)
{
    uint32_t mask = 0xFFFFFFFFUL << (32 - prefix_len);
    return htonl(ntohl(subnet) & mask);
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

    nvs_get_u32(handle, "usb_ip", &cfg->usb_subnet);
    nvs_get_u8(handle, "usb_pfx", &cfg->usb_prefix_len);

    if (nvs_get_u32(handle, "wifi_ip", &cfg->wifi_subnet) != ESP_OK) {
        nvs_get_u32(handle, "dhcp_ip", &cfg->wifi_subnet);
    }
    if (nvs_get_u8(handle, "wifi_pfx", &cfg->wifi_prefix_len) != ESP_OK) {
        nvs_get_u8(handle, "dhcp_pfx", &cfg->wifi_prefix_len);
    }

    nvs_get_u8(handle, "dhcp_gw", &cfg->dhcp_gw_enabled);
    nvs_get_u8(handle, "dhcp_dns", &cfg->dhcp_dns_enabled);
    nvs_get_i8(handle, "wifi_txp", &cfg->wifi_tx_power);
    nvs_get_u8(handle, "wifi_ch", &cfg->wifi_channel);

    nvs_close(handle);

    cfg->usb_prefix_len = sanitize_prefix(cfg->usb_prefix_len);
    cfg->wifi_prefix_len = sanitize_prefix(cfg->wifi_prefix_len);
    cfg->usb_subnet = normalize_subnet(cfg->usb_subnet, cfg->usb_prefix_len);
    cfg->wifi_subnet = normalize_subnet(cfg->wifi_subnet, cfg->wifi_prefix_len);

    uint8_t *usb = (uint8_t *)&cfg->usb_subnet;
    uint8_t *wifi = (uint8_t *)&cfg->wifi_subnet;
    ESP_LOGI(TAG, "Config loaded: SSID=%s usb=%d.%d.%d.%d/%d wifi=%d.%d.%d.%d/%d",
             cfg->wifi_ssid,
             usb[0], usb[1], usb[2], usb[3], cfg->usb_prefix_len,
             wifi[0], wifi[1], wifi[2], wifi[3], cfg->wifi_prefix_len);
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
    nvs_set_u32(handle, "usb_ip", cfg->usb_subnet);
    nvs_set_u8(handle, "usb_pfx", cfg->usb_prefix_len);
    nvs_set_u32(handle, "wifi_ip", cfg->wifi_subnet);
    nvs_set_u8(handle, "wifi_pfx", cfg->wifi_prefix_len);
    nvs_set_u32(handle, "dhcp_ip", cfg->wifi_subnet);
    nvs_set_u8(handle, "dhcp_pfx", cfg->wifi_prefix_len);
    nvs_set_u8(handle, "dhcp_gw", cfg->dhcp_gw_enabled);
    nvs_set_u8(handle, "dhcp_dns", cfg->dhcp_dns_enabled);
    nvs_set_i8(handle, "wifi_txp", cfg->wifi_tx_power);
    nvs_set_u8(handle, "wifi_ch", cfg->wifi_channel);

    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Config saved");
    return err;
}
