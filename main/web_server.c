#include "web_server.h"
#include "bridge.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

static const char *TAG = "web";
static netlink_config_t *s_cfg;

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

/* ---- ARP client enumeration ---- */

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
} client_entry_t;

typedef struct {
    client_entry_t *out;
    int max_clients;
    struct netif *br_lwip;
    uint32_t self_ip;
    int count;
    SemaphoreHandle_t done;
} arp_enum_ctx_t;

static void arp_enum_fn(void *arg)
{
    arp_enum_ctx_t *ctx = (arp_enum_ctx_t *)arg;
    ctx->count = 0;
    for (size_t i = 0; i < ARP_TABLE_SIZE && ctx->count < ctx->max_clients; i++) {
        ip4_addr_t *ip = NULL;
        struct netif *netif = NULL;
        struct eth_addr *mac = NULL;
        if (etharp_get_entry(i, &ip, &netif, &mac)) {
            if (netif == ctx->br_lwip && ip->addr != ctx->self_ip) {
                ctx->out[ctx->count].ip = ip->addr;
                memcpy(ctx->out[ctx->count].mac, mac->addr, 6);
                ctx->count++;
            }
        }
    }
    xSemaphoreGive(ctx->done);
}

static int get_clients(client_entry_t *out, int max_clients)
{
    esp_netif_t *br = bridge_get_netif();
    if (!br) return 0;

    struct netif *br_lwip = esp_netif_get_netif_impl(br);
    if (!br_lwip) return 0;

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(br, &ip_info);

    arp_enum_ctx_t ctx = {
        .out = out,
        .max_clients = max_clients,
        .br_lwip = br_lwip,
        .self_ip = ip_info.ip.addr,
        .count = 0,
        .done = xSemaphoreCreateBinary(),
    };
    if (!ctx.done) return 0;

    if (tcpip_callback(arp_enum_fn, &ctx) == ERR_OK) {
        xSemaphoreTake(ctx.done, portMAX_DELAY);
    }
    vSemaphoreDelete(ctx.done);
    return ctx.count;
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
        "<h2>Connected Devices</h2>",
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

/* ---- Start ---- */

esp_err_t web_server_start(netlink_config_t *cfg)
{
    s_cfg = cfg;

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

    httpd_register_uri_handler(server, &dashboard);
    httpd_register_uri_handler(server, &config_get);
    httpd_register_uri_handler(server, &config_post);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}
