#include "nvs_config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "config";
static const char *NVS_NAMESPACE = "taplink";

static uint32_t prefix_to_netmask(uint8_t prefix_len)
{
    if (prefix_len == 0) {
        return 0;
    }
    if (prefix_len >= 32) {
        return 0xFFFFFFFF;
    }
    return htonl(0xFFFFFFFF << (32 - prefix_len));
}

static bool ip_in_subnet(uint32_t ip, uint32_t subnet, uint8_t prefix_len)
{
    uint32_t mask = prefix_to_netmask(prefix_len);
    return (ip & mask) == (subnet & mask);
}

static bool subnets_overlap(uint32_t subnet_a, uint8_t prefix_a, uint32_t subnet_b, uint8_t prefix_b)
{
    uint32_t a = ntohl(subnet_a);
    uint32_t b = ntohl(subnet_b);
    uint32_t mask_a = ntohl(prefix_to_netmask(prefix_a));
    uint32_t mask_b = ntohl(prefix_to_netmask(prefix_b));

    return ((a & mask_a) == (b & mask_a)) || ((a & mask_b) == (b & mask_b));
}

static bool network_is_valid(uint32_t subnet, uint8_t prefix_len)
{
    if (prefix_len < 8 || prefix_len > 29) {
        return false;
    }
    uint32_t mask = prefix_to_netmask(prefix_len);
    return (subnet & ~mask) == 0;
}

static void config_sanitize(taplink_config_t *cfg)
{
    taplink_config_t defaults;
    config_get_defaults(&defaults);

    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    cfg->wifi_password[sizeof(cfg->wifi_password) - 1] = '\0';
    cfg->admin_password[sizeof(cfg->admin_password) - 1] = '\0';

    if (cfg->wifi_ssid[0] == '\0') {
        snprintf(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "%s", defaults.wifi_ssid);
    }
    if (strlen(cfg->wifi_password) > 63) {
        cfg->wifi_password[63] = '\0';
    }
    if (cfg->wifi_password[0] != '\0' && strlen(cfg->wifi_password) < 8) {
        snprintf(cfg->wifi_password, sizeof(cfg->wifi_password), "%s", defaults.wifi_password);
    }
    if (cfg->admin_password[0] == '\0') {
        snprintf(cfg->admin_password, sizeof(cfg->admin_password), "%s", defaults.admin_password);
    }

    if (!network_is_valid(cfg->usb_subnet, cfg->usb_prefix_len) ||
        !network_is_valid(cfg->wifi_subnet, cfg->wifi_prefix_len) ||
        subnets_overlap(cfg->usb_subnet, cfg->usb_prefix_len, cfg->wifi_subnet, cfg->wifi_prefix_len)) {
        ESP_LOGW(TAG, "Invalid saved network config, restoring default subnets");
        cfg->usb_subnet = defaults.usb_subnet;
        cfg->usb_prefix_len = defaults.usb_prefix_len;
        cfg->wifi_subnet = defaults.wifi_subnet;
        cfg->wifi_prefix_len = defaults.wifi_prefix_len;
    }

    cfg->usb_subnet &= prefix_to_netmask(cfg->usb_prefix_len);
    cfg->wifi_subnet &= prefix_to_netmask(cfg->wifi_prefix_len);
    cfg->dhcp_gw_enabled = cfg->dhcp_gw_enabled ? 1 : 0;
    cfg->dhcp_dns_enabled = cfg->dhcp_dns_enabled ? 1 : 0;

    if (cfg->wifi_tx_power < 8 || cfg->wifi_tx_power > 80) {
        cfg->wifi_tx_power = defaults.wifi_tx_power;
    }
    if (cfg->wifi_channel > 13) {
        cfg->wifi_channel = defaults.wifi_channel;
    }

    uint16_t used_listen_ports[TAPLINK_MAX_PORT_FORWARDS] = {0};
    size_t used_count = 0;
    uint32_t usb_mask = prefix_to_netmask(cfg->usb_prefix_len);
    uint32_t usb_network = cfg->usb_subnet & usb_mask;
    uint32_t usb_gateway = usb_network | htonl(1);
    uint32_t usb_broadcast = usb_network | ~usb_mask;

    for (int i = 0; i < TAPLINK_MAX_PORT_FORWARDS; i++) {
        port_forward_rule_t *rule = &cfg->port_forwards[i];
        if (!rule->enabled) {
            memset(rule, 0, sizeof(*rule));
            continue;
        }

        bool duplicate = false;
        for (size_t j = 0; j < used_count; j++) {
            if (used_listen_ports[j] == rule->listen_port) {
                duplicate = true;
                break;
            }
        }

        bool invalid = duplicate || rule->listen_port == 0 || rule->listen_port == 80 || rule->target_port == 0 ||
                       rule->target_ip == 0 || !ip_in_subnet(rule->target_ip, cfg->usb_subnet, cfg->usb_prefix_len) ||
                       rule->target_ip == usb_network || rule->target_ip == usb_gateway ||
                       rule->target_ip == usb_broadcast;
        if (invalid) {
            ESP_LOGW(TAG, "Dropping invalid port-forward rule %d from NVS", i);
            memset(rule, 0, sizeof(*rule));
            continue;
        }

        rule->enabled = 1;
        used_listen_ports[used_count++] = rule->listen_port;
    }
}

void config_get_defaults(taplink_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->wifi_ssid, "ESP32-TapLink");
    strcpy(cfg->wifi_password, "12345678");
    strcpy(cfg->admin_password, "admin");
    cfg->usb_subnet = inet_addr("192.168.5.0");
    cfg->usb_prefix_len = 24;
    cfg->wifi_subnet = inet_addr("192.168.4.0");
    cfg->wifi_prefix_len = 24;
    cfg->dhcp_gw_enabled = 0;
    cfg->dhcp_dns_enabled = 0;
    cfg->wifi_tx_power = 44;
    cfg->wifi_channel = 0;
}

esp_err_t config_load(taplink_config_t *cfg)
{
    config_get_defaults(cfg);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved config, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK)
        return err;

    size_t len;

    len = sizeof(cfg->wifi_ssid);
    nvs_get_str(handle, "wifi_ssid", cfg->wifi_ssid, &len);

    len = sizeof(cfg->wifi_password);
    nvs_get_str(handle, "wifi_pass", cfg->wifi_password, &len);

    len = sizeof(cfg->admin_password);
    nvs_get_str(handle, "admin_pass", cfg->admin_password, &len);

    nvs_get_u32(handle, "usb_ip", &cfg->usb_subnet);
    nvs_get_u8(handle, "usb_pfx", &cfg->usb_prefix_len);

    err = nvs_get_u32(handle, "wifi_ip", &cfg->wifi_subnet);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_get_u32(handle, "dhcp_ip", &cfg->wifi_subnet);
    }

    err = nvs_get_u8(handle, "wifi_pfx", &cfg->wifi_prefix_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_get_u8(handle, "dhcp_pfx", &cfg->wifi_prefix_len);
    }

    nvs_get_u8(handle, "dhcp_gw", &cfg->dhcp_gw_enabled);
    nvs_get_u8(handle, "dhcp_dns", &cfg->dhcp_dns_enabled);
    nvs_get_i8(handle, "wifi_txp", &cfg->wifi_tx_power);
    nvs_get_u8(handle, "wifi_ch", &cfg->wifi_channel);

    for (int i = 0; i < TAPLINK_MAX_PORT_FORWARDS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "pf%d_enabled", i);
        nvs_get_u8(handle, key, &cfg->port_forwards[i].enabled);
        snprintf(key, sizeof(key), "pf%d_lport", i);
        nvs_get_u16(handle, key, &cfg->port_forwards[i].listen_port);
        snprintf(key, sizeof(key), "pf%d_tip", i);
        nvs_get_u32(handle, key, &cfg->port_forwards[i].target_ip);
        snprintf(key, sizeof(key), "pf%d_tport", i);
        nvs_get_u16(handle, key, &cfg->port_forwards[i].target_port);
    }

    nvs_close(handle);
    config_sanitize(cfg);

    uint8_t *usb = (uint8_t *)&cfg->usb_subnet;
    uint8_t *wifi = (uint8_t *)&cfg->wifi_subnet;
    ESP_LOGI(TAG, "Config loaded: SSID=%s usb=%d.%d.%d.%d/%d wifi=%d.%d.%d.%d/%d", cfg->wifi_ssid, usb[0], usb[1],
             usb[2], usb[3], cfg->usb_prefix_len, wifi[0], wifi[1], wifi[2], wifi[3], cfg->wifi_prefix_len);
    return ESP_OK;
}

esp_err_t config_save(const taplink_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;

#define CHECK_NVS(op)                                                                                                  \
    do {                                                                                                               \
        err = (op);                                                                                                    \
        if (err != ESP_OK)                                                                                             \
            goto done;                                                                                                 \
    } while (0)
    CHECK_NVS(nvs_set_str(handle, "wifi_ssid", cfg->wifi_ssid));
    CHECK_NVS(nvs_set_str(handle, "wifi_pass", cfg->wifi_password));
    CHECK_NVS(nvs_set_str(handle, "admin_pass", cfg->admin_password));
    CHECK_NVS(nvs_set_u32(handle, "usb_ip", cfg->usb_subnet));
    CHECK_NVS(nvs_set_u8(handle, "usb_pfx", cfg->usb_prefix_len));
    CHECK_NVS(nvs_set_u32(handle, "wifi_ip", cfg->wifi_subnet));
    CHECK_NVS(nvs_set_u8(handle, "wifi_pfx", cfg->wifi_prefix_len));
    CHECK_NVS(nvs_set_u32(handle, "dhcp_ip", cfg->wifi_subnet));
    CHECK_NVS(nvs_set_u8(handle, "dhcp_pfx", cfg->wifi_prefix_len));
    CHECK_NVS(nvs_set_u8(handle, "dhcp_gw", cfg->dhcp_gw_enabled));
    CHECK_NVS(nvs_set_u8(handle, "dhcp_dns", cfg->dhcp_dns_enabled));
    CHECK_NVS(nvs_set_i8(handle, "wifi_txp", cfg->wifi_tx_power));
    CHECK_NVS(nvs_set_u8(handle, "wifi_ch", cfg->wifi_channel));

    for (int i = 0; i < TAPLINK_MAX_PORT_FORWARDS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "pf%d_enabled", i);
        CHECK_NVS(nvs_set_u8(handle, key, cfg->port_forwards[i].enabled));
        snprintf(key, sizeof(key), "pf%d_lport", i);
        CHECK_NVS(nvs_set_u16(handle, key, cfg->port_forwards[i].listen_port));
        snprintf(key, sizeof(key), "pf%d_tip", i);
        CHECK_NVS(nvs_set_u32(handle, key, cfg->port_forwards[i].target_ip));
        snprintf(key, sizeof(key), "pf%d_tport", i);
        CHECK_NVS(nvs_set_u16(handle, key, cfg->port_forwards[i].target_port));
    }

    err = nvs_commit(handle);
done:
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config saved");
    } else {
        ESP_LOGE(TAG, "Config save failed: %s", esp_err_to_name(err));
    }
#undef CHECK_NVS
    return err;
}
