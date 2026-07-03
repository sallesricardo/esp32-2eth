#include "tcp_server_app.h"

#include <string.h>
#include <errno.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "tcp_server";

typedef struct {
    uint16_t port;
} tcp_server_task_args_t;

static void tcp_server_task(void *pvParameters)
{
    tcp_server_task_args_t *args = (tcp_server_task_args_t *)pvParameters;
    uint16_t port = args->port;
    vPortFree(args); // copiado no start(), pode liberar já

    char rx_buffer[128];
    char addr_str[64];
    struct sockaddr_in dest_addr = {
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket created");

    if (bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", port);

    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "Error during listen: errno %d", errno);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket listening");

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        if (source_addr.sin_family == PF_INET) {
            inet_ntoa_r(source_addr.sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
        }
        ESP_LOGI(TAG, "Socket accepted ip: %s", addr_str);

        int len;
        do {
            len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len < 0) {
                ESP_LOGE(TAG, "Error during receiving: errno %d", errno);
            } else if (len == 0) {
                ESP_LOGW(TAG, "Connection closed");
            } else {
                rx_buffer[len] = 0;
                ESP_LOGI(TAG, "Received %d bytes: %s", len, rx_buffer);
                // TODO: processar dados binários recebidos
            }
        } while (len > 0);

        shutdown(sock, 0);
        close(sock);
    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}

void tcp_server_app_start(uint16_t port)
{
    tcp_server_task_args_t *args = pvPortMalloc(sizeof(tcp_server_task_args_t));
    args->port = port;
    xTaskCreate(tcp_server_task, "tcp_server", 4096, args, 5, NULL);
}
