idf_component_register(
    SRCS "mqtt_app.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif mqtt esp_event
    EMBED_TXTFILES "certs/ca.crt" "certs/client.crt" "certs/client.key"
)
