#include "router.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <arpa/inet.h>
#include <inttypes.h>
#include <string.h>

static const char *TAG = "router";

static esp_netif_t *s_usb_netif;
static esp_netif_t *s_wifi_netif;

uint32_t router_prefix_to_netmask(uint8_t prefix_len)
{
    if (prefix_len == 0) return 0;
    if (prefix_len >= 32) return 0xFFFFFFFF;
    return htonl(0xFFFFFFFF << (32 - prefix_len));
}

void router_make_ip_info(uint32_t subnet, uint8_t prefix_len, bool advertise_gw,
                         esp_netif_ip_info_t *ip_info)
{
    uint32_t network = subnet & router_prefix_to_netmask(prefix_len);

    memset(ip_info, 0, sizeof(*ip_info));
    ip_info->ip.addr = network | htonl(1);
    ip_info->netmask.addr = router_prefix_to_netmask(prefix_len);
    ip_info->gw.addr = advertise_gw ? ip_info->ip.addr : 0;
}

esp_err_t router_configure_dhcps(esp_netif_t *netif, uint32_t subnet,
                                 uint8_t prefix_len, bool advertise_gw,
                                 bool advertise_dns, const char *name)
{
    if (prefix_len < 8 || prefix_len > 29) {
        ESP_LOGE(TAG, "%s: unsupported prefix /%d for DHCP server", name, prefix_len);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t network = subnet & router_prefix_to_netmask(prefix_len);
    uint32_t host_count = (1UL << (32 - prefix_len)) - 2;
    uint32_t pool_end_host = host_count < 11 ? host_count : 11;

    esp_netif_ip_info_t ip_info;
    router_make_ip_info(network, prefix_len, advertise_gw, &ip_info);

    uint32_t lease_time = 60;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_IP_ADDRESS_LEASE_TIME,
                                           &lease_time, sizeof(lease_time)));

    dhcps_lease_t pool = {
        .enable = true,
        .start_ip.addr = network | htonl(2),
        .end_ip.addr = network | htonl(pool_end_host),
    };
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_REQUESTED_IP_ADDRESS,
                                           &pool, sizeof(pool)));

    dhcps_offer_t router_offer = advertise_gw ? OFFER_ROUTER : OFFER_START;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
                                           &router_offer, sizeof(router_offer)));

    dhcps_offer_t dns_offer = advertise_dns ? OFFER_DNS : OFFER_START;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &dns_offer, sizeof(dns_offer)));

    if (advertise_dns) {
        esp_netif_dns_info_t dns = {
            .ip = {
                .u_addr.ip4.addr = ip_info.ip.addr,
                .type = ESP_IPADDR_TYPE_V4,
            },
        };
        ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));
    }

    ESP_LOGI(TAG, "%s: IP=" IPSTR " pool=.2-.%" PRIu32 " gw=%s dns=%s",
             name, IP2STR(&ip_info.ip),
             pool_end_host,
             advertise_gw ? "on" : "off",
             advertise_dns ? "on" : "off");
    return ESP_OK;
}

static bool subnets_overlap(uint32_t subnet_a, uint8_t prefix_a,
                            uint32_t subnet_b, uint8_t prefix_b)
{
    uint32_t a = ntohl(subnet_a);
    uint32_t b = ntohl(subnet_b);
    uint32_t mask_a = ntohl(router_prefix_to_netmask(prefix_a));
    uint32_t mask_b = ntohl(router_prefix_to_netmask(prefix_b));

    return ((a & mask_a) == (b & mask_a)) || ((a & mask_b) == (b & mask_b));
}

esp_err_t router_start(esp_netif_t *usb_netif, esp_netif_t *wifi_netif,
                       const netlink_config_t *cfg)
{
    s_usb_netif = usb_netif;
    s_wifi_netif = wifi_netif;

    if (subnets_overlap(cfg->usb_subnet, cfg->usb_prefix_len,
                        cfg->wifi_subnet, cfg->wifi_prefix_len)) {
        ESP_LOGW(TAG, "USB and WiFi subnets overlap; routed forwarding needs distinct subnets");
    }

#if CONFIG_LWIP_IP_FORWARD
    ESP_LOGI(TAG, "IPv4 forwarding enabled for USB NCM <-> WiFi AP routing");
#else
    ESP_LOGE(TAG, "CONFIG_LWIP_IP_FORWARD is disabled; clients can reach gateways but not the other subnet");
#endif

    return ESP_OK;
}

esp_netif_t *router_get_usb_netif(void)
{
    return s_usb_netif;
}

esp_netif_t *router_get_wifi_netif(void)
{
    return s_wifi_netif;
}
