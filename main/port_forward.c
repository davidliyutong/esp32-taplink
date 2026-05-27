#include "port_forward.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "router.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "port_forward";

enum
{
    PORT_FORWARD_TASK_STACK = 4096,
    PORT_FORWARD_TASK_PRIO = 4,
    PORT_FORWARD_SESSIONS_PER_RULE = 2,
    PORT_FORWARD_CONNECT_TIMEOUT_SEC = 10,
    PORT_FORWARD_IO_TIMEOUT_SEC = 30,
    PORT_FORWARD_IDLE_TIMEOUT_SEC = 1800,
};

typedef struct
{
    port_forward_rule_t rule;
    uint32_t listen_ip;
    uint32_t wifi_subnet;
    uint8_t wifi_prefix_len;
    SemaphoreHandle_t session_slots;
    uint8_t index;
} port_forward_listener_t;

typedef struct
{
    port_forward_listener_t *listener;
    int client_fd;
} port_forward_session_t;

static port_forward_listener_t s_listeners[NETLINK_MAX_PORT_FORWARDS];
static TaskHandle_t s_listener_tasks[NETLINK_MAX_PORT_FORWARDS];
static bool s_started;

static bool ip_in_subnet(uint32_t ip, uint32_t subnet, uint8_t prefix_len)
{
    uint32_t mask = router_prefix_to_netmask(prefix_len);
    return (ip & mask) == (subnet & mask);
}

static void format_ipv4(uint32_t addr, char *out, size_t out_len)
{
    uint8_t *ip = (uint8_t *)&addr;
    snprintf(out, out_len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static void set_socket_timeouts(int fd)
{
    struct timeval timeout = {
        .tv_sec = PORT_FORWARD_IO_TIMEOUT_SEC,
        .tv_usec = 0,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static bool restore_socket_flags(int fd, int flags)
{
    return fcntl(fd, F_SETFL, flags) == 0;
}

static bool connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addr_len, int timeout_sec)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return connect(fd, addr, addr_len) == 0;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return connect(fd, addr, addr_len) == 0;
    }

    int rc = connect(fd, addr, addr_len);
    if (rc == 0) {
        restore_socket_flags(fd, flags);
        return true;
    }
    if (errno != EINPROGRESS) {
        restore_socket_flags(fd, flags);
        return false;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval timeout = {
        .tv_sec = timeout_sec,
        .tv_usec = 0,
    };

    rc = select(fd + 1, NULL, &wfds, NULL, &timeout);
    if (rc <= 0) {
        restore_socket_flags(fd, flags);
        if (rc == 0) {
            errno = ETIMEDOUT;
        }
        return false;
    }

    int so_error = 0;
    socklen_t so_error_len = sizeof(so_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0) {
        restore_socket_flags(fd, flags);
        return false;
    }

    restore_socket_flags(fd, flags);
    if (so_error != 0) {
        errno = so_error;
        return false;
    }
    return true;
}

static bool send_all(int fd, const uint8_t *data, int len)
{
    int sent = 0;
    while (sent < len) {
        int rc = send(fd, data + sent, len - sent, 0);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc <= 0) {
            return false;
        }
        sent += rc;
    }
    return true;
}

static bool relay_ready_socket(int from_fd, int to_fd)
{
    uint8_t buf[1024];
    int len = recv(from_fd, buf, sizeof(buf), 0);
    if (len < 0 && errno == EINTR) {
        return true;
    }
    if (len <= 0) {
        return false;
    }
    return send_all(to_fd, buf, len);
}

static void session_task(void *arg)
{
    port_forward_session_t *session = (port_forward_session_t *)arg;
    port_forward_listener_t *listener = session->listener;
    int client_fd = session->client_fd;
    int target_fd = -1;

    struct sockaddr_in target = {
        .sin_family = AF_INET,
        .sin_port = htons(listener->rule.target_port),
        .sin_addr.s_addr = listener->rule.target_ip,
    };

    target_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (target_fd < 0) {
        ESP_LOGW(TAG, "rule %u: target socket failed: errno=%d", listener->index, errno);
        goto done;
    }
    set_socket_timeouts(target_fd);

    if (!connect_with_timeout(target_fd, (struct sockaddr *)&target, sizeof(target),
                              PORT_FORWARD_CONNECT_TIMEOUT_SEC)) {
        ESP_LOGW(TAG, "rule %u: connect target failed: errno=%d", listener->index, errno);
        goto done;
    }

    ESP_LOGI(TAG, "rule %u: session connected", listener->index);

    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client_fd, &rfds);
        FD_SET(target_fd, &rfds);
        int maxfd = client_fd > target_fd ? client_fd : target_fd;
        struct timeval timeout = {
            .tv_sec = PORT_FORWARD_IDLE_TIMEOUT_SEC,
            .tv_usec = 0,
        };

        int rc = select(maxfd + 1, &rfds, NULL, NULL, &timeout);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc <= 0) {
            break;
        }
        if (FD_ISSET(client_fd, &rfds) && !relay_ready_socket(client_fd, target_fd)) {
            break;
        }
        if (FD_ISSET(target_fd, &rfds) && !relay_ready_socket(target_fd, client_fd)) {
            break;
        }
    }

done:
    if (target_fd >= 0) {
        close(target_fd);
    }
    close(client_fd);
    xSemaphoreGive(listener->session_slots);
    free(session);
    vTaskDelete(NULL);
}

static void listener_task(void *arg)
{
    port_forward_listener_t *listener = (port_forward_listener_t *)arg;
    int listen_fd = -1;

    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "rule %u: listen socket failed: errno=%d", listener->index, errno);
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(listener->rule.listen_port),
        .sin_addr.s_addr = listener->listen_ip,
    };

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        char bind_ip[16];
        format_ipv4(addr.sin_addr.s_addr, bind_ip, sizeof(bind_ip));
        ESP_LOGE(TAG, "rule %u: bind %s:%u failed: errno=%d", listener->index, bind_ip, listener->rule.listen_port,
                 errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, PORT_FORWARD_SESSIONS_PER_RULE) != 0) {
        ESP_LOGE(TAG, "rule %u: listen failed: errno=%d", listener->index, errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    char listen_ip[16];
    char target_ip[16];
    format_ipv4(addr.sin_addr.s_addr, listen_ip, sizeof(listen_ip));
    format_ipv4(listener->rule.target_ip, target_ip, sizeof(target_ip));
    ESP_LOGI(TAG, "rule %u: %s:%u -> %s:%u", listener->index, listen_ip, listener->rule.listen_port, target_ip,
             listener->rule.target_port);

    while (true) {
        struct sockaddr_in peer = {0};
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGW(TAG, "rule %u: accept failed: errno=%d", listener->index, errno);
            continue;
        }

        if (!ip_in_subnet(peer.sin_addr.s_addr, listener->wifi_subnet, listener->wifi_prefix_len)) {
            char peer_ip[16];
            format_ipv4(peer.sin_addr.s_addr, peer_ip, sizeof(peer_ip));
            ESP_LOGW(TAG, "rule %u: reject non-WiFi peer %s", listener->index, peer_ip);
            close(client_fd);
            continue;
        }

        if (xSemaphoreTake(listener->session_slots, 0) != pdTRUE) {
            ESP_LOGW(TAG, "rule %u: session limit reached", listener->index);
            close(client_fd);
            continue;
        }
        set_socket_timeouts(client_fd);

        port_forward_session_t *session = calloc(1, sizeof(*session));
        if (!session) {
            xSemaphoreGive(listener->session_slots);
            close(client_fd);
            continue;
        }
        session->listener = listener;
        session->client_fd = client_fd;

        char task_name[16];
        snprintf(task_name, sizeof(task_name), "pf_sess_%u", listener->index);
        if (xTaskCreate(session_task, task_name, PORT_FORWARD_TASK_STACK, session, PORT_FORWARD_TASK_PRIO, NULL) !=
            pdPASS) {
            ESP_LOGE(TAG, "rule %u: session task create failed", listener->index);
            free(session);
            xSemaphoreGive(listener->session_slots);
            close(client_fd);
        }
    }
}

static bool rule_is_valid(const netlink_config_t *cfg, const port_forward_rule_t *rule)
{
    if (!rule->enabled) {
        return false;
    }
    if (rule->listen_port == 0 || rule->listen_port == 80 || rule->target_port == 0) {
        return false;
    }
    if (rule->target_ip == 0 || !ip_in_subnet(rule->target_ip, cfg->usb_subnet, cfg->usb_prefix_len)) {
        return false;
    }
    return true;
}

esp_err_t port_forward_start(const netlink_config_t *cfg)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_netif_ip_info_t wifi_ip;
    router_make_ip_info(cfg->wifi_subnet, cfg->wifi_prefix_len, true, &wifi_ip);

    for (int i = 0; i < NETLINK_MAX_PORT_FORWARDS; i++) {
        const port_forward_rule_t *rule = &cfg->port_forwards[i];
        if (!rule_is_valid(cfg, rule)) {
            continue;
        }

        bool duplicate = false;
        for (int j = 0; j < i; j++) {
            if (cfg->port_forwards[j].enabled && cfg->port_forwards[j].listen_port == rule->listen_port) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            ESP_LOGW(TAG, "rule %d skipped: duplicate listen port %u", i, rule->listen_port);
            continue;
        }

        s_listeners[i] = (port_forward_listener_t){
            .rule = *rule,
            .listen_ip = wifi_ip.ip.addr,
            .wifi_subnet = cfg->wifi_subnet,
            .wifi_prefix_len = cfg->wifi_prefix_len,
            .index = i,
        };
        s_listeners[i].session_slots =
            xSemaphoreCreateCounting(PORT_FORWARD_SESSIONS_PER_RULE, PORT_FORWARD_SESSIONS_PER_RULE);
        if (!s_listeners[i].session_slots) {
            ESP_LOGE(TAG, "rule %d skipped: semaphore allocation failed", i);
            continue;
        }

        char task_name[16];
        snprintf(task_name, sizeof(task_name), "pf_listen_%d", i);
        if (xTaskCreate(listener_task, task_name, PORT_FORWARD_TASK_STACK, &s_listeners[i], PORT_FORWARD_TASK_PRIO,
                        &s_listener_tasks[i]) != pdPASS) {
            ESP_LOGE(TAG, "rule %d skipped: listener task create failed", i);
            vSemaphoreDelete(s_listeners[i].session_slots);
            s_listeners[i].session_slots = NULL;
        }
    }

    s_started = true;
    return ESP_OK;
}
