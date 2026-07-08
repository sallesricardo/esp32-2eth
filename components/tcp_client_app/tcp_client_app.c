#include "tcp_client_app.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "tcp_client";

#define RECONNECT_DELAY_MS 3000

typedef struct {
    char *host;   // strdup'ed em start()
    uint16_t port;
    esp_netif_t *bind_netif; // NULL = deixa a tabela de rotas escolher a interface
    tcp_client_data_cb_t on_data_received;
} tcp_client_task_args_t;

// Socket da conexão atual com o host remoto (-1 = nenhuma conexão ativa).
// Protegido por mutex porque tcp_client_app_send() pode ser chamada de
// outra task (ex: a task do tcp_server_app fazendo o proxy local -> remoto).
static int s_remote_sock = -1;
static SemaphoreHandle_t s_sock_mutex = NULL;

static void set_active_socket(int sock)
{
    xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
    s_remote_sock = sock;
    xSemaphoreGive(s_sock_mutex);
}

bool tcp_client_app_send(const uint8_t *data, size_t len)
{
    bool ok = false;
    xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
    if (s_remote_sock >= 0) {
        int sent = send(s_remote_sock, data, len, 0);
        if (sent < 0) {
            ESP_LOGW(TAG, "tcp_client_app_send falhou: errno %d", errno);
        } else {
            ok = true;
        }
    } else {
        ESP_LOGW(TAG, "tcp_client_app_send: sem conexão ativa com o host remoto");
    }
    xSemaphoreGive(s_sock_mutex);
    return ok;
}

// Tenta conectar uma vez. Retorna o socket conectado, ou -1 em caso de falha
// (já fechando o socket criado, se houver).
static int try_connect(const char *host, uint16_t port, esp_netif_t *bind_netif)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", port);

    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGW(TAG, "getaddrinfo('%s') falhou: %d", host, err);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        freeaddrinfo(res);
        return -1;
    }

    // Se bind_netif for informado, fixa o IP de origem ANTES do connect(),
    // forçando a saída por essa interface independente do que a tabela de
    // rotas escolheria para o IP de destino.
    if (bind_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(bind_netif, &ip_info) == ESP_OK) {
            struct sockaddr_in local_addr = {
                .sin_family = AF_INET,
                .sin_addr.s_addr = ip_info.ip.addr,
                .sin_port = 0, // porta efêmera, o SO escolhe
            };
            if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) != 0) {
                ESP_LOGW(TAG, "Falha ao fixar interface de saída (bind): errno %d", errno);
                close(sock);
                freeaddrinfo(res);
                return -1;
            }
            ESP_LOGI(TAG, "Saida fixada na interface %s (" IPSTR ")",
                     esp_netif_get_desc(bind_netif), IP2STR(&ip_info.ip));
        } else {
            ESP_LOGW(TAG, "Falha ao obter IP de bind_netif, deixando roteamento decidir");
        }
    }

    ESP_LOGI(TAG, "Conectando a %s:%u...", host, port);
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "Falha ao conectar a %s:%u: errno %d", host, port, errno);
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    ESP_LOGI(TAG, "Conectado a %s:%u", host, port);
    return sock;
}

static void tcp_client_task(void *pvParameters)
{
    tcp_client_task_args_t *args = (tcp_client_task_args_t *)pvParameters;
    char *host = args->host; // ownership passa pra cá
    uint16_t port = args->port;
    esp_netif_t *bind_netif = args->bind_netif;
    tcp_client_data_cb_t on_data_received = args->on_data_received;
    vPortFree(args);

    uint8_t rx_buffer[128];

    while (1) {
        int sock = try_connect(host, port, bind_netif);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        set_active_socket(sock);

        int len;
        do {
            len = recv(sock, rx_buffer, sizeof(rx_buffer), 0);
            if (len < 0) {
                ESP_LOGE(TAG, "Error during receiving: errno %d", errno);
            } else if (len == 0) {
                ESP_LOGW(TAG, "Host remoto fechou a conexão");
            } else {
                ESP_LOGI(TAG, "Received %d bytes from remote host", len);
                if (on_data_received != NULL) {
                    on_data_received(rx_buffer, (size_t)len);
                }
            }
        } while (len > 0);

        set_active_socket(-1);
        close(sock);

        ESP_LOGW(TAG, "Desconectado do host remoto, tentando reconectar em %d ms...",
                 RECONNECT_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
}

void tcp_client_app_start(const char *host, uint16_t port, esp_netif_t *bind_netif,
                           tcp_client_data_cb_t on_data_received)
{
    if (s_sock_mutex == NULL) {
        s_sock_mutex = xSemaphoreCreateMutex();
    }

    tcp_client_task_args_t *args = pvPortMalloc(sizeof(tcp_client_task_args_t));
    args->host = strdup(host);
    args->port = port;
    args->bind_netif = bind_netif;
    args->on_data_received = on_data_received;
    xTaskCreate(tcp_client_task, "tcp_client", 4096, args, 5, NULL);
}
