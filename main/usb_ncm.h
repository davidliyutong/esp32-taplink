#pragma once

#include "esp_err.h"
#include "esp_netif.h"

esp_netif_t *usb_ncm_netif_create(void);
esp_err_t usb_ncm_start(void);
bool usb_ncm_is_connected(void);
