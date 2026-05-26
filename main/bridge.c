#include "bridge.h"
#include "esp_netif.h"
#include "esp_netif_br_glue.h"
#include "esp_event.h"
#include "esp_eth_com.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <arpa/inet.h>

static const char *TAG = "bridge";

static esp_netif_t *s_br_netif;
static const netlink_config_t *s_cfg;

static uint32_t prefix_to_netmask(uint8_t prefix_len)
{
    if (prefix_len == 0) return 0;
    if (prefix_len >= 32) return 0xFFFFFFFF;
    return htonl(0xFFFFFFFF << (32 - prefix_len));
}

esp_netif_t *bridge_create(const netlink_config_t *cfg)
{
    s_cfg = cfg;

    esp_netif_ip_info_t ip_info = {
        .ip.addr = cfg->dhcp_subnet | htonl(1),
        .netmask.addr = prefix_to_netmask(cfg->dhcp_prefix_len),
    };
    ip_info.gw.addr = cfg->dhcp_gw_enabled ? ip_info.ip.addr : 0;

    bridgeif_config_t br_if_cfg = {
        .max_fdb_dyn_entries = 10,
        .max_fdb_sta_entries = 2,
        .max_ports = 2,
    };

    esp_netif_inherent_config_t br_inherent = ESP_NETIF_INHERENT_DEFAULT_BR_DHCPS();
    br_inherent.ip_info = &ip_info;
    br_inherent.bridge_info = &br_if_cfg;

    esp_netif_config_t br_cfg = {
        .base = &br_inherent,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_BR,
    };

    s_br_netif = esp_netif_new(&br_cfg);
    assert(s_br_netif);

    ESP_LOGI(TAG, "Bridge created, IP=" IPSTR " gw=%s dns=%s",
             IP2STR(&ip_info.ip),
             cfg->dhcp_gw_enabled ? "on" : "off",
             cfg->dhcp_dns_enabled ? "on" : "off");
    return s_br_netif;
}

esp_err_t bridge_start(esp_netif_t *br_netif, esp_netif_t *usb_netif, esp_netif_t *wifi_netif)
{
    esp_netif_br_glue_handle_t glue = esp_netif_br_glue_new();

    ESP_ERROR_CHECK(esp_netif_br_glue_add_port(glue, usb_netif));
    ESP_ERROR_CHECK(esp_netif_br_glue_add_wifi_port(glue, wifi_netif));
    ESP_ERROR_CHECK(esp_netif_attach(br_netif, glue));

    ESP_LOGI(TAG, "Bridge started with USB NCM + WiFi AP ports");
    return ESP_OK;
}

esp_netif_t *bridge_get_netif(void)
{
    return s_br_netif;
}
