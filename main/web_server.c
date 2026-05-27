#include "web_server.h"
#include "router.h"
#include "usb_ncm.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include <stdio.h>
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

static esp_err_t send_chunkf(httpd_req_t *req, const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len < 0) {
        return ESP_FAIL;
    }
    if (len >= (int)sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    return httpd_resp_send_chunk(req, buf, len);
}

/* ---- Formatting and config helpers ---- */

static void format_ipv4(uint32_t addr, char *out, size_t out_len)
{
    uint8_t *ip = (uint8_t *)&addr;
    snprintf(out, out_len, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

static void format_duration(uint32_t seconds, char *out, size_t out_len)
{
    uint32_t minutes = seconds / 60;
    uint32_t hours = minutes / 60;

    if (hours > 0) {
        snprintf(out, out_len, "%uh %02um", (unsigned)hours,
                 (unsigned)(minutes % 60));
    } else if (minutes > 0) {
        snprintf(out, out_len, "%um", (unsigned)minutes);
    } else {
        snprintf(out, out_len, "%us", (unsigned)seconds);
    }
}

static bool parse_subnet_text(const char *subnet_str, uint32_t *subnet,
                              uint8_t *prefix_len)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%s", subnet_str);

    char *slash = strchr(buf, '/');
    if (!slash) {
        return false;
    }

    *slash = '\0';
    in_addr_t addr = inet_addr(buf);
    if (addr == INADDR_NONE) {
        return false;
    }

    int pfx = atoi(slash + 1);
    if (pfx < 8 || pfx > 29) {
        return false;
    }

    *subnet = addr & router_prefix_to_netmask((uint8_t)pfx);
    *prefix_len = (uint8_t)pfx;
    return true;
}

static bool parse_subnet_field(const char *body, const char *key,
                               uint32_t *subnet, uint8_t *prefix_len)
{
    char subnet_str[32];
    if (!get_form_value(body, key, subnet_str, sizeof(subnet_str))) {
        return false;
    }
    return parse_subnet_text(subnet_str, subnet, prefix_len);
}

static bool ip_in_subnet(uint32_t ip, uint32_t subnet, uint8_t prefix_len)
{
    uint32_t mask = router_prefix_to_netmask(prefix_len);
    return (ip & mask) == (subnet & mask);
}

static bool parse_port_u16(const char *text, uint16_t *out)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (!text[0] || (end && *end) || value < 1 || value > 65535) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

/* ---- GET / (dashboard) ---- */

static esp_err_t dashboard_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    router_dhcp_lease_t leases[ROUTER_MAX_DHCP_LEASES];
    size_t lease_count = router_get_dhcp_leases(leases, ROUTER_MAX_DHCP_LEASES);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, HTML_HEADER, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
        "<div class='card'><h1>ESP32-NetLink</h1>"
        "<h2>DHCP Leases</h2>",
        HTTPD_RESP_USE_STRLEN);

    if (lease_count == 0) {
        httpd_resp_send_chunk(req,
            "<p class='empty'>No devices connected</p>",
            HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_chunk(req,
            "<table><tr><th>Interface</th><th>IP Address</th>"
            "<th>MAC Address</th><th>Expires</th></tr>",
            HTTPD_RESP_USE_STRLEN);
        for (size_t i = 0; i < lease_count; i++) {
            uint8_t *ip = (uint8_t *)&leases[i].ip;
            uint8_t *m = leases[i].mac;
            char expires[16];
            format_duration(leases[i].expires_in_seconds, expires, sizeof(expires));
            send_chunkf(req,
                "<tr><td>%s</td><td>%d.%d.%d.%d</td>"
                "<td>%02x:%02x:%02x:%02x:%02x:%02x</td><td>%s</td></tr>",
                leases[i].iface,
                ip[0], ip[1], ip[2], ip[3],
                m[0], m[1], m[2], m[3], m[4], m[5],
                expires);
        }
        httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
    }

    send_chunkf(req,
        "</div>"
        "<a href='/config' style='display:block;text-align:center;background:#2196F3;color:#fff;"
        "padding:10px 24px;border-radius:4px;text-decoration:none;font-size:1em;margin-top:16px'>"
        "Settings</a>"
        "<a href='/diag' style='display:block;text-align:center;background:#757575;color:#fff;"
        "padding:10px 24px;border-radius:4px;text-decoration:none;font-size:1em;margin-top:8px'>"
        "Diagnostics</a>"
        "%s",
        HTML_FOOTER);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---- GET /config ---- */

static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    char usb_ip_str[16];
    char wifi_ip_str[16];
    format_ipv4(s_cfg->usb_subnet, usb_ip_str, sizeof(usb_ip_str));
    format_ipv4(s_cfg->wifi_subnet, wifi_ip_str, sizeof(wifi_ip_str));

    int8_t txp = s_cfg->wifi_tx_power;
    uint8_t ch = s_cfg->wifi_channel;

    httpd_resp_set_type(req, "text/html");

#define SEL(val) ((txp == (val)) ? " selected" : "")
    httpd_resp_send_chunk(req, HTML_HEADER, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
    send_chunkf(req,
        "<h1>ESP32-NetLink Settings</h1>"
        "<form method='POST' action='/config'>"
        "<label>WiFi SSID</label>"
        "<input type='text' name='wifi_ssid' value='%s' maxlength='32' required>"
        "<label>WiFi Password</label>"
        "<input type='password' name='wifi_pass' value='%s' maxlength='64'>",
        s_cfg->wifi_ssid, s_cfg->wifi_password);
    send_chunkf(req,
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

#define CHSEL(v) ((ch == (v)) ? " selected" : "")
    send_chunkf(req,
        "<label>WiFi Channel</label>"
        "<select name='wifi_ch' style='width:100%%;padding:8px;border:1px solid #ddd;border-radius:4px;font-size:1em'>"
        "<option value='0'%s>Auto</option>", CHSEL(0));
    for (int i = 1; i <= 13; i++) {
        send_chunkf(req,
            "<option value='%d'%s>%d</option>", i, CHSEL(i), i);
    }
    httpd_resp_send_chunk(req, "</select>", HTTPD_RESP_USE_STRLEN);
#undef CHSEL

    send_chunkf(req,
        "<label>USB Subnet (e.g. 192.168.5.0/24)</label>"
        "<input type='text' name='usb_subnet' value='%s/%d' required>"
        "<label>WiFi Subnet (e.g. 192.168.4.0/24)</label>"
        "<input type='text' name='wifi_subnet' value='%s/%d' required>"
        "<div class='toggle'>"
        "<input type='checkbox' name='dhcp_gw' id='dhcp_gw' value='1'%s>"
        "<label for='dhcp_gw' style='margin:0'>Advertise gateway in DHCP</label></div>"
        "<div class='toggle'>"
        "<input type='checkbox' name='dhcp_dns' id='dhcp_dns' value='1'%s>"
        "<label for='dhcp_dns' style='margin:0'>Advertise DNS in DHCP</label></div>",
        usb_ip_str, s_cfg->usb_prefix_len,
        wifi_ip_str, s_cfg->wifi_prefix_len,
        s_cfg->dhcp_gw_enabled ? " checked" : "",
        s_cfg->dhcp_dns_enabled ? " checked" : "");

    httpd_resp_send_chunk(req,
        "<h2 style='margin-top:20px'>WiFi to USB Port Forwarding</h2>"
        "<table><tr><th>On</th><th>Listen</th><th>Target IP</th><th>Target</th></tr>",
        HTTPD_RESP_USE_STRLEN);
    for (int i = 0; i < NETLINK_MAX_PORT_FORWARDS; i++) {
        char target_ip[16] = "";
        char listen_port[8] = "";
        char target_port[8] = "";
        if (s_cfg->port_forwards[i].target_ip) {
            format_ipv4(s_cfg->port_forwards[i].target_ip, target_ip, sizeof(target_ip));
        }
        if (s_cfg->port_forwards[i].listen_port) {
            snprintf(listen_port, sizeof(listen_port), "%u",
                     s_cfg->port_forwards[i].listen_port);
        }
        if (s_cfg->port_forwards[i].target_port) {
            snprintf(target_port, sizeof(target_port), "%u",
                     s_cfg->port_forwards[i].target_port);
        }
        send_chunkf(req,
            "<tr>"
            "<td><input type='checkbox' name='pf%d_en' value='1'%s></td>"
            "<td><input type='text' name='pf%d_lport' value='%s' placeholder='2222'></td>"
            "<td><input type='text' name='pf%d_tip' value='%s' placeholder='192.168.5.2'></td>"
            "<td><input type='text' name='pf%d_tport' value='%s' placeholder='22'></td>"
            "</tr>",
            i, s_cfg->port_forwards[i].enabled ? " checked" : "",
            i, listen_port,
            i, target_ip,
            i, target_port);
    }
    httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);

    send_chunkf(req,
        "<label>Admin Password</label>"
        "<input type='password' name='admin_pass' value='%s' maxlength='64' required>"
        "<button type='submit'>Save &amp; Reboot</button>"
        "</form></div>"
        "<a href='/' style='display:block;text-align:center;background:#757575;color:#fff;"
        "padding:10px 24px;border-radius:4px;text-decoration:none;font-size:1em;margin-top:16px'>"
        "Back to Dashboard</a>%s",
        s_cfg->admin_password, HTML_FOOTER);
    httpd_resp_send_chunk(req, NULL, 0);
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

    char body[2048];
    if (req->content_len >= sizeof(body)) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, "Config form is too large", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    int received = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, body + received,
                                   req->content_len - received);
        if (chunk == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (chunk <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
            return ESP_FAIL;
        }
        received += chunk;
    }
    if (received == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    netlink_config_t new_cfg = *s_cfg;

    get_form_value(body, "wifi_ssid", new_cfg.wifi_ssid, sizeof(new_cfg.wifi_ssid));
    get_form_value(body, "wifi_pass", new_cfg.wifi_password, sizeof(new_cfg.wifi_password));
    get_form_value(body, "admin_pass", new_cfg.admin_password, sizeof(new_cfg.admin_password));

    if (!parse_subnet_field(body, "usb_subnet",
                            &new_cfg.usb_subnet, &new_cfg.usb_prefix_len)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid USB subnet");
        return ESP_FAIL;
    }

    if (!parse_subnet_field(body, "wifi_subnet",
                            &new_cfg.wifi_subnet, &new_cfg.wifi_prefix_len)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid WiFi subnet");
        return ESP_FAIL;
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
    if (router_subnets_overlap(new_cfg.usb_subnet, new_cfg.usb_prefix_len,
                               new_cfg.wifi_subnet, new_cfg.wifi_prefix_len)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "USB and WiFi subnets must not overlap");
        return ESP_FAIL;
    }

    for (int i = 0; i < NETLINK_MAX_PORT_FORWARDS; i++) {
        port_forward_rule_t *rule = &new_cfg.port_forwards[i];
        char key[16];
        char value[32];

        memset(rule, 0, sizeof(*rule));
        snprintf(key, sizeof(key), "pf%d_en", i);
        rule->enabled = get_form_value(body, key, value, sizeof(value)) ? 1 : 0;

        snprintf(key, sizeof(key), "pf%d_lport", i);
        bool has_lport = get_form_value(body, key, value, sizeof(value)) && value[0];
        if (has_lport && !parse_port_u16(value, &rule->listen_port) && rule->enabled) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid listen port");
            return ESP_FAIL;
        }

        snprintf(key, sizeof(key), "pf%d_tip", i);
        bool has_tip = get_form_value(body, key, value, sizeof(value)) && value[0];
        if (has_tip) {
            in_addr_t target_ip = inet_addr(value);
            if (target_ip == INADDR_NONE) {
                if (rule->enabled) {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid target IP");
                    return ESP_FAIL;
                }
            } else {
                rule->target_ip = target_ip;
            }
        }

        snprintf(key, sizeof(key), "pf%d_tport", i);
        bool has_tport = get_form_value(body, key, value, sizeof(value)) && value[0];
        if (has_tport && !parse_port_u16(value, &rule->target_port) && rule->enabled) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid target port");
            return ESP_FAIL;
        }

        if (!rule->enabled) {
            continue;
        }
        if (!has_lport || !has_tip || !has_tport ||
            rule->listen_port == 0 || rule->target_port == 0 || rule->target_ip == 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Enabled port forward rule is incomplete");
            return ESP_FAIL;
        }
        if (rule->listen_port == 80) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Listen port 80 is reserved for the web UI");
            return ESP_FAIL;
        }
        if (!ip_in_subnet(rule->target_ip, new_cfg.usb_subnet, new_cfg.usb_prefix_len)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Target IP must be in the USB subnet");
            return ESP_FAIL;
        }

        uint32_t mask = router_prefix_to_netmask(new_cfg.usb_prefix_len);
        uint32_t network = new_cfg.usb_subnet & mask;
        uint32_t gateway = network | htonl(1);
        uint32_t broadcast = network | ~mask;
        if (rule->target_ip == network || rule->target_ip == gateway ||
            rule->target_ip == broadcast) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Target IP cannot be network, gateway, or broadcast");
            return ESP_FAIL;
        }

        for (int j = 0; j < i; j++) {
            if (new_cfg.port_forwards[j].enabled &&
                new_cfg.port_forwards[j].listen_port == rule->listen_port) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Duplicate listen ports are not allowed");
                return ESP_FAIL;
            }
        }
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

    /* DHCP leases */
    router_dhcp_lease_t leases[ROUTER_MAX_DHCP_LEASES];
    size_t ccnt = router_get_dhcp_leases(leases, ROUTER_MAX_DHCP_LEASES);
    len = snprintf(buf, sizeof(buf),
        "<div class='c'><h2>DHCP Leases (%u)</h2>", (unsigned)ccnt);
    httpd_resp_send_chunk(req, buf, len);
    if (ccnt == 0) {
        httpd_resp_send_chunk(req, "<p style='color:#666'>None</p>",
                              HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_chunk(req,
            "<table><tr><th>If</th><th>IP</th><th>MAC</th><th>Expires</th></tr>",
            HTTPD_RESP_USE_STRLEN);
        for (size_t i = 0; i < ccnt; i++) {
            uint8_t *ip = (uint8_t *)&leases[i].ip;
            uint8_t *m = leases[i].mac;
            char expires[16];
            format_duration(leases[i].expires_in_seconds, expires, sizeof(expires));
            len = snprintf(buf, sizeof(buf),
                "<tr><td>%s</td><td>%d.%d.%d.%d</td>"
                "<td>%02x:%02x:%02x:%02x:%02x:%02x</td><td>%s</td></tr>",
                leases[i].iface,
                ip[0], ip[1], ip[2], ip[3],
                m[0], m[1], m[2], m[3], m[4], m[5],
                expires);
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
