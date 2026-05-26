#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include "nvs_config.h"

esp_netif_t *bridge_create(const netlink_config_t *cfg);
esp_err_t bridge_start(esp_netif_t *br_netif, esp_netif_t *usb_netif, esp_netif_t *wifi_netif);
