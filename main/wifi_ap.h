#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include "nvs_config.h"

esp_netif_t *wifi_ap_create(void);
esp_err_t wifi_ap_start(const netlink_config_t *cfg);
