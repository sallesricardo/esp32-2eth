idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES eth_w5500 mqtt_app tcp_server_app tcp_client_app esp_netif esp_event driver esp_driver_gpio
)
