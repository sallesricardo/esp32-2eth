idf_component_register(
    SRCS "tcp_server_app.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip freertos
)
