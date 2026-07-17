# NOTA: REQUIRES/PRIV_REQUIRES não pode depender de valores CONFIG_ (o
# ESP-IDF expande os requisitos de componente ANTES de carregar o
# sdkconfig.cmake, então CONFIG_NETIF_BACKEND_WIFI ainda não existe aqui).
# Por isso esp_wifi entra sempre no REQUIRES, mesmo só sendo usado no
# backend de teste (guardado por #if CONFIG_NETIF_BACKEND_WIFI em
# app_main.c) -- o linker (--gc-sections, padrão no IDF) descarta os
# objetos não referenciados quando o backend de produção (W5500) está
# ativo. nvs_flash já é usado nos dois backends (device_config sempre
# precisa de NVS; o Wi-Fi de teste também usa pra calibração de rádio).
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        eth_w5500
        mqtt_app
        tcp_server_app
        tcp_client_app
        proxy_events
        device_config
        esp_netif
        esp_event
        esp_timer
        driver
        esp_driver_gpio
        cjson
        esp_wifi
        nvs_flash
)
