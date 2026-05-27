#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "nvs_config.h"
#include "usb_ncm.h"
#include "wifi_ap.h"
#include "bridge.h"
#include "web_server.h"

static const char *TAG = "main";

#define LED_GPIO      GPIO_NUM_21
#define BUTTON_GPIO   GPIO_NUM_0
#define RESET_HOLD_MS 5000

static netlink_config_t s_config;
static volatile bool s_booting = true;

static void led_button_task(void *arg)
{
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);

    uint32_t btn_held_ms = 0;

    while (1) {
        int blink_ms = s_booting ? 100 : (usb_ncm_is_connected() ? 1000 : 500);

        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(blink_ms));
        gpio_set_level(LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(blink_ms));

        if (gpio_get_level(BUTTON_GPIO) == 0) {
            btn_held_ms += blink_ms * 2;
            if (btn_held_ms >= RESET_HOLD_MS) {
                ESP_LOGW(TAG, "Factory reset triggered by long press");
                gpio_set_level(LED_GPIO, 1);
                nvs_flash_erase();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        } else {
            btn_held_ms = 0;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "esp32-netlink starting (version: %s)", FIRMWARE_VERSION);

    xTaskCreate(led_button_task, "led_btn", 2048, NULL, 1, NULL);

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

    ESP_ERROR_CHECK(web_server_start(&s_config));
    ESP_ERROR_CHECK(wifi_ap_start(&s_config));
    ESP_ERROR_CHECK(usb_ncm_start());

    s_booting = false;
    ESP_LOGI(TAG, "All systems up. Bridge ready.");
}
