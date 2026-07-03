idf_component_register(
    SRCS "eth_w5500.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_eth esp_netif driver esp_event w5500
)
