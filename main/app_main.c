#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "nvs_config.h"
#include "usb_ncm.h"
#include "wifi_ap.h"
#include "bridge.h"
#include "web_server.h"

static const char *TAG = "main";

static netlink_config_t s_config;

void app_main(void)
{
    ESP_LOGI(TAG, "esp32-netlink starting (version: %s)", FIRMWARE_VERSION);

    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(config_load(&s_config));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    esp_netif_t *usb_netif = usb_ncm_netif_create();
    esp_netif_t *wifi_netif = wifi_ap_create();

    esp_netif_t *br_netif = bridge_create(&s_config);
    ESP_ERROR_CHECK(bridge_start(br_netif, usb_netif, wifi_netif));

    ESP_ERROR_CHECK(wifi_ap_start(&s_config));

    gpio_set_level(GPIO_NUM_21, 1);

    ESP_ERROR_CHECK(usb_ncm_start());

    ESP_ERROR_CHECK(web_server_start(&s_config));

    ESP_LOGI(TAG, "All systems up. Bridge ready.");

    while (1) {
        gpio_set_level(GPIO_NUM_21, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(GPIO_NUM_21, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
