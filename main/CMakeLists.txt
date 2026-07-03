idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_eth esp_event esp_netif esp_driver_spi esp_driver_gpio
)
