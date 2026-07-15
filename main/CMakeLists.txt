# NOTA: REQUIRES/PRIV_REQUIRES não pode depender de valores CONFIG_ (o
# ESP-IDF expande os requisitos de componente ANTES de carregar o
# sdkconfig.cmake, então CONFIG_NETIF_BACKEND_WIFI ainda não existe aqui).
# Por isso esp_wifi/nvs_flash entram sempre no REQUIRES; quando o backend
# de produção (W5500) está ativo, esse código simplesmente não é chamado
# (guardado por #if CONFIG_NETIF_BACKEND_WIFI em app_main.c) e o linker
# (--gc-sections, padrão no IDF) descarta os objetos não referenciados.
idf_component_register(
    SRCS "mqtts_example.c" "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES eth_w5500 mqtt_app tcp_server_app tcp_client_app proxy_events esp_netif esp_event esp_timer driver esp_driver_gpio cjson esp_wifi nvs_flash
)
