#include "web_server.h"
#include "bridge.h"
#include "usb_ncm.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <arpa/inet.h>

static const char *TAG = "web";
static netlink_config_t *s_cfg;

/* ---- Log ring buffer ---- */

#define LOG_LINES 30
#define LOG_LINE_LEN 200

static char s_log_buf[LOG_LINES][LOG_LINE_LEN];
static int s_log_head;
static int s_log_count;
static SemaphoreHandle_t s_log_mutex;

static int log_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);

    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        vsnprintf(s_log_buf[s_log_head], LOG_LINE_LEN, fmt, copy);
        int len = strlen(s_log_buf[s_log_head]);
        while (len > 0 && (s_log_buf[s_log_head][len - 1] == '\n' ||
                           s_log_buf[s_log_head][len - 1] == '\r'))
            s_log_buf[s_log_head][--len] = '\0';
        s_log_head = (s_log_head + 1) % LOG_LINES;
        if (s_log_count < LOG_LINES) s_log_count++;
        xSemaphoreGive(s_log_mutex);
    }

    va_end(copy);
    return vprintf(fmt, args);
}

/* ---- Base64 decode (for HTTP Basic Auth) ---- */

static const uint8_t B64[256] = {
    ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
    ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
    ['Y']=24,['Z']=25,
    ['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,['g']=32,['h']=33,
    ['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,['o']=40,['p']=41,
    ['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,['w']=48,['x']=49,
    ['y']=50,['z']=51,
    ['0']=52,['1']=53,['2']=54,['3']=55,['4']=56,['5']=57,['6']=58,['7']=59,
    ['8']=60,['9']=61,['+']=62,['/']=63,
};

static int b64_decode(const char *in, size_t in_len, char *out, size_t out_max)
{
    size_t out_len = 0;
    for (size_t i = 0; i + 3 < in_len && out_len + 3 <= out_max; i += 4) {
        uint32_t v = (B64[(uint8_t)in[i]] << 18) |
                     (B64[(uint8_t)in[i+1]] << 12) |
                     (B64[(uint8_t)in[i+2]] << 6) |
                      B64[(uint8_t)in[i+3]];
        out[out_len++] = (v >> 16) & 0xFF;
        if (in[i+2] != '=') out[out_len++] = (v >> 8) & 0xFF;
        if (in[i+3] != '=') out[out_len++] = v & 0xFF;
    }
    if (out_len < out_max) out[out_len] = '\0';
    return out_len;
}

/* ---- Auth check ---- */

static bool check_auth(httpd_req_t *req)
{
    char auth[256];
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0 || hdr_len >= sizeof(auth)) return false;

    httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth));

    if (strncmp(auth, "Basic ", 6) != 0) return false;

    char decoded[128];
    b64_decode(auth + 6, strlen(auth + 6), decoded, sizeof(decoded));

    char expected[140];
    snprintf(expected, sizeof(expected), "admin:%s", s_cfg->admin_password);
    return strcmp(decoded, expected) == 0;
}

static esp_err_t send_auth_required(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-NetLink\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- URL decode ---- */

static void url_decode(char *dst, const char *src, size_t dst_max)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di < dst_max - 1; i++) {
        if (src[i] == '+') {
            dst[di++] = ' ';
        } else if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = {src[i+1], src[i+2], 0};
            dst[di++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = '\0';
}

static bool get_form_value(const char *body, const char *key, char *out, size_t out_max)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) return false;
    if (p != body && *(p - 1) != '&') return false;
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_max) len = out_max - 1;
    char encoded[256];
    memcpy(encoded, p, len);
    encoded[len] = '\0';
    url_decode(out, encoded, out_max);
    return true;
}

/* ---- HTML ---- */

static const char HTML_HEADER[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-NetLink</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:480px;margin:40px auto;padding:0 20px;background:#f5f5f5}"
    ".card{background:#fff;border-radius:8px;padding:24px;box-shadow:0 2px 4px rgba(0,0,0,.1);margin-bottom:16px}"
    "h1{color:#333;font-size:1.4em;margin:0 0 20px}"
    "h2{color:#555;font-size:1.1em;margin:0 0 12px}"
    "label{display:block;margin:12px 0 4px;color:#555;font-size:.9em}"
    "input[type=text],input[type=password]{width:100%;padding:8px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;font-size:1em}"
    "button{background:#2196F3;color:#fff;border:none;padding:10px 24px;border-radius:4px;cursor:pointer;font-size:1em;margin-top:16px;width:100%}"
    "button:hover{background:#1976D2}"
    ".msg{background:#E8F5E9;color:#2E7D32;padding:12px;border-radius:4px;margin-bottom:16px}"
    "table{width:100%;border-collapse:collapse;font-size:.9em}"
    "th,td{padding:8px;text-align:left;border-bottom:1px solid #eee}"
    "th{color:#888;font-weight:500}"
    ".toggle{display:flex;align-items:center;gap:8px;margin:12px 0}"
    ".toggle input{width:auto;margin:0}"
    "a{color:#2196F3}"
    ".empty{color:#999;font-style:italic;padding:12px 0}"
    "</style></head><body>";

static const char HTML_FOOTER[] = "</body></html>";

/* ---- DHCP client tracking ---- */

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
} client_entry_t;

#define MAX_DHCP_CLIENTS 10
#define DHCP_CLIENT_TIMEOUT_US \
    (((int64_t)BRIDGE_DHCP_LEASE_MINUTES + 5) * 60 * 1000000)

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    bool active;
    int64_t last_seen_us;
} dhcp_client_t;

static dhcp_client_t s_dhcp_clients[MAX_DHCP_CLIENTS];
static SemaphoreHandle_t s_dhcp_mutex;

static void wifi_disconnect_handler(void *arg, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    wifi_event_ap_stadisconnected_t *evt =
        (wifi_event_ap_stadisconnected_t *)event_data;
    if (!s_dhcp_mutex) return;

    xSemaphoreTake(s_dhcp_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_DHCP_CLIENTS; i++) {
        if (s_dhcp_clients[i].active &&
            memcmp(s_dhcp_clients[i].mac, evt->mac, 6) == 0) {
            s_dhcp_clients[i].active = false;
            break;
        }
    }
    xSemaphoreGive(s_dhcp_mutex);
}

static void dhcp_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    ip_event_ap_staipassigned_t *evt = (ip_event_ap_staipassigned_t *)event_data;
    if (!s_dhcp_mutex) return;

    xSemaphoreTake(s_dhcp_mutex, portMAX_DELAY);

    int free_slot = -1;
    for (int i = 0; i < MAX_DHCP_CLIENTS; i++) {
        if (s_dhcp_clients[i].active &&
            memcmp(s_dhcp_clients[i].mac, evt->mac, 6) == 0) {
            s_dhcp_clients[i].ip = evt->ip.addr;
            s_dhcp_clients[i].last_seen_us = esp_timer_get_time();
            xSemaphoreGive(s_dhcp_mutex);
            return;
        }
        if (!s_dhcp_clients[i].active && free_slot < 0)
            free_slot = i;
    }

    if (free_slot >= 0) {
        s_dhcp_clients[free_slot].ip = evt->ip.addr;
        memcpy(s_dhcp_clients[free_slot].mac, evt->mac, 6);
        s_dhcp_clients[free_slot].last_seen_us = esp_timer_get_time();
        s_dhcp_clients[free_slot].active = true;
    }

    xSemaphoreGive(s_dhcp_mutex);
}

static int get_clients(client_entry_t *out, int max_clients)
{
    if (!s_dhcp_mutex) return 0;

    xSemaphoreTake(s_dhcp_mutex, portMAX_DELAY);

    int64_t now = esp_timer_get_time();
    int count = 0;

    for (int i = 0; i < MAX_DHCP_CLIENTS && count < max_clients; i++) {
        if (s_dhcp_clients[i].active) {
            if (now - s_dhcp_clients[i].last_seen_us < DHCP_CLIENT_TIMEOUT_US) {
                out[count].ip = s_dhcp_clients[i].ip;
                memcpy(out[count].mac, s_dhcp_clients[i].mac, 6);
                count++;
            } else {
                s_dhcp_clients[i].active = false;
            }
        }
    }

    xSemaphoreGive(s_dhcp_mutex);
    return count;
}

/* ---- GET / (dashboard) ---- */

static esp_err_t dashboard_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    client_entry_t clients[10];
    int client_count = get_clients(clients, 10);

    char page[3072];
    int pos = 0;

    pos += snprintf(page + pos, sizeof(page) - pos,
        "%s<div class='card'><h1>ESP32-NetLink</h1>"
        "<h2>DHCP Clients</h2>",
        HTML_HEADER);
    if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;

    if (client_count == 0) {
        pos += snprintf(page + pos, sizeof(page) - pos,
            "<p class='empty'>No devices connected</p>");
        if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;
    } else {
        pos += snprintf(page + pos, sizeof(page) - pos,
            "<table><tr><th>IP Address</th><th>MAC Address</th></tr>");
        if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;
        for (int i = 0; i < client_count && pos < (int)sizeof(page) - 128; i++) {
            uint8_t *ip = (uint8_t *)&clients[i].ip;
            uint8_t *m = clients[i].mac;
            pos += snprintf(page + pos, sizeof(page) - pos,
                "<tr><td>%d.%d.%d.%d</td>"
                "<td>%02x:%02x:%02x:%02x:%02x:%02x</td></tr>",
                ip[0], ip[1], ip[2], ip[3],
                m[0], m[1], m[2], m[3], m[4], m[5]);
            if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;
        }
        pos += snprintf(page + pos, sizeof(page) - pos, "</table>");
        if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;
    }

    pos += snprintf(page + pos, sizeof(page) - pos,
        "</div>"
        "<a href='/config' style='display:block;text-align:center;background:#2196F3;color:#fff;"
        "padding:10px 24px;border-radius:4px;text-decoration:none;font-size:1em;margin-top:16px'>"
        "Settings</a>"
        "<a href='/diag' style='display:block;text-align:center;background:#757575;color:#fff;"
        "padding:10px 24px;border-radius:4px;text-decoration:none;font-size:1em;margin-top:8px'>"
        "Diagnostics</a>"
        "%s",
        HTML_FOOTER);
    if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, pos);
    return ESP_OK;
}

/* ---- GET /config ---- */

static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    uint8_t *sn = (uint8_t *)&s_cfg->dhcp_subnet;
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", sn[0], sn[1], sn[2], sn[3]);

    char page[5120];
    int pos = 0;
    int8_t txp = s_cfg->wifi_tx_power;
    uint8_t ch = s_cfg->wifi_channel;

#define SEL(val) ((txp == (val)) ? " selected" : "")
    pos += snprintf(page + pos, sizeof(page) - pos, "%s<div class='card'>", HTML_HEADER);
    if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;
    pos += snprintf(page + pos, sizeof(page) - pos,
        "<h1>ESP32-NetLink Settings</h1>"
        "<form method='POST' action='/config'>"
        "<label>WiFi SSID</label>"
        "<input type='text' name='wifi_ssid' value='%s' maxlength='32' required>"
        "<label>WiFi Password</label>"
        "<input type='password' name='wifi_pass' value='%s' maxlength='64'>",
        s_cfg->wifi_ssid, s_cfg->wifi_password);
    if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;
    pos += snprintf(page + pos, sizeof(page) - pos,
        "<label>WiFi TX Power</label>"
        "<select name='wifi_txp' style='width:100%%;padding:8px;border:1px solid #ddd;border-radius:4px;font-size:1em'>"
        "<option value='80'%s>20 dBm (max)</option>"
        "<option value='68'%s>17 dBm</option>"
        "<option value='60'%s>15 dBm</option>"
        "<option value='44'%s>11 dBm</option>"
        "<option value='34'%s>8.5 dBm</option>"
        "<option value='20'%s>5 dBm</option>"
        "<option value='8'%s>2 dBm (min)</option>"
        "</select>",
        SEL(80), SEL(68), SEL(60), SEL(44), SEL(34), SEL(20), SEL(8));
#undef SEL
    if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;

#define CHSEL(v) ((ch == (v)) ? " selected" : "")
    pos += snprintf(page + pos, sizeof(page) - pos,
        "<label>WiFi Channel</label>"
        "<select name='wifi_ch' style='width:100%%;padding:8px;border:1px solid #ddd;border-radius:4px;font-size:1em'>"
        "<option value='0'%s>Auto</option>", CHSEL(0));
    if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;
    for (int i = 1; i <= 13; i++) {
        pos += snprintf(page + pos, sizeof(page) - pos,
            "<option value='%d'%s>%d</option>", i, CHSEL(i), i);
        if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;
    }
    pos += snprintf(page + pos, sizeof(page) - pos, "</select>");
#undef CHSEL
    if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;

    pos += snprintf(page + pos, sizeof(page) - pos,
        "<label>DHCP Subnet (e.g. 192.168.4.0/24)</label>"
        "<input type='text' name='dhcp_subnet' value='%s/%d' required>"
        "<div class='toggle'>"
        "<input type='checkbox' name='dhcp_gw' id='dhcp_gw' value='1'%s>"
        "<label for='dhcp_gw' style='margin:0'>Advertise gateway in DHCP</label></div>"
        "<div class='toggle'>"
        "<input type='checkbox' name='dhcp_dns' id='dhcp_dns' value='1'%s>"
        "<label for='dhcp_dns' style='margin:0'>Advertise DNS in DHCP</label></div>",
        ip_str, s_cfg->dhcp_prefix_len,
        s_cfg->dhcp_gw_enabled ? " checked" : "",
        s_cfg->dhcp_dns_enabled ? " checked" : "");
    if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;
    pos += snprintf(page + pos, sizeof(page) - pos,
        "<label>Admin Password</label>"
        "<input type='password' name='admin_pass' value='%s' maxlength='64' required>"
        "<button type='submit'>Save &amp; Reboot</button>"
        "</form></div>"
        "<a href='/' style='display:block;text-align:center;background:#757575;color:#fff;"
        "padding:10px 24px;border-radius:4px;text-decoration:none;font-size:1em;margin-top:16px'>"
        "Back to Dashboard</a>%s",
        s_cfg->admin_password, HTML_FOOTER);
    if (pos >= (int)sizeof(page)) pos = sizeof(page) - 1;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, pos);
    return ESP_OK;
}

/* ---- POST /config ---- */

static void restart_cb(TimerHandle_t timer)
{
    esp_restart();
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    netlink_config_t new_cfg = *s_cfg;

    get_form_value(body, "wifi_ssid", new_cfg.wifi_ssid, sizeof(new_cfg.wifi_ssid));
    get_form_value(body, "wifi_pass", new_cfg.wifi_password, sizeof(new_cfg.wifi_password));
    get_form_value(body, "admin_pass", new_cfg.admin_password, sizeof(new_cfg.admin_password));

    char subnet_str[32];
    if (get_form_value(body, "dhcp_subnet", subnet_str, sizeof(subnet_str))) {
        char *slash = strchr(subnet_str, '/');
        if (slash) {
            *slash = '\0';
            in_addr_t addr = inet_addr(subnet_str);
            if (addr != INADDR_NONE) {
                new_cfg.dhcp_subnet = addr;
                int pfx = atoi(slash + 1);
                if (pfx >= 8 && pfx <= 30) {
                    new_cfg.dhcp_prefix_len = (uint8_t)pfx;
                }
            }
        }
    }

    char gw_val[4];
    new_cfg.dhcp_gw_enabled = get_form_value(body, "dhcp_gw", gw_val, sizeof(gw_val)) ? 1 : 0;

    char dns_val[4];
    new_cfg.dhcp_dns_enabled = get_form_value(body, "dhcp_dns", dns_val, sizeof(dns_val)) ? 1 : 0;

    char txp_val[8];
    if (get_form_value(body, "wifi_txp", txp_val, sizeof(txp_val))) {
        new_cfg.wifi_tx_power = (int8_t)atoi(txp_val);
    }

    char ch_val[4];
    if (get_form_value(body, "wifi_ch", ch_val, sizeof(ch_val))) {
        new_cfg.wifi_channel = (uint8_t)atoi(ch_val);
    }

    if (strlen(new_cfg.wifi_ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID cannot be empty");
        return ESP_FAIL;
    }
    if (strlen(new_cfg.admin_password) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Admin password cannot be empty");
        return ESP_FAIL;
    }

    esp_err_t err = config_save(&new_cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }
    *s_cfg = new_cfg;

    char page[2048];
    int len = snprintf(page, sizeof(page),
        "%s<div class='card'>"
        "<div class='msg'>Settings saved. Rebooting in 3 seconds...</div>"
        "<h1>ESP32-NetLink</h1>"
        "<p>Please reconnect after reboot.</p>"
        "</div>%s",
        HTML_HEADER, HTML_FOOTER);
    if (len >= (int)sizeof(page)) len = sizeof(page) - 1;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, len);

    TimerHandle_t timer = xTimerCreate("restart", pdMS_TO_TICKS(3000), pdFALSE, NULL, restart_cb);
    xTimerStart(timer, 0);
    return ESP_OK;
}

/* ---- GET /diag (diagnostics) ---- */

typedef struct {
    char name[6];
    uint32_t ip;
    uint32_t netmask;
    uint8_t mac[6];
    bool up;
    bool link_up;
} netif_diag_t;

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    char netif_name[6];
} arp_diag_t;

typedef struct {
    netif_diag_t netifs[8];
    int netif_count;
    arp_diag_t arps[16];
    int arp_count;
    SemaphoreHandle_t done;
} diag_lwip_ctx_t;

static void diag_collect_fn(void *arg)
{
    diag_lwip_ctx_t *ctx = (diag_lwip_ctx_t *)arg;

    ctx->netif_count = 0;
    struct netif *n;
    NETIF_FOREACH(n) {
        if (ctx->netif_count >= 8) break;
        netif_diag_t *d = &ctx->netifs[ctx->netif_count];
        snprintf(d->name, sizeof(d->name), "%c%c%d",
                 n->name[0], n->name[1], n->num);
        d->ip = netif_ip4_addr(n)->addr;
        d->netmask = netif_ip4_netmask(n)->addr;
        memcpy(d->mac, n->hwaddr, 6);
        d->up = !!(n->flags & NETIF_FLAG_UP);
        d->link_up = !!(n->flags & NETIF_FLAG_LINK_UP);
        ctx->netif_count++;
    }

    ctx->arp_count = 0;
    for (size_t i = 0; i < ARP_TABLE_SIZE && ctx->arp_count < 16; i++) {
        ip4_addr_t *ip = NULL;
        struct netif *netif = NULL;
        struct eth_addr *mac = NULL;
        if (etharp_get_entry(i, &ip, &netif, &mac)) {
            arp_diag_t *a = &ctx->arps[ctx->arp_count];
            a->ip = ip->addr;
            memcpy(a->mac, mac->addr, 6);
            snprintf(a->netif_name, sizeof(a->netif_name), "%c%c%d",
                     netif->name[0], netif->name[1], netif->num);
            ctx->arp_count++;
        }
    }

    xSemaphoreGive(ctx->done);
}

static void html_escape_chunk(httpd_req_t *req, const char *src)
{
    char buf[512];
    size_t di = 0;
    for (size_t i = 0; src[i]; i++) {
        const char *esc = NULL;
        size_t elen = 0;
        switch (src[i]) {
        case '<': esc = "&lt;";  elen = 4; break;
        case '>': esc = "&gt;";  elen = 4; break;
        case '&': esc = "&amp;"; elen = 5; break;
        }
        if (esc) {
            if (di + elen >= sizeof(buf) - 1) {
                buf[di] = '\0';
                httpd_resp_send_chunk(req, buf, di);
                di = 0;
            }
            memcpy(buf + di, esc, elen);
            di += elen;
        } else {
            buf[di++] = src[i];
        }
        if (di >= sizeof(buf) - 8) {
            buf[di] = '\0';
            httpd_resp_send_chunk(req, buf, di);
            di = 0;
        }
    }
    if (di > 0) {
        buf[di] = '\0';
        httpd_resp_send_chunk(req, buf, di);
    }
}

static esp_err_t diag_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    httpd_resp_set_type(req, "text/html");

    diag_lwip_ctx_t lwip_ctx = { .done = xSemaphoreCreateBinary() };
    if (lwip_ctx.done) {
        if (tcpip_callback(diag_collect_fn, &lwip_ctx) == ERR_OK)
            xSemaphoreTake(lwip_ctx.done, portMAX_DELAY);
        vSemaphoreDelete(lwip_ctx.done);
    }

    wifi_sta_list_t sta_list = {0};
    esp_wifi_ap_get_sta_list(&sta_list);

    char buf[512];
    int len;

    httpd_resp_send_chunk(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='5'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Diagnostics</title><style>"
        "body{font-family:monospace;max-width:600px;margin:20px auto;padding:0 10px;"
        "background:#1a1a2e;color:#e0e0e0}"
        ".c{background:#16213e;border-radius:8px;padding:12px;margin-bottom:10px}"
        "h2{color:#e94560;font-size:1em;margin:0 0 6px}"
        "table{width:100%;border-collapse:collapse;font-size:.85em}"
        "th,td{padding:3px 6px;text-align:left;border-bottom:1px solid #0f3460}"
        "th{color:#e94560}"
        ".g{color:#4ecca3}.r{color:#e94560}"
        "pre{background:#0f3460;padding:8px;border-radius:4px;font-size:.78em;"
        "max-height:400px;overflow:auto;white-space:pre-wrap;word-break:break-all}"
        "a{color:#4ecca3}"
        "</style></head><body><h1 style='color:#e94560;font-size:1.2em'>Diagnostics</h1>",
        HTTPD_RESP_USE_STRLEN);

    /* Network interfaces */
    httpd_resp_send_chunk(req,
        "<div class='c'><h2>Network Interfaces</h2>"
        "<table><tr><th>Name</th><th>IP</th><th>MAC</th><th>State</th></tr>",
        HTTPD_RESP_USE_STRLEN);
    for (int i = 0; i < lwip_ctx.netif_count; i++) {
        netif_diag_t *d = &lwip_ctx.netifs[i];
        uint8_t *ip = (uint8_t *)&d->ip;
        uint8_t *m = d->mac;
        len = snprintf(buf, sizeof(buf),
            "<tr><td>%s</td><td>%d.%d.%d.%d</td>"
            "<td>%02x:%02x:%02x:%02x:%02x:%02x</td>"
            "<td class='%s'>%s%s</td></tr>",
            d->name, ip[0], ip[1], ip[2], ip[3],
            m[0], m[1], m[2], m[3], m[4], m[5],
            (d->up && d->link_up) ? "g" : "r",
            d->up ? "UP" : "DOWN",
            d->link_up ? " LINK" : "");
        httpd_resp_send_chunk(req, buf, len);
    }
    httpd_resp_send_chunk(req, "</table></div>", HTTPD_RESP_USE_STRLEN);

    /* USB NCM */
    len = snprintf(buf, sizeof(buf),
        "<div class='c'><h2>USB NCM</h2>"
        "<table><tr><th>Status</th><th>RX pkts</th><th>TX pkts</th></tr>"
        "<tr><td class='%s'>%s</td><td>%lu</td><td>%lu</td></tr></table></div>",
        usb_ncm_is_connected() ? "g" : "r",
        usb_ncm_is_connected() ? "Connected" : "Disconnected",
        (unsigned long)usb_ncm_get_rx_count(),
        (unsigned long)usb_ncm_get_tx_count());
    httpd_resp_send_chunk(req, buf, len);

    /* WiFi stations */
    len = snprintf(buf, sizeof(buf),
        "<div class='c'><h2>WiFi Stations (%d)</h2>", sta_list.num);
    httpd_resp_send_chunk(req, buf, len);
    if (sta_list.num == 0) {
        httpd_resp_send_chunk(req, "<p style='color:#666'>None</p>",
                              HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_chunk(req,
            "<table><tr><th>MAC</th><th>RSSI</th></tr>",
            HTTPD_RESP_USE_STRLEN);
        for (int i = 0; i < sta_list.num; i++) {
            uint8_t *m = sta_list.sta[i].mac;
            len = snprintf(buf, sizeof(buf),
                "<tr><td>%02x:%02x:%02x:%02x:%02x:%02x</td><td>%d</td></tr>",
                m[0], m[1], m[2], m[3], m[4], m[5],
                sta_list.sta[i].rssi);
            httpd_resp_send_chunk(req, buf, len);
        }
        httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

    /* DHCP clients */
    client_entry_t clients[10];
    int ccnt = get_clients(clients, 10);
    len = snprintf(buf, sizeof(buf),
        "<div class='c'><h2>DHCP Clients (%d)</h2>", ccnt);
    httpd_resp_send_chunk(req, buf, len);
    if (ccnt == 0) {
        httpd_resp_send_chunk(req, "<p style='color:#666'>None</p>",
                              HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_chunk(req,
            "<table><tr><th>IP</th><th>MAC</th></tr>",
            HTTPD_RESP_USE_STRLEN);
        for (int i = 0; i < ccnt; i++) {
            uint8_t *ip = (uint8_t *)&clients[i].ip;
            uint8_t *m = clients[i].mac;
            len = snprintf(buf, sizeof(buf),
                "<tr><td>%d.%d.%d.%d</td>"
                "<td>%02x:%02x:%02x:%02x:%02x:%02x</td></tr>",
                ip[0], ip[1], ip[2], ip[3],
                m[0], m[1], m[2], m[3], m[4], m[5]);
            httpd_resp_send_chunk(req, buf, len);
        }
        httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

    /* ARP table */
    len = snprintf(buf, sizeof(buf),
        "<div class='c'><h2>ARP Table (%d)</h2>", lwip_ctx.arp_count);
    httpd_resp_send_chunk(req, buf, len);
    if (lwip_ctx.arp_count == 0) {
        httpd_resp_send_chunk(req, "<p style='color:#666'>Empty</p>",
                              HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_chunk(req,
            "<table><tr><th>IP</th><th>MAC</th><th>Netif</th></tr>",
            HTTPD_RESP_USE_STRLEN);
        for (int i = 0; i < lwip_ctx.arp_count; i++) {
            uint8_t *ip = (uint8_t *)&lwip_ctx.arps[i].ip;
            uint8_t *m = lwip_ctx.arps[i].mac;
            len = snprintf(buf, sizeof(buf),
                "<tr><td>%d.%d.%d.%d</td>"
                "<td>%02x:%02x:%02x:%02x:%02x:%02x</td>"
                "<td>%s</td></tr>",
                ip[0], ip[1], ip[2], ip[3],
                m[0], m[1], m[2], m[3], m[4], m[5],
                lwip_ctx.arps[i].netif_name);
            httpd_resp_send_chunk(req, buf, len);
        }
        httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

    /* Recent logs */
    httpd_resp_send_chunk(req,
        "<div class='c'><h2>Recent Logs</h2><pre>",
        HTTPD_RESP_USE_STRLEN);
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int start = (s_log_count < LOG_LINES) ? 0 : s_log_head;
        int total = (s_log_count < LOG_LINES) ? s_log_count : LOG_LINES;
        for (int i = 0; i < total; i++) {
            int idx = (start + i) % LOG_LINES;
            html_escape_chunk(req, s_log_buf[idx]);
            httpd_resp_send_chunk(req, "\n", 1);
        }
        xSemaphoreGive(s_log_mutex);
    }
    httpd_resp_send_chunk(req, "</pre></div>", HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req,
        "<a href='/' style='display:block;text-align:center;background:#0f3460;"
        "color:#e0e0e0;padding:10px;border-radius:4px;text-decoration:none;"
        "margin-top:10px'>Back to Dashboard</a></body></html>",
        HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---- Start ---- */

esp_err_t web_server_start(netlink_config_t *cfg)
{
    s_cfg = cfg;

    s_dhcp_mutex = xSemaphoreCreateMutex();
    memset(s_dhcp_clients, 0, sizeof(s_dhcp_clients));
    esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                               dhcp_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                               wifi_disconnect_handler, NULL);

    s_log_mutex = xSemaphoreCreateMutex();
    esp_log_set_vprintf(log_vprintf);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    httpd_handle_t server = NULL;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t dashboard = {
        .uri = "/", .method = HTTP_GET, .handler = dashboard_handler,
    };
    const httpd_uri_t config_get = {
        .uri = "/config", .method = HTTP_GET, .handler = config_get_handler,
    };
    const httpd_uri_t config_post = {
        .uri = "/config", .method = HTTP_POST, .handler = config_post_handler,
    };
    const httpd_uri_t diag = {
        .uri = "/diag", .method = HTTP_GET, .handler = diag_handler,
    };

    httpd_register_uri_handler(server, &dashboard);
    httpd_register_uri_handler(server, &config_get);
    httpd_register_uri_handler(server, &config_post);
    httpd_register_uri_handler(server, &diag);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}
