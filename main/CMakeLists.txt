# CONFIG_NETIF_BACKEND_WIFI vem do sdkconfig (Kconfig.projbuild) e já está
# disponível aqui como variável de cache do CMake nesse ponto do build.
# esp_wifi/nvs_flash só entram no link quando o modo de teste (sem W5500)
# está habilitado, pra não inchar o binário de produção com código não usado.
set(EXTRA_REQUIRES "")
if(CONFIG_NETIF_BACKEND_WIFI)
    list(APPEND EXTRA_REQUIRES esp_wifi nvs_flash)
endif()

idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES eth_w5500 mqtt_app tcp_server_app tcp_client_app proxy_events esp_netif esp_event esp_timer driver esp_driver_gpio cjson ${EXTRA_REQUIRES}
)
