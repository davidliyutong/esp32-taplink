#pragma once

#include "esp_err.h"
#include "nvs_config.h"

esp_err_t port_forward_start(const netlink_config_t *cfg);
