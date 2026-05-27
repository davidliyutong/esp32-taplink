#pragma once

#include "esp_err.h"
#include <stdint.h>

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char admin_password[65];
    uint32_t usb_subnet;
    uint8_t usb_prefix_len;
    uint32_t wifi_subnet;
    uint8_t wifi_prefix_len;
    uint8_t dhcp_gw_enabled;
    uint8_t dhcp_dns_enabled;
    int8_t wifi_tx_power;
    uint8_t wifi_channel;
} netlink_config_t;

void config_get_defaults(netlink_config_t *cfg);
esp_err_t config_load(netlink_config_t *cfg);
esp_err_t config_save(const netlink_config_t *cfg);
