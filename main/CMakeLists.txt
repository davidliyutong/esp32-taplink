idf_component_register(
    SRCS "app_main.c"
         "nvs_config.c"
         "usb_ncm.c"
         "wifi_ap.c"
         "bridge.c"
         "web_server.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif nvs_flash esp_http_server esp_event esp_eth lwip
)
