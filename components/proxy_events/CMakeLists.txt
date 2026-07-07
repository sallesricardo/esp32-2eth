idf_component_register(
    SRCS "proxy_events.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_timer freertos
)
