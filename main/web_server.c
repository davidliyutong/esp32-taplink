#include "web_server.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/wdt_hal.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "router.h"
#include "soc/rtc.h"
#include "usb_ncm.h"
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web";
static taplink_config_t *s_cfg;

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
        while (len > 0 && (s_log_buf[s_log_head][len - 1] == '\n' || s_log_buf[s_log_head][len - 1] == '\r'))
            s_log_buf[s_log_head][--len] = '\0';
        s_log_head = (s_log_head + 1) % LOG_LINES;
        if (s_log_count < LOG_LINES)
            s_log_count++;
        xSemaphoreGive(s_log_mutex);
    }

    va_end(copy);
    return vprintf(fmt, args);
}

/* ---- Base64 decode (for HTTP Basic Auth) ---- */

static const uint8_t B64[256] = {
    ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,  ['F'] = 5,  ['G'] = 6,  ['H'] = 7,
    ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15,
    ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19, ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23,
    ['Y'] = 24, ['Z'] = 25, ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31,
    ['g'] = 32, ['h'] = 33, ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39,
    ['o'] = 40, ['p'] = 41, ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47,
    ['w'] = 48, ['x'] = 49, ['y'] = 50, ['z'] = 51, ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55,
    ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59, ['8'] = 60, ['9'] = 61, ['+'] = 62, ['/'] = 63,
};

static int b64_decode(const char *in, size_t in_len, char *out, size_t out_max)
{
    size_t out_len = 0;
    for (size_t i = 0; i + 3 < in_len && out_len + 3 <= out_max; i += 4) {
        uint32_t v = (B64[(uint8_t)in[i]] << 18) | (B64[(uint8_t)in[i + 1]] << 12) | (B64[(uint8_t)in[i + 2]] << 6) |
                     B64[(uint8_t)in[i + 3]];
        out[out_len++] = (v >> 16) & 0xFF;
        if (in[i + 2] != '=')
            out[out_len++] = (v >> 8) & 0xFF;
        if (in[i + 3] != '=')
            out[out_len++] = v & 0xFF;
    }
    if (out_len < out_max)
        out[out_len] = '\0';
    return out_len;
}

/* ---- Auth check ---- */

static bool check_auth(httpd_req_t *req)
{
    char auth[256];
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0 || hdr_len >= sizeof(auth))
        return false;

    httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth));

    if (strncmp(auth, "Basic ", 6) != 0)
        return false;

    char decoded[128];
    b64_decode(auth + 6, strlen(auth + 6), decoded, sizeof(decoded));

    char expected[140];
    snprintf(expected, sizeof(expected), "admin:%s", s_cfg->admin_password);
    return strcmp(decoded, expected) == 0;
}

static esp_err_t send_auth_required(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-TapLink\"");
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
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
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
    if (!p)
        return false;
    if (p != body && *(p - 1) != '&')
        return false;
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_max)
        len = out_max - 1;
    char encoded[256];
    memcpy(encoded, p, len);
    encoded[len] = '\0';
    url_decode(out, encoded, out_max);
    return true;
}

/* ---- HTML ---- */

static const char HTML_HEADER[] = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                                  "<title>ESP32-TapLink</title>"
                                  "<link rel='stylesheet' href='/style.css'></head><body><main>";

static const char HTML_FOOTER[] = "</main></body></html>";

static const char STYLE_CSS[] = "body{font-family:system-ui,-apple-system,BlinkMacSystemFont,sans-serif;"
                                "max-width:760px;margin:24px auto;padding:0 "
                                "14px;background:#f6f7f9;color:#20242a}"
                                "main{display:block}"
                                ".nav{display:flex;gap:6px;flex-wrap:wrap;margin:0 0 14px}"
                                ".nav a{background:#e8edf3;color:#2d3748;text-decoration:none;padding:8px "
                                "10px;border-radius:6px;font-size:.92em}"
                                ".nav a.on{background:#1f6feb;color:#fff}"
                                ".card{background:#fff;border:1px solid "
                                "#e2e8f0;border-radius:8px;padding:18px;box-shadow:0 1px 2px "
                                "rgba(15,23,42,.06);margin-bottom:14px}"
                                "h1{font-size:1.35em;margin:0 0 16px;color:#111827}"
                                "h2{font-size:1.05em;margin:18px 0 10px;color:#374151}"
                                "form{margin:0}"
                                "label{display:block;margin:12px 0 4px;color:#4b5563;font-size:.9em}"
                                "input[type=text],input[type=password],select{width:100%;padding:8px;"
                                "border:1px solid "
                                "#cbd5e1;border-radius:6px;box-sizing:border-box;font-size:1em;background:#"
                                "fff}"
                                "button,.btn{display:block;width:100%;box-sizing:border-box;text-align:"
                                "center;background:#1f6feb;color:#fff;border:0;padding:10px "
                                "14px;border-radius:6px;text-decoration:none;font-size:1em;cursor:pointer}"
                                "form button[type=submit]{margin-top:16px}"
                                ".btn.secondary,button.secondary{background:#64748b}.btn.warn,button.warn{"
                                "background:#b45309}"
                                ".actions{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,"
                                "1fr));gap:8px;margin-top:14px}"
                                ".card>.actions{margin-top:10px}"
                                ".inline{margin:0}.msg{background:#ecfdf5;color:#166534;padding:10px;"
                                "border-radius:6px;margin:0 0 14px;border:1px solid #bbf7d0}"
                                ".empty{color:#6b7280;font-style:italic;padding:12px "
                                "0}.toggle{display:flex;align-items:center;gap:8px;margin:12px 0}.toggle "
                                "input{width:auto;margin:0}.toggle label{margin:0}"
                                ".table-wrap{overflow-x:auto}table{width:100%;border-collapse:collapse;"
                                "font-size:.9em}th,td{padding:8px;text-align:left;border-bottom:1px solid "
                                "#e5e7eb;vertical-align:top}th{color:#64748b;font-weight:600}"
                                ".ok{color:#047857}.bad{color:#b91c1c}.muted{color:#6b7280}.pf-table "
                                "input{min-width:88px}"
                                "pre{background:#0f172a;color:#e5e7eb;padding:10px;border-radius:6px;font-"
                                "size:.82em;max-height:420px;overflow:auto;white-space:pre-wrap;word-break:"
                                "break-all}"
                                "@media(max-width:520px){body{margin:14px "
                                "auto}.card{padding:14px}th,td{padding:6px}.nav "
                                "a{flex:1;text-align:center}}";

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

static void html_escape_chunk(httpd_req_t *req, const char *src)
{
    char buf[512];
    size_t di = 0;
    for (size_t i = 0; src[i]; i++) {
        const char *esc = NULL;
        size_t elen = 0;
        switch (src[i]) {
        case '<':
            esc = "&lt;";
            elen = 4;
            break;
        case '>':
            esc = "&gt;";
            elen = 4;
            break;
        case '&':
            esc = "&amp;";
            elen = 5;
            break;
        case '"':
            esc = "&quot;";
            elen = 6;
            break;
        case '\'':
            esc = "&#39;";
            elen = 5;
            break;
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

static esp_err_t style_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    httpd_resp_send(req, STYLE_CSS, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t send_page_start(httpd_req_t *req, const char *active, bool refresh)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (refresh) {
        httpd_resp_send_chunk(req,
                              "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                              "<meta http-equiv='refresh' content='15'>"
                              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                              "<title>ESP32-TapLink</title>"
                              "<link rel='stylesheet' href='/style.css'></head><body><main>",
                              HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_chunk(req, HTML_HEADER, HTTPD_RESP_USE_STRLEN);
    }

    return send_chunkf(req,
                       "<nav class='nav'>"
                       "<a class='%s' href='/'>Dashboard</a>"
                       "<a class='%s' href='/config'>Settings</a>"
                       "<a class='%s' href='/port-forward'>Port Forwarding</a>"
                       "<a class='%s' href='/diag'>Diagnostics</a>"
                       "<a class='%s' href='/update'>Update</a>"
                       "</nav>",
                       strcmp(active, "dashboard") == 0 ? "on" : "", strcmp(active, "settings") == 0 ? "on" : "",
                       strcmp(active, "ports") == 0 ? "on" : "", strcmp(active, "diag") == 0 ? "on" : "",
                       strcmp(active, "update") == 0 ? "on" : "");
}

static void send_page_end(httpd_req_t *req)
{
    httpd_resp_send_chunk(req, HTML_FOOTER, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
}

static bool get_query_value(httpd_req_t *req, const char *key, char *out, size_t out_max)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len >= 128) {
        return false;
    }

    char query[128];
    char encoded[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, encoded, sizeof(encoded)) != ESP_OK) {
        return false;
    }

    url_decode(out, encoded, out_max);
    return true;
}

static bool query_is_saved(httpd_req_t *req)
{
    char saved[8];
    return get_query_value(req, "saved", saved, sizeof(saved)) && strcmp(saved, "1") == 0;
}

static void send_saved_notice(httpd_req_t *req)
{
    if (query_is_saved(req)) {
        httpd_resp_send_chunk(req, "<div class='msg'>Saved. Reboot required.</div>", HTTPD_RESP_USE_STRLEN);
    }
}

static const char *normalize_return_path(const char *path)
{
    if (strcmp(path, "/config") == 0 || strcmp(path, "/port-forward") == 0 || strcmp(path, "/diag") == 0 ||
        strcmp(path, "/update") == 0 || strcmp(path, "/") == 0) {
        return path;
    }
    return "/";
}

static esp_err_t send_redirect(httpd_req_t *req, const char *location)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static esp_err_t read_form_body(httpd_req_t *req, char *body, size_t body_size)
{
    if (req->content_len >= body_size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, "Form is too large", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    size_t received = 0;
    int timeouts = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, body + received, req->content_len - received);
        if (chunk == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts >= 5) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Timed out reading form body");
                return ESP_FAIL;
            }
            continue;
        }
        if (chunk <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
            return ESP_FAIL;
        }
        timeouts = 0;
        received += (size_t)chunk;
    }
    body[received] = '\0';
    return ESP_OK;
}

static void send_reboot_form(httpd_req_t *req, const char *return_path)
{
    send_chunkf(req,
                "<form class='inline' method='POST' action='/reboot'>"
                "<input type='hidden' name='return' value='%s'>"
                "<button class='warn' type='submit'>Reboot</button></form>",
                normalize_return_path(return_path));
}

static void send_value_input(httpd_req_t *req, const char *label, const char *type, const char *id, const char *name,
                             const char *value, const char *attrs)
{
    send_chunkf(req, "<label for='%s'>%s</label><input type='%s' id='%s' name='%s' value=\"", id, label, type, id,
                name);
    html_escape_chunk(req, value);
    send_chunkf(req, "\" %s>", attrs ? attrs : "");
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
        snprintf(out, out_len, "%uh %02um", (unsigned)hours, (unsigned)(minutes % 60));
    } else if (minutes > 0) {
        snprintf(out, out_len, "%um", (unsigned)minutes);
    } else {
        snprintf(out, out_len, "%us", (unsigned)seconds);
    }
}

static bool parse_subnet_text(const char *subnet_str, uint32_t *subnet, uint8_t *prefix_len)
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

static bool parse_subnet_field(const char *body, const char *key, uint32_t *subnet, uint8_t *prefix_len)
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

static esp_err_t validate_port_forwards(httpd_req_t *req, const taplink_config_t *cfg)
{
    for (int i = 0; i < TAPLINK_MAX_PORT_FORWARDS; i++) {
        const port_forward_rule_t *rule = &cfg->port_forwards[i];
        if (!rule->enabled) {
            continue;
        }

        if (rule->listen_port == 0 || rule->target_port == 0 || rule->target_ip == 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Enabled port forward rule is incomplete");
            return ESP_FAIL;
        }
        if (rule->listen_port == 80) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Listen port 80 is reserved for the web UI");
            return ESP_FAIL;
        }
        if (!ip_in_subnet(rule->target_ip, cfg->usb_subnet, cfg->usb_prefix_len)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Target IP must be in the USB subnet");
            return ESP_FAIL;
        }

        uint32_t mask = router_prefix_to_netmask(cfg->usb_prefix_len);
        uint32_t network = cfg->usb_subnet & mask;
        uint32_t gateway = network | htonl(1);
        uint32_t broadcast = network | ~mask;
        if (rule->target_ip == network || rule->target_ip == gateway || rule->target_ip == broadcast) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Target IP cannot be network, gateway, or broadcast");
            return ESP_FAIL;
        }

        for (int j = 0; j < i; j++) {
            if (cfg->port_forwards[j].enabled && cfg->port_forwards[j].listen_port == rule->listen_port) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Duplicate listen ports are not allowed");
                return ESP_FAIL;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t parse_port_forward_rules(httpd_req_t *req, const char *body, taplink_config_t *cfg)
{
    for (int i = 0; i < TAPLINK_MAX_PORT_FORWARDS; i++) {
        port_forward_rule_t *rule = &cfg->port_forwards[i];
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
    }

    return validate_port_forwards(req, cfg);
}

/* ---- GET / (dashboard) ---- */

static esp_err_t dashboard_handler(httpd_req_t *req)
{
    if (!check_auth(req))
        return send_auth_required(req);

    router_dhcp_lease_t leases[ROUTER_MAX_DHCP_LEASES];
    size_t lease_count = router_get_dhcp_leases(leases, ROUTER_MAX_DHCP_LEASES);

    send_page_start(req, "dashboard", false);
    httpd_resp_send_chunk(req,
                          "<div class='card'><h1>ESP32-TapLink</h1>"
                          "<h2>DHCP Leases</h2>",
                          HTTPD_RESP_USE_STRLEN);

    if (lease_count == 0) {
        httpd_resp_send_chunk(req, "<p class='empty'>No devices connected</p>", HTTPD_RESP_USE_STRLEN);
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
                        leases[i].iface, ip[0], ip[1], ip[2], ip[3], m[0], m[1], m[2], m[3], m[4], m[5], expires);
        }
        httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
    send_page_end(req);
    return ESP_OK;
}

/* ---- GET /config ---- */

static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (!check_auth(req))
        return send_auth_required(req);

    char usb_ip_str[16];
    char wifi_ip_str[16];
    format_ipv4(s_cfg->usb_subnet, usb_ip_str, sizeof(usb_ip_str));
    format_ipv4(s_cfg->wifi_subnet, wifi_ip_str, sizeof(wifi_ip_str));

    int8_t txp = s_cfg->wifi_tx_power;
    uint8_t ch = s_cfg->wifi_channel;

    send_page_start(req, "settings", false);
    send_saved_notice(req);
    httpd_resp_send_chunk(req,
                          "<div class='card'><h1>Settings</h1>"
                          "<form method='POST' action='/config'>",
                          HTTPD_RESP_USE_STRLEN);

    send_value_input(req, "WiFi SSID", "text", "wifi_ssid", "wifi_ssid", s_cfg->wifi_ssid, "maxlength='32' required");
    send_value_input(req, "WiFi Password", "password", "wifi_pass", "wifi_pass", s_cfg->wifi_password,
                     "maxlength='63'");
    send_value_input(req, "Confirm WiFi Password", "password", "wifi_pass2", "wifi_pass_confirm", s_cfg->wifi_password,
                     "maxlength='63'");
    httpd_resp_send_chunk(req,
                          "<div class='toggle'><input type='checkbox' id='show_wifi' "
                          "onclick=\"var t=this.checked?'text':'password';"
                          "document.getElementById('wifi_pass').type=t;"
                          "document.getElementById('wifi_pass2').type=t;\">"
                          "<label for='show_wifi'>Show WiFi password</label></div>",
                          HTTPD_RESP_USE_STRLEN);

#define SEL(val) ((txp == (val)) ? " selected" : "")
    send_chunkf(req,
                "<label for='wifi_txp'>WiFi TX Power</label>"
                "<select id='wifi_txp' name='wifi_txp'>"
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
                "<label for='wifi_ch'>WiFi Channel</label>"
                "<select id='wifi_ch' name='wifi_ch'>"
                "<option value='0'%s>Auto</option>",
                CHSEL(0));
    for (int i = 1; i <= 13; i++) {
        send_chunkf(req, "<option value='%d'%s>%d</option>", i, CHSEL(i), i);
    }
    httpd_resp_send_chunk(req, "</select>", HTTPD_RESP_USE_STRLEN);
#undef CHSEL

    send_chunkf(req,
                "<label for='usb_subnet'>USB Subnet</label>"
                "<input type='text' id='usb_subnet' name='usb_subnet' value='%s/%d' "
                "required>"
                "<label for='wifi_subnet'>WiFi Subnet</label>"
                "<input type='text' id='wifi_subnet' name='wifi_subnet' value='%s/%d' "
                "required>"
                "<div class='toggle'>"
                "<input type='checkbox' name='dhcp_gw' id='dhcp_gw' value='1'%s>"
                "<label for='dhcp_gw'>Advertise gateway in DHCP</label></div>"
                "<div class='toggle'>"
                "<input type='checkbox' name='dhcp_dns' id='dhcp_dns' value='1'%s>"
                "<label for='dhcp_dns'>Advertise DNS in DHCP</label></div>",
                usb_ip_str, s_cfg->usb_prefix_len, wifi_ip_str, s_cfg->wifi_prefix_len,
                s_cfg->dhcp_gw_enabled ? " checked" : "", s_cfg->dhcp_dns_enabled ? " checked" : "");

    send_value_input(req, "Admin Password", "password", "admin_pass", "admin_pass", s_cfg->admin_password,
                     "maxlength='64' required");
    send_value_input(req, "Confirm Admin Password", "password", "admin_pass2", "admin_pass_confirm",
                     s_cfg->admin_password, "maxlength='64' required");

    httpd_resp_send_chunk(req, "<button type='submit'>Save</button></form>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "<div class='actions'>", HTTPD_RESP_USE_STRLEN);
    send_reboot_form(req, "/config");
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
    send_page_end(req);
    return ESP_OK;
}

/* ---- POST /config ---- */

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (!check_auth(req))
        return send_auth_required(req);

    char body[2048];
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    taplink_config_t new_cfg = *s_cfg;

    get_form_value(body, "wifi_ssid", new_cfg.wifi_ssid, sizeof(new_cfg.wifi_ssid));

    char wifi_pass[65];
    char wifi_pass_confirm[65];
    char admin_pass[65];
    char admin_pass_confirm[65];
    bool wifi_pass_present = get_form_value(body, "wifi_pass", wifi_pass, sizeof(wifi_pass));
    bool wifi_confirm_present = get_form_value(body, "wifi_pass_confirm", wifi_pass_confirm, sizeof(wifi_pass_confirm));
    bool admin_pass_present = get_form_value(body, "admin_pass", admin_pass, sizeof(admin_pass));
    bool admin_confirm_present =
        get_form_value(body, "admin_pass_confirm", admin_pass_confirm, sizeof(admin_pass_confirm));

    if (wifi_pass_present != wifi_confirm_present || (wifi_pass_present && strcmp(wifi_pass, wifi_pass_confirm) != 0)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "WiFi password confirmation does not match");
        return ESP_FAIL;
    }
    if (wifi_pass_present) {
        size_t wifi_pass_len = strlen(wifi_pass);
        if (wifi_pass_len > 0 && (wifi_pass_len < 8 || wifi_pass_len > 63)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "WiFi password must be 8 to 63 characters");
            return ESP_FAIL;
        }
    }
    if (admin_pass_present != admin_confirm_present ||
        (admin_pass_present && strcmp(admin_pass, admin_pass_confirm) != 0)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Admin password confirmation does not match");
        return ESP_FAIL;
    }
    if (wifi_pass_present) {
        snprintf(new_cfg.wifi_password, sizeof(new_cfg.wifi_password), "%s", wifi_pass);
    }
    if (admin_pass_present) {
        snprintf(new_cfg.admin_password, sizeof(new_cfg.admin_password), "%s", admin_pass);
    }

    if (!parse_subnet_field(body, "usb_subnet", &new_cfg.usb_subnet, &new_cfg.usb_prefix_len)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid USB subnet");
        return ESP_FAIL;
    }

    if (!parse_subnet_field(body, "wifi_subnet", &new_cfg.wifi_subnet, &new_cfg.wifi_prefix_len)) {
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
    if (router_subnets_overlap(new_cfg.usb_subnet, new_cfg.usb_prefix_len, new_cfg.wifi_subnet,
                               new_cfg.wifi_prefix_len)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "USB and WiFi subnets must not overlap");
        return ESP_FAIL;
    }

    esp_err_t err = config_save(&new_cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }
    *s_cfg = new_cfg;

    return send_redirect(req, "/config?saved=1");
}

/* ---- GET/POST /port-forward ---- */

static esp_err_t port_forward_get_handler(httpd_req_t *req)
{
    if (!check_auth(req))
        return send_auth_required(req);

    send_page_start(req, "ports", false);
    send_saved_notice(req);
    if (query_is_saved(req)) {
        httpd_resp_send_chunk(req, "<div class='actions'>", HTTPD_RESP_USE_STRLEN);
        send_reboot_form(req, "/port-forward");
        httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req,
                          "<div class='card'><h1>Port Forwarding</h1>"
                          "<form method='POST' action='/port-forward'>"
                          "<div class='table-wrap'><table class='pf-table'>"
                          "<tr><th>On</th><th>Listen</th><th>Target IP</th><th>Target</th></tr>",
                          HTTPD_RESP_USE_STRLEN);

    for (int i = 0; i < TAPLINK_MAX_PORT_FORWARDS; i++) {
        char target_ip[16] = "";
        char listen_port[8] = "";
        char target_port[8] = "";
        if (s_cfg->port_forwards[i].target_ip) {
            format_ipv4(s_cfg->port_forwards[i].target_ip, target_ip, sizeof(target_ip));
        }
        if (s_cfg->port_forwards[i].listen_port) {
            snprintf(listen_port, sizeof(listen_port), "%u", s_cfg->port_forwards[i].listen_port);
        }
        if (s_cfg->port_forwards[i].target_port) {
            snprintf(target_port, sizeof(target_port), "%u", s_cfg->port_forwards[i].target_port);
        }
        send_chunkf(req,
                    "<tr>"
                    "<td><input type='checkbox' name='pf%d_en' value='1'%s></td>"
                    "<td><input type='text' name='pf%d_lport' value='%s' "
                    "placeholder='2222'></td>"
                    "<td><input type='text' name='pf%d_tip' value='%s' "
                    "placeholder='192.168.5.2'></td>"
                    "<td><input type='text' name='pf%d_tport' value='%s' "
                    "placeholder='22'></td>"
                    "</tr>",
                    i, s_cfg->port_forwards[i].enabled ? " checked" : "", i, listen_port, i, target_ip, i, target_port);
    }

    httpd_resp_send_chunk(req, "</table></div><button type='submit'>Save</button></form>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
    send_page_end(req);
    return ESP_OK;
}

static esp_err_t port_forward_post_handler(httpd_req_t *req)
{
    if (!check_auth(req))
        return send_auth_required(req);

    char body[2048];
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    taplink_config_t new_cfg = *s_cfg;
    if (parse_port_forward_rules(req, body, &new_cfg) != ESP_OK) {
        return ESP_FAIL;
    }

    esp_err_t err = config_save(&new_cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }
    *s_cfg = new_cfg;

    return send_redirect(req, "/port-forward?saved=1");
}

/* ---- Reboot ---- */

static void trigger_rtc_reset(uint32_t delay_ms)
{
    uint32_t slow_hz = rtc_clk_slow_freq_get_hz();
    uint32_t timeout_ticks = (uint32_t)((uint64_t)delay_ms * slow_hz / 1000ULL);
    if (timeout_ticks == 0) {
        timeout_ticks = 1;
    }

    wdt_hal_context_t rtc_wdt_ctx;
    wdt_hal_init(&rtc_wdt_ctx, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&rtc_wdt_ctx);
    wdt_hal_disable(&rtc_wdt_ctx);
    wdt_hal_feed(&rtc_wdt_ctx);
    wdt_hal_config_stage(&rtc_wdt_ctx, WDT_STAGE0, timeout_ticks, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_config_stage(&rtc_wdt_ctx, WDT_STAGE1, timeout_ticks, WDT_STAGE_ACTION_OFF);
    wdt_hal_config_stage(&rtc_wdt_ctx, WDT_STAGE2, timeout_ticks, WDT_STAGE_ACTION_OFF);
    wdt_hal_config_stage(&rtc_wdt_ctx, WDT_STAGE3, timeout_ticks, WDT_STAGE_ACTION_OFF);
    wdt_hal_set_flashboot_en(&rtc_wdt_ctx, true);
    wdt_hal_enable(&rtc_wdt_ctx);
    wdt_hal_write_protect_enable(&rtc_wdt_ctx);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    trigger_rtc_reset(3000);
    usb_ncm_prepare_restart();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (!check_auth(req))
        return send_auth_required(req);

    char body[128];
    char return_buf[32] = "/";
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }
    get_form_value(body, "return", return_buf, sizeof(return_buf));

    const char *return_path = normalize_return_path(return_buf);
    char location[64];
    snprintf(location, sizeof(location), "/rebooting?return=%s", return_path);
    send_redirect(req, location);

    BaseType_t task_ok = xTaskCreate(reboot_task, "web_reboot", 3072, NULL, 5, NULL);
    if (task_ok != pdPASS) {
        trigger_rtc_reset(3000);
        usb_ncm_prepare_restart();
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    return ESP_OK;
}

static esp_err_t rebooting_handler(httpd_req_t *req)
{
    char return_buf[32] = "/";
    get_query_value(req, "return", return_buf, sizeof(return_buf));
    const char *return_path = normalize_return_path(return_buf);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, HTML_HEADER, HTTPD_RESP_USE_STRLEN);
    send_chunkf(req,
                "<div class='card'><h1>Rebooting</h1>"
                "<p class='muted'>Waiting for ESP32-TapLink to come back.</p>"
                "<a class='btn secondary' href='%s'>Return</a></div>"
                "<script>var target='%s';setTimeout(function "
                "poll(){fetch(target,{cache:'no-store'})"
                ".then(function(r){if(r.ok){location.href=target}else{setTimeout("
                "poll,1500)}})"
                ".catch(function(){setTimeout(poll,1500)})},4000)</script>",
                return_path, return_path);
    send_page_end(req);
    return ESP_OK;
}

/* ---- GET /diag (diagnostics) ---- */

typedef struct
{
    char name[6];
    uint32_t ip;
    uint32_t netmask;
    uint8_t mac[6];
    bool up;
    bool link_up;
} netif_diag_t;

typedef struct
{
    uint32_t ip;
    uint8_t mac[6];
    char netif_name[6];
} arp_diag_t;

typedef struct
{
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
    NETIF_FOREACH(n)
    {
        if (ctx->netif_count >= 8)
            break;
        netif_diag_t *d = &ctx->netifs[ctx->netif_count];
        snprintf(d->name, sizeof(d->name), "%c%c%d", n->name[0], n->name[1], n->num);
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
            snprintf(a->netif_name, sizeof(a->netif_name), "%c%c%d", netif->name[0], netif->name[1], netif->num);
            ctx->arp_count++;
        }
    }

    xSemaphoreGive(ctx->done);
}

static esp_err_t diag_handler(httpd_req_t *req)
{
    if (!check_auth(req))
        return send_auth_required(req);

    diag_lwip_ctx_t lwip_ctx = {.done = xSemaphoreCreateBinary()};
    if (lwip_ctx.done) {
        if (tcpip_callback(diag_collect_fn, &lwip_ctx) == ERR_OK)
            xSemaphoreTake(lwip_ctx.done, portMAX_DELAY);
        vSemaphoreDelete(lwip_ctx.done);
    }

    wifi_sta_list_t sta_list = {0};
    esp_wifi_ap_get_sta_list(&sta_list);

    send_page_start(req, "diag", true);

    /* Network interfaces */
    httpd_resp_send_chunk(req,
                          "<div class='card'><h1>Diagnostics</h1>"
                          "<h2>Network Interfaces</h2><div class='table-wrap'>"
                          "<table><tr><th>Name</th><th>IP</th><th>MAC</th><th>State</th></tr>",
                          HTTPD_RESP_USE_STRLEN);
    for (int i = 0; i < lwip_ctx.netif_count; i++) {
        netif_diag_t *d = &lwip_ctx.netifs[i];
        uint8_t *ip = (uint8_t *)&d->ip;
        uint8_t *m = d->mac;
        send_chunkf(req,
                    "<tr><td>%s</td><td>%d.%d.%d.%d</td>"
                    "<td>%02x:%02x:%02x:%02x:%02x:%02x</td>"
                    "<td class='%s'>%s%s</td></tr>",
                    d->name, ip[0], ip[1], ip[2], ip[3], m[0], m[1], m[2], m[3], m[4], m[5],
                    (d->up && d->link_up) ? "ok" : "bad", d->up ? "UP" : "DOWN", d->link_up ? " LINK" : "");
    }
    httpd_resp_send_chunk(req, "</table></div>", HTTPD_RESP_USE_STRLEN);

    /* USB NCM */
    send_chunkf(req,
                "<h2>USB NCM</h2>"
                "<table><tr><th>Status</th><th>RX pkts</th><th>TX pkts</th></tr>"
                "<tr><td class='%s'>%s</td><td>%lu</td><td>%lu</td></tr></table>",
                usb_ncm_is_connected() ? "ok" : "bad", usb_ncm_is_connected() ? "Connected" : "Disconnected",
                (unsigned long)usb_ncm_get_rx_count(), (unsigned long)usb_ncm_get_tx_count());

    /* WiFi stations */
    send_chunkf(req, "<h2>WiFi Stations (%d)</h2>", sta_list.num);
    if (sta_list.num == 0) {
        httpd_resp_send_chunk(req, "<p class='empty'>None</p>", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_chunk(req, "<table><tr><th>MAC</th><th>RSSI</th></tr>", HTTPD_RESP_USE_STRLEN);
        for (int i = 0; i < sta_list.num; i++) {
            uint8_t *m = sta_list.sta[i].mac;
            send_chunkf(req, "<tr><td>%02x:%02x:%02x:%02x:%02x:%02x</td><td>%d</td></tr>", m[0], m[1], m[2], m[3], m[4],
                        m[5], sta_list.sta[i].rssi);
        }
        httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
    }

    /* DHCP leases */
    router_dhcp_lease_t leases[ROUTER_MAX_DHCP_LEASES];
    size_t ccnt = router_get_dhcp_leases(leases, ROUTER_MAX_DHCP_LEASES);
    send_chunkf(req, "<h2>DHCP Leases (%u)</h2>", (unsigned)ccnt);
    if (ccnt == 0) {
        httpd_resp_send_chunk(req, "<p class='empty'>None</p>", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_chunk(req, "<table><tr><th>If</th><th>IP</th><th>MAC</th><th>Expires</th></tr>",
                              HTTPD_RESP_USE_STRLEN);
        for (size_t i = 0; i < ccnt; i++) {
            uint8_t *ip = (uint8_t *)&leases[i].ip;
            uint8_t *m = leases[i].mac;
            char expires[16];
            format_duration(leases[i].expires_in_seconds, expires, sizeof(expires));
            send_chunkf(req,
                        "<tr><td>%s</td><td>%d.%d.%d.%d</td>"
                        "<td>%02x:%02x:%02x:%02x:%02x:%02x</td><td>%s</td></tr>",
                        leases[i].iface, ip[0], ip[1], ip[2], ip[3], m[0], m[1], m[2], m[3], m[4], m[5], expires);
        }
        httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
    }

    /* ARP table */
    send_chunkf(req, "<h2>ARP Table (%d)</h2>", lwip_ctx.arp_count);
    if (lwip_ctx.arp_count == 0) {
        httpd_resp_send_chunk(req, "<p class='empty'>Empty</p>", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_chunk(req, "<table><tr><th>IP</th><th>MAC</th><th>Netif</th></tr>", HTTPD_RESP_USE_STRLEN);
        for (int i = 0; i < lwip_ctx.arp_count; i++) {
            uint8_t *ip = (uint8_t *)&lwip_ctx.arps[i].ip;
            uint8_t *m = lwip_ctx.arps[i].mac;
            send_chunkf(req,
                        "<tr><td>%d.%d.%d.%d</td>"
                        "<td>%02x:%02x:%02x:%02x:%02x:%02x</td>"
                        "<td>%s</td></tr>",
                        ip[0], ip[1], ip[2], ip[3], m[0], m[1], m[2], m[3], m[4], m[5], lwip_ctx.arps[i].netif_name);
        }
        httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
    }

    /* Recent logs */
    httpd_resp_send_chunk(req, "<h2>Recent Logs</h2><pre>", HTTPD_RESP_USE_STRLEN);
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

    send_page_end(req);
    return ESP_OK;
}

/* ---- OTA update ---- */

static esp_err_t update_get_handler(httpd_req_t *req)
{
    if (!check_auth(req))
        return send_auth_required(req);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    send_page_start(req, "update", false);
    httpd_resp_send_chunk(req, "<div class='card'><h1>Firmware Update</h1>", HTTPD_RESP_USE_STRLEN);

    send_chunkf(req,
                "<table>"
                "<tr><th>Version</th><td>%s</td></tr>"
                "<tr><th>Running</th><td>%s</td></tr>"
                "<tr><th>Update target</th><td>%s</td></tr>"
                "</table>",
                FIRMWARE_VERSION, running ? running->label : "?", next ? next->label : "none");

    if (!next) {
        httpd_resp_send_chunk(
            req, "<p class='bad'>No OTA partition available. Flash a partition table with OTA support.</p>",
            HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_chunk(req,
                              "<label for='fw'>Select firmware (.bin)</label>"
                              "<input type='file' id='fw' accept='.bin'>"
                              "<div id='prog' style='display:none;margin:12px 0'>"
                              "<div style='background:#e5e7eb;border-radius:6px;height:20px'>"
                              "<div id='bar' style='background:#1f6feb;height:100%;border-radius:6px;"
                              "width:0;transition:width .3s'></div></div>"
                              "<p id='st' class='muted'>Uploading...</p></div>"
                              "<button id='btn' onclick='doUp()'>Upload &amp; Update</button>"
                              "<script>"
                              "function doUp(){"
                              "var f=document.getElementById('fw').files[0];"
                              "if(!f){alert('Select a file');return}"
                              "var b=document.getElementById('btn'),"
                              "p=document.getElementById('prog'),"
                              "bar=document.getElementById('bar'),"
                              "st=document.getElementById('st');"
                              "b.disabled=true;p.style.display='block';"
                              "var x=new XMLHttpRequest();"
                              "x.open('POST','/update',true);"
                              "x.setRequestHeader('Content-Type','application/octet-stream');"
                              "x.upload.onprogress=function(e){"
                              "if(e.lengthComputable){"
                              "var pct=Math.round(e.loaded/e.total*100);"
                              "bar.style.width=pct+'%';"
                              "st.textContent='Uploading: '+pct+'%'}};",
                              HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req,
                              "x.onload=function(){"
                              "if(x.status===200){"
                              "st.textContent='Update complete! Rebooting...';"
                              "bar.style.width='100%';bar.style.background='#047857';"
                              "setTimeout(function(){location.href='/rebooting?return=/update'},1000)"
                              "}else{"
                              "st.textContent='Error: '+x.responseText;"
                              "bar.style.background='#b91c1c';b.disabled=false}};"
                              "x.onerror=function(){"
                              "st.textContent='Upload failed (network error)';"
                              "bar.style.background='#b91c1c';b.disabled=false};"
                              "x.send(f)}</script>",
                              HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
    send_page_end(req);
    return ESP_OK;
}

static volatile bool s_ota_in_progress;

#define OTA_BUF_SIZE 2048
#define OTA_MAX_TIMEOUTS 5
#define OTA_LOG_INTERVAL (100 * 1024)

static esp_err_t update_post_handler(httpd_req_t *req)
{
    if (!check_auth(req))
        return send_auth_required(req);

    if (s_ota_in_progress) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA already in progress");
        return ESP_FAIL;
    }

    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware data");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    if (req->content_len > update_partition->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware too large for partition");
        return ESP_FAIL;
    }

    s_ota_in_progress = true;
    ESP_LOGI(TAG, "OTA start: %u bytes -> %s", (unsigned)req->content_len, update_partition->label);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        s_ota_in_progress = false;
        return ESP_FAIL;
    }

    char buf[OTA_BUF_SIZE];
    size_t remaining = req->content_len;
    size_t written = 0;
    size_t next_log = OTA_LOG_INTERVAL;
    int timeouts = 0;

    while (remaining > 0) {
        size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts >= OTA_MAX_TIMEOUTS) {
                ESP_LOGE(TAG, "OTA timed out at %u/%u bytes", (unsigned)written, (unsigned)req->content_len);
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Upload timed out");
                s_ota_in_progress = false;
                return ESP_FAIL;
            }
            continue;
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "OTA receive error at %u/%u bytes", (unsigned)written, (unsigned)req->content_len);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            s_ota_in_progress = false;
            return ESP_FAIL;
        }
        timeouts = 0;

        err = esp_ota_write(ota_handle, buf, (size_t)received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            s_ota_in_progress = false;
            return ESP_FAIL;
        }

        written += (size_t)received;
        remaining -= (size_t)received;

        if (written >= next_log) {
            ESP_LOGI(TAG, "OTA progress: %u/%u bytes (%u%%)", (unsigned)written, (unsigned)req->content_len,
                     (unsigned)(written * 100 / req->content_len));
            next_log += OTA_LOG_INTERVAL;
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Firmware validation failed");
        s_ota_in_progress = false;
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
        s_ota_in_progress = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA complete: %u bytes written to %s", (unsigned)written, update_partition->label);
    httpd_resp_sendstr(req, "OK");

    xTaskCreate(reboot_task, "ota_reboot", 3072, NULL, 5, NULL);
    return ESP_OK;
}

/* ---- Start ---- */

esp_err_t web_server_start(taplink_config_t *cfg)
{
    s_cfg = cfg;

    s_log_mutex = xSemaphoreCreateMutex();
    esp_log_set_vprintf(log_vprintf);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 16;
    httpd_handle_t server = NULL;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t dashboard = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = dashboard_handler,
    };
    const httpd_uri_t style = {
        .uri = "/style.css",
        .method = HTTP_GET,
        .handler = style_handler,
    };
    const httpd_uri_t config_get = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
    };
    const httpd_uri_t config_post = {
        .uri = "/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
    };
    const httpd_uri_t port_forward_get = {
        .uri = "/port-forward",
        .method = HTTP_GET,
        .handler = port_forward_get_handler,
    };
    const httpd_uri_t port_forward_post = {
        .uri = "/port-forward",
        .method = HTTP_POST,
        .handler = port_forward_post_handler,
    };
    const httpd_uri_t diag = {
        .uri = "/diag",
        .method = HTTP_GET,
        .handler = diag_handler,
    };
    const httpd_uri_t reboot_post = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = reboot_post_handler,
    };
    const httpd_uri_t rebooting = {
        .uri = "/rebooting",
        .method = HTTP_GET,
        .handler = rebooting_handler,
    };
    const httpd_uri_t update_get = {
        .uri = "/update",
        .method = HTTP_GET,
        .handler = update_get_handler,
    };
    const httpd_uri_t update_post = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = update_post_handler,
    };

    httpd_register_uri_handler(server, &dashboard);
    httpd_register_uri_handler(server, &style);
    httpd_register_uri_handler(server, &config_get);
    httpd_register_uri_handler(server, &config_post);
    httpd_register_uri_handler(server, &port_forward_get);
    httpd_register_uri_handler(server, &port_forward_post);
    httpd_register_uri_handler(server, &diag);
    httpd_register_uri_handler(server, &reboot_post);
    httpd_register_uri_handler(server, &rebooting);
    httpd_register_uri_handler(server, &update_get);
    httpd_register_uri_handler(server, &update_post);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}
