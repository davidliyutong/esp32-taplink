#include "router.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "lwip/prot/dhcp.h"
#include "sdkconfig.h"
#include <arpa/inet.h>
#include <inttypes.h>
#include <string.h>

static const char *TAG = "router";

enum {
    DHCP_OPTION_STATIC_ROUTE = 33,
    DHCP_OPTION_CLASSLESS_STATIC_ROUTE = 121,
    DHCP_OPTION_MS_CLASSLESS_STATIC_ROUTE = 249,
    ROUTER_DHCP_LEASE_SECONDS =
        ROUTER_DHCP_LEASE_TIME_UNITS * CONFIG_LWIP_DHCPS_LEASE_UNIT,
    DHCPS_STATE_DECLINE_LOCAL = 2,
    DHCPS_STATE_NAK_LOCAL = 4,
    DHCPS_STATE_RELEASE_LOCAL = 6,
};

typedef enum {
    ROUTER_LEASE_IFACE_UNKNOWN = 0,
    ROUTER_LEASE_IFACE_USB,
    ROUTER_LEASE_IFACE_WIFI,
} router_lease_iface_t;

typedef struct {
    bool active;
    router_lease_iface_t iface;
    uint32_t ip;
    uint8_t mac[6];
    TickType_t last_seen;
} router_dhcp_lease_slot_t;

static esp_netif_t *s_usb_netif;
static esp_netif_t *s_wifi_netif;
static const netlink_config_t *s_cfg;
static router_dhcp_lease_slot_t s_dhcp_leases[ROUTER_MAX_DHCP_LEASES];
static portMUX_TYPE s_dhcp_leases_lock = portMUX_INITIALIZER_UNLOCKED;

static TickType_t dhcp_lease_duration_ticks(void)
{
    return pdMS_TO_TICKS((uint32_t)ROUTER_DHCP_LEASE_SECONDS * 1000U);
}

static bool dhcp_lease_expired(const router_dhcp_lease_slot_t *lease,
                               TickType_t now)
{
    return lease->active &&
           (TickType_t)(now - lease->last_seen) >= dhcp_lease_duration_ticks();
}

static const char *lease_iface_name(router_lease_iface_t iface)
{
    switch (iface) {
    case ROUTER_LEASE_IFACE_USB:
        return "USB";
    case ROUTER_LEASE_IFACE_WIFI:
        return "WiFi";
    default:
        return "Unknown";
    }
}

static router_lease_iface_t lease_iface_from_netif(esp_netif_t *netif)
{
    if (netif == s_usb_netif) {
        return ROUTER_LEASE_IFACE_USB;
    }
    if (netif == s_wifi_netif) {
        return ROUTER_LEASE_IFACE_WIFI;
    }
    return ROUTER_LEASE_IFACE_UNKNOWN;
}

static void prune_expired_leases_locked(TickType_t now)
{
    for (size_t i = 0; i < ROUTER_MAX_DHCP_LEASES; i++) {
        if (dhcp_lease_expired(&s_dhcp_leases[i], now)) {
            s_dhcp_leases[i].active = false;
        }
    }
}

static void clear_dhcp_leases(void)
{
    portENTER_CRITICAL(&s_dhcp_leases_lock);
    memset(s_dhcp_leases, 0, sizeof(s_dhcp_leases));
    portEXIT_CRITICAL(&s_dhcp_leases_lock);
}

static void record_dhcp_lease(router_lease_iface_t iface, uint32_t ip,
                              const uint8_t mac[6])
{
    if (iface == ROUTER_LEASE_IFACE_UNKNOWN || ip == 0) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    int match = -1;
    int empty = -1;

    portENTER_CRITICAL(&s_dhcp_leases_lock);
    prune_expired_leases_locked(now);

    for (size_t i = 0; i < ROUTER_MAX_DHCP_LEASES; i++) {
        router_dhcp_lease_slot_t *lease = &s_dhcp_leases[i];
        if (!lease->active) {
            if (empty < 0) {
                empty = (int)i;
            }
            continue;
        }
        if (lease->iface == iface &&
            (lease->ip == ip || memcmp(lease->mac, mac, sizeof(lease->mac)) == 0)) {
            match = (int)i;
            break;
        }
    }

    int slot = match >= 0 ? match : empty;
    if (slot >= 0) {
        s_dhcp_leases[slot] = (router_dhcp_lease_slot_t) {
            .active = true,
            .iface = iface,
            .ip = ip,
            .last_seen = now,
        };
        memcpy(s_dhcp_leases[slot].mac, mac, sizeof(s_dhcp_leases[slot].mac));
    }
    portEXIT_CRITICAL(&s_dhcp_leases_lock);
}

static void remove_dhcp_lease(uint32_t ip, const uint8_t mac[6])
{
    portENTER_CRITICAL(&s_dhcp_leases_lock);
    for (size_t i = 0; i < ROUTER_MAX_DHCP_LEASES; i++) {
        router_dhcp_lease_slot_t *lease = &s_dhcp_leases[i];
        if (!lease->active) {
            continue;
        }
        bool ip_matches = ip != 0 && lease->ip == ip;
        bool mac_matches = memcmp(lease->mac, mac, sizeof(lease->mac)) == 0;
        if (mac_matches && (ip == 0 || ip_matches)) {
            lease->active = false;
        }
    }
    portEXIT_CRITICAL(&s_dhcp_leases_lock);
}

static void dhcp_assigned_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id != IP_EVENT_AP_STAIPASSIGNED || !event_data) {
        return;
    }

    ip_event_ap_staipassigned_t *event = event_data;
    record_dhcp_lease(lease_iface_from_netif(event->esp_netif),
                      event->ip.addr, event->mac);
}

uint32_t router_prefix_to_netmask(uint8_t prefix_len)
{
    if (prefix_len == 0) {
        return 0;
    }
    if (prefix_len >= 32) {
        return 0xFFFFFFFF;
    }
    return htonl(0xFFFFFFFF << (32 - prefix_len));
}

bool router_subnets_overlap(uint32_t subnet_a, uint8_t prefix_a,
                            uint32_t subnet_b, uint8_t prefix_b)
{
    uint32_t a = ntohl(subnet_a);
    uint32_t b = ntohl(subnet_b);
    uint32_t mask_a = ntohl(router_prefix_to_netmask(prefix_a));
    uint32_t mask_b = ntohl(router_prefix_to_netmask(prefix_b));

    return ((a & mask_a) == (b & mask_a)) || ((a & mask_b) == (b & mask_b));
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

    uint32_t lease_time = ROUTER_DHCP_LEASE_TIME_UNITS;
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

static uint8_t classful_prefix_len(uint32_t subnet)
{
    uint8_t first_octet = (uint8_t)(ntohl(subnet) >> 24);

    if (first_octet < 128) {
        return 8;
    }
    if (first_octet < 192) {
        return 16;
    }
    if (first_octet < 224) {
        return 24;
    }
    return 0;
}

static uint8_t *append_static_route_option(uint8_t *opt, uint32_t subnet,
                                           uint32_t router)
{
    *opt++ = DHCP_OPTION_STATIC_ROUTE;
    *opt++ = 8;
    memcpy(opt, &subnet, 4);
    opt += 4;
    memcpy(opt, &router, 4);
    opt += 4;

    return opt;
}

static uint8_t *append_classless_route_option(uint8_t *opt, uint8_t option,
                                              uint32_t subnet,
                                              uint8_t prefix_len,
                                              uint32_t router)
{
    uint8_t subnet_bytes = (prefix_len + 7) / 8;

    *opt++ = option;
    *opt++ = 1 + subnet_bytes + 4;
    *opt++ = prefix_len;
    memcpy(opt, &subnet, subnet_bytes);
    opt += subnet_bytes;
    memcpy(opt, &router, 4);
    opt += 4;

    return opt;
}

static void append_route_options(uint8_t **opts, uint32_t route_subnet,
                                 uint8_t route_prefix_len, uint32_t router)
{
    uint8_t *opt = *opts;

    opt = append_classless_route_option(opt, DHCP_OPTION_CLASSLESS_STATIC_ROUTE,
                                        route_subnet, route_prefix_len, router);
    opt = append_classless_route_option(opt, DHCP_OPTION_MS_CLASSLESS_STATIC_ROUTE,
                                        route_subnet, route_prefix_len, router);

    if (route_prefix_len == classful_prefix_len(route_subnet)) {
        opt = append_static_route_option(opt, route_subnet, router);
    }

    *opts = opt;
}

void router_append_dhcps_routes(struct netif *netif, u8_t state, u8_t **opts)
{
    if (!s_cfg || !netif || !opts || !*opts) {
        return;
    }
    if (state != DHCP_OFFER && state != DHCP_ACK) {
        return;
    }
    if (router_subnets_overlap(s_cfg->usb_subnet, s_cfg->usb_prefix_len,
                               s_cfg->wifi_subnet, s_cfg->wifi_prefix_len)) {
        return;
    }

    if (s_usb_netif && netif == esp_netif_get_netif_impl(s_usb_netif)) {
        esp_netif_ip_info_t ip_info;
        router_make_ip_info(s_cfg->usb_subnet, s_cfg->usb_prefix_len,
                            true, &ip_info);
        append_route_options(opts, s_cfg->wifi_subnet, s_cfg->wifi_prefix_len,
                             ip_info.ip.addr);
    } else if (s_wifi_netif && netif == esp_netif_get_netif_impl(s_wifi_netif)) {
        esp_netif_ip_info_t ip_info;
        router_make_ip_info(s_cfg->wifi_subnet, s_cfg->wifi_prefix_len,
                            true, &ip_info);
        append_route_options(opts, s_cfg->usb_subnet, s_cfg->usb_prefix_len,
                             ip_info.ip.addr);
    }
}

s16_t router_observe_dhcps_state(struct dhcps_msg *msg, u16_t len, s16_t state)
{
    (void)len;

    if (!msg) {
        return state;
    }

    if (state == DHCPS_STATE_RELEASE_LOCAL ||
        state == DHCPS_STATE_DECLINE_LOCAL ||
        state == DHCPS_STATE_NAK_LOCAL) {
        uint32_t ciaddr = 0;
        memcpy(&ciaddr, msg->ciaddr, sizeof(ciaddr));
        remove_dhcp_lease(ciaddr, msg->chaddr);
    }

    return state;
}

esp_err_t router_start(esp_netif_t *usb_netif, esp_netif_t *wifi_netif,
                       const netlink_config_t *cfg)
{
    s_usb_netif = usb_netif;
    s_wifi_netif = wifi_netif;
    s_cfg = cfg;
    clear_dhcp_leases();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                               dhcp_assigned_handler, NULL));

    if (router_subnets_overlap(cfg->usb_subnet, cfg->usb_prefix_len,
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

size_t router_get_dhcp_leases(router_dhcp_lease_t *out, size_t max_leases)
{
    if (!out || max_leases == 0) {
        return 0;
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t lease_ticks = dhcp_lease_duration_ticks();
    size_t count = 0;

    portENTER_CRITICAL(&s_dhcp_leases_lock);
    prune_expired_leases_locked(now);

    for (size_t i = 0; i < ROUTER_MAX_DHCP_LEASES && count < max_leases; i++) {
        router_dhcp_lease_slot_t *lease = &s_dhcp_leases[i];
        if (!lease->active) {
            continue;
        }

        TickType_t elapsed = now - lease->last_seen;
        TickType_t remaining = elapsed >= lease_ticks ? 0 : lease_ticks - elapsed;
        out[count] = (router_dhcp_lease_t) {
            .ip = lease->ip,
            .iface = lease_iface_name(lease->iface),
            .expires_in_seconds = remaining / configTICK_RATE_HZ,
        };
        memcpy(out[count].mac, lease->mac, sizeof(out[count].mac));
        count++;
    }

    portEXIT_CRITICAL(&s_dhcp_leases_lock);
    return count;
}
