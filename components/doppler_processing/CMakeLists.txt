idf_component_register(
    SRCS "doppler_processing.c"
    INCLUDE_DIRS "include"
    REQUIRES doppler_events esp_event esp_timer
)
