#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include "nvs_config.h"
#include <stdbool.h>
#include <stdint.h>

uint32_t router_prefix_to_netmask(uint8_t prefix_len);
void router_make_ip_info(uint32_t subnet, uint8_t prefix_len, bool advertise_gw,
                         esp_netif_ip_info_t *ip_info);
esp_err_t router_configure_dhcps(esp_netif_t *netif, uint32_t subnet,
                                 uint8_t prefix_len, bool advertise_gw,
                                 bool advertise_dns, const char *name);
esp_err_t router_start(esp_netif_t *usb_netif, esp_netif_t *wifi_netif,
                       const netlink_config_t *cfg);
esp_netif_t *router_get_usb_netif(void);
esp_netif_t *router_get_wifi_netif(void);
