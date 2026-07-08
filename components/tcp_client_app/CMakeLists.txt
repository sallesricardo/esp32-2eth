idf_component_register(
    SRCS "tcp_client_app.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip freertos esp_netif
)
