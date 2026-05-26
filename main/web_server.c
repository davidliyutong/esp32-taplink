#include "web_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
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
    ".card{background:#fff;border-radius:8px;padding:24px;box-shadow:0 2px 4px rgba(0,0,0,.1)}"
    "h1{color:#333;font-size:1.4em;margin:0 0 20px}"
    "label{display:block;margin:12px 0 4px;color:#555;font-size:.9em}"
    "input[type=text],input[type=password]{width:100%;padding:8px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;font-size:1em}"
    "button{background:#2196F3;color:#fff;border:none;padding:10px 24px;border-radius:4px;cursor:pointer;font-size:1em;margin-top:16px;width:100%}"
    "button:hover{background:#1976D2}"
    ".msg{background:#E8F5E9;color:#2E7D32;padding:12px;border-radius:4px;margin-bottom:16px}"
    "</style></head><body><div class='card'>";

static const char HTML_FOOTER[] = "</div></body></html>";

/* ---- GET /config ---- */

static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    char ip_str[16];
    struct in_addr subnet_addr = {.s_addr = s_cfg->dhcp_subnet};
    inet_ntoa_r(subnet_addr, ip_str, sizeof(ip_str));

    char page[2048];
    int len = snprintf(page, sizeof(page),
        "%s"
        "<h1>ESP32-NetLink Settings</h1>"
        "<form method='POST' action='/config'>"
        "<label>WiFi SSID</label>"
        "<input type='text' name='wifi_ssid' value='%s' maxlength='32' required>"
        "<label>WiFi Password</label>"
        "<input type='password' name='wifi_pass' value='%s' maxlength='64'>"
        "<label>DHCP Subnet (e.g. 192.168.4.0/24)</label>"
        "<input type='text' name='dhcp_subnet' value='%s/%d' required>"
        "<label>Admin Password</label>"
        "<input type='password' name='admin_pass' value='%s' maxlength='64' required>"
        "<button type='submit'>Save &amp; Reboot</button>"
        "</form>"
        "%s",
        HTML_HEADER,
        s_cfg->wifi_ssid,
        s_cfg->wifi_password,
        ip_str, s_cfg->dhcp_prefix_len,
        s_cfg->admin_password,
        HTML_FOOTER);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, len);
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

    char page[1280];
    int len = snprintf(page, sizeof(page),
        "%s"
        "<div class='msg'>Settings saved. Rebooting in 3 seconds...</div>"
        "<h1>ESP32-NetLink</h1>"
        "<p>Please reconnect after reboot.</p>"
        "%s",
        HTML_HEADER, HTML_FOOTER);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, len);

    TimerHandle_t timer = xTimerCreate("restart", pdMS_TO_TICKS(3000), pdFALSE, NULL, restart_cb);
    xTimerStart(timer, 0);
    return ESP_OK;
}

/* ---- GET / ---- */

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/config");
    httpd_resp_send(req, NULL, 0);
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

    const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler,
    };
    const httpd_uri_t config_get = {
        .uri = "/config", .method = HTTP_GET, .handler = config_get_handler,
    };
    const httpd_uri_t config_post = {
        .uri = "/config", .method = HTTP_POST, .handler = config_post_handler,
    };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &config_get);
    httpd_register_uri_handler(server, &config_post);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}
