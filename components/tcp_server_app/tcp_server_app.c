#include "tcp_server_app.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "tcp_server";

typedef struct {
    uint16_t port;
    char *welcome_msg;              // copiado (strdup) em start(), liberado na task
    tcp_client_connected_cb_t on_client_connected;
    tcp_client_on_received_cb_t on_client_received;
} tcp_server_task_args_t;

static void tcp_server_task(void *pvParameters)
{
    tcp_server_task_args_t *args = (tcp_server_task_args_t *)pvParameters;
    uint16_t port = args->port;
    char *welcome_msg = args->welcome_msg; // ownership passa pra cá
    tcp_client_connected_cb_t on_client_connected = args->on_client_connected;
    tcp_client_on_received_cb_t on_client_received = args->on_client_received;
    vPortFree(args); // a struct em si pode ser liberada já; welcome_msg não

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

        // 1. Envia a mensagem de boas-vindas ao cliente que acabou de conectar
        if (welcome_msg != NULL) {
            int to_send = strlen(welcome_msg);
            int sent = send(sock, welcome_msg, to_send, 0);
            if (sent < 0) {
                ESP_LOGW(TAG, "Falha ao enviar welcome_msg: errno %d", errno);
            }
        }

        // 2. Notifica quem quiser saber que um cliente conectou (ex: publicar no MQTT)
        if (on_client_connected != NULL) {
            on_client_connected(addr_str);
        }

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
                on_client_received(len, rx_buffer);
                // TODO: processar dados binários recebidos
            }
        } while (len > 0);

        shutdown(sock, 0);
        close(sock);
    }

CLEAN_UP:
    if (welcome_msg != NULL) {
        free(welcome_msg);
    }
    close(listen_sock);
    vTaskDelete(NULL);
}

void tcp_server_app_start(uint16_t port,
                           const char *welcome_msg,
                           tcp_client_connected_cb_t on_client_connected,
                           tcp_client_on_received_cb_t on_client_received)
{
    tcp_server_task_args_t *args = pvPortMalloc(sizeof(tcp_server_task_args_t));
    args->port = port;
    args->welcome_msg = (welcome_msg != NULL) ? strdup(welcome_msg) : NULL;
    args->on_client_connected = on_client_connected;
    args->on_client_received = on_client_received;
    xTaskCreate(tcp_server_task, "tcp_server", 4096, args, 5, NULL);
}
