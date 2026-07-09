idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES eth_w5500 mqtt_app tcp_server_app tcp_client_app proxy_events esp_netif esp_event esp_timer driver esp_driver_gpio cjson esp_wifi nvs_flash
)
