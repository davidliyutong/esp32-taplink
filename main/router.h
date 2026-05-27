#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include "nvs_config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROUTER_MAX_DHCP_LEASES 16
#define ROUTER_DHCP_LEASE_TIME_UNITS 60U

typedef struct
{
    uint32_t ip;
    uint8_t mac[6];
    const char *iface;
    uint32_t expires_in_seconds;
} router_dhcp_lease_t;

uint32_t router_prefix_to_netmask(uint8_t prefix_len);
bool router_subnets_overlap(uint32_t subnet_a, uint8_t prefix_a, uint32_t subnet_b, uint8_t prefix_b);
void router_make_ip_info(uint32_t subnet, uint8_t prefix_len, bool advertise_gw, esp_netif_ip_info_t *ip_info);
esp_err_t router_configure_dhcps(esp_netif_t *netif, uint32_t subnet, uint8_t prefix_len, bool advertise_gw,
                                 bool advertise_dns, const char *name);
esp_err_t router_start(esp_netif_t *usb_netif, esp_netif_t *wifi_netif, const netlink_config_t *cfg);
esp_netif_t *router_get_usb_netif(void);
esp_netif_t *router_get_wifi_netif(void);
size_t router_get_dhcp_leases(router_dhcp_lease_t *out, size_t max_leases);
