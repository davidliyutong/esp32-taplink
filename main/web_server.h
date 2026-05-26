#pragma once

#include "esp_err.h"
#include "nvs_config.h"

esp_err_t web_server_start(netlink_config_t *cfg);
