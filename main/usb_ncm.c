#include "usb_ncm.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_eth_com.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "usb_ncm";

typedef struct {
    esp_netif_driver_base_t base;
    uint8_t mac_addr[6];
} usb_ncm_driver_t;

static usb_ncm_driver_t s_driver;
static volatile bool s_connected;

static esp_err_t usb_ncm_transmit(void *h, void *buffer, size_t len)
{
    return tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
}

static void usb_ncm_free_rx(void *h, void *buffer)
{
    free(buffer);
}

static esp_err_t usb_ncm_recv_cb(void *buffer, uint16_t len, void *ctx)
{
    usb_ncm_driver_t *driver = (usb_ncm_driver_t *)ctx;
    if (driver->base.netif) {
        void *buf_copy = malloc(len);
        if (!buf_copy) return ESP_ERR_NO_MEM;
        memcpy(buf_copy, buffer, len);
        esp_netif_receive(driver->base.netif, buf_copy, len, buf_copy);
    }
    return ESP_OK;
}

static void usb_ncm_port_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    void *handle = *(void **)event_data;
    if (handle != &s_driver) return;

    switch (event_id) {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Port action: start");
        esp_netif_action_start(s_driver.base.netif, 0, 0, NULL);
        break;
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Port action: connected");
        esp_netif_action_connected(s_driver.base.netif, 0, 0, NULL);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        esp_netif_action_disconnected(s_driver.base.netif, 0, 0, NULL);
        break;
    case ETHERNET_EVENT_STOP:
        esp_netif_action_stop(s_driver.base.netif, 0, 0, NULL);
        break;
    }
}

bool usb_ncm_is_connected(void)
{
    return s_connected;
}

static void usb_ncm_init_cb(void *ctx)
{
    s_connected = true;
    ESP_LOGI(TAG, "USB NCM initialized by host");
    void *handle = &s_driver;
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_START, &handle, sizeof(handle), 0);
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_CONNECTED, &handle, sizeof(handle), 0);
}

static esp_err_t usb_ncm_post_attach(esp_netif_t *esp_netif, void *args)
{
    usb_ncm_driver_t *driver = (usb_ncm_driver_t *)args;
    driver->base.netif = esp_netif;

    const esp_netif_driver_ifconfig_t ifconfig = {
        .handle = driver,
        .transmit = usb_ncm_transmit,
        .driver_free_rx_buffer = usb_ncm_free_rx,
    };
    ESP_ERROR_CHECK(esp_netif_set_driver_config(esp_netif, &ifconfig));

    esp_netif_set_mac(esp_netif, driver->mac_addr);
    return ESP_OK;
}

esp_netif_t *usb_ncm_netif_create(void)
{
    uint8_t base_mac[6];
    esp_efuse_mac_get_default(base_mac);
    memcpy(s_driver.mac_addr, base_mac, 6);
    s_driver.mac_addr[5] = (s_driver.mac_addr[5] + 4) & 0xFF;
    s_driver.mac_addr[0] |= 0x02;
    s_driver.mac_addr[0] &= ~0x01;

    esp_netif_inherent_config_t base_cfg = {
        .flags = 0,
        .mac = {0},
        .ip_info = NULL,
        .get_ip_event = 0,
        .lost_ip_event = 0,
        .if_key = "USB_NCM",
        .if_desc = "usb ncm",
        .route_prio = 50,
        .bridge_info = NULL,
    };

    esp_netif_config_t netif_cfg = {
        .base = &base_cfg,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    assert(netif);

    s_driver.base.post_attach = usb_ncm_post_attach;
    ESP_ERROR_CHECK(esp_netif_attach(netif, &s_driver));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                usb_ncm_port_event_handler, NULL));

    ESP_LOGI(TAG, "USB NCM netif created, MAC=%02x:%02x:%02x:%02x:%02x:%02x",
             s_driver.mac_addr[0], s_driver.mac_addr[1], s_driver.mac_addr[2],
             s_driver.mac_addr[3], s_driver.mac_addr[4], s_driver.mac_addr[5]);
    return netif;
}

esp_err_t usb_ncm_start(void)
{
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = usb_ncm_recv_cb,
        .free_tx_buffer = NULL,
        .on_init_callback = usb_ncm_init_cb,
        .user_context = &s_driver,
    };
    memcpy(net_cfg.mac_addr, s_driver.mac_addr, 6);
    ESP_ERROR_CHECK(tinyusb_net_init(&net_cfg));

    void *handle = &s_driver;
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_START, &handle, sizeof(handle), portMAX_DELAY);
    esp_event_post(ETH_EVENT, ETHERNET_EVENT_CONNECTED, &handle, sizeof(handle), portMAX_DELAY);

    ESP_LOGI(TAG, "USB NCM started");
    return ESP_OK;
}
