#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include "nvs_config.h"

esp_netif_t *usb_ncm_netif_create(const netlink_config_t *cfg);
esp_err_t usb_ncm_start(void);
bool usb_ncm_is_connected(void);
uint32_t usb_ncm_get_rx_count(void);
uint32_t usb_ncm_get_tx_count(void);
