#include "tcp_client_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "tcp_client";

// ============================================================================
// DEFINIÇÕES E CONSTANTES
// ============================================================================
// Framing do protocolo do radar: header/footer + tamanho (u16 BE) + comando +
// número de frame + payload + checksum (soma dos bytes entre o tamanho e o
// payload, inclusive) + footer. Ver README (seção "Remote / Doppler framing")
// se/quando esse formato for documentado lá.
#define FRAME_HEADER     0xDB
#define FRAME_FOOTER     0xDC
#define MAX_PAYLOAD_SIZE 1024
#define RECONNECT_DELAY_MS 3000 // reconecta a cada 3s se a conexão cair (ver README)

// ============================================================================
// VARIÁVEIS GLOBAIS E ESTRUTURAS
// ============================================================================
static int s_sock = -1;                          // Descritor do Socket TCP (-1 = desconectado)
static SemaphoreHandle_t s_sock_mutex = NULL;     // Protege s_sock e o envio (Tx)
static uint8_t s_tx_frame_number = 0;             // Contador de frames enviados (tcp_client_app_send_command)

// Argumentos passados pra task de recepção (copiados de tcp_client_app_start)
typedef struct {
    char host[128];
    uint16_t port;
    esp_netif_t *bind_netif;
    tcp_client_data_cb_t on_data_received;
} tcp_client_task_args_t;

// Máquina de Estados do Parser
typedef enum {
    STATE_WAIT_HEADER,
    STATE_WAIT_LEN_H,
    STATE_WAIT_LEN_L,
    STATE_WAIT_CMD,
    STATE_WAIT_FRAME,
    STATE_WAIT_PAYLOAD,
    STATE_WAIT_CHECKSUM,
    STATE_WAIT_FOOTER
} parser_state_t;

// Estrutura de contexto do Parser
typedef struct {
    parser_state_t state;
    uint16_t expected_payload_len;
    uint16_t payload_index;
    uint8_t command;
    uint8_t frame_number;
    uint8_t calculated_checksum;
    uint8_t received_checksum;
    uint8_t payload[MAX_PAYLOAD_SIZE];
    int64_t start_timestamp_us; // Capturado no preâmbulo (byte de header)
} protocol_parser_t;

// ============================================================================
// MÁQUINA DE ESTADOS (PARSER)
// ============================================================================
static void reset_parser(protocol_parser_t *parser)
{
    parser->state = STATE_WAIT_HEADER;
    parser->expected_payload_len = 0;
    parser->payload_index = 0;
    parser->calculated_checksum = 0;
    // Não resetamos start_timestamp_us aqui: só é atualizado quando um novo
    // header chega (abaixo), pra não perder a captura entre um reset e o
    // próximo preâmbulo.
}

// Processa um byte recebido, avançando a máquina de estados. Quando um
// pacote completo e íntegro (checksum ok) é montado, chama `callback` com o
// timestamp capturado no preâmbulo (primeiro byte, o header).
static void process_byte(protocol_parser_t *parser, uint8_t b, tcp_client_data_cb_t callback)
{
    switch (parser->state) {
        case STATE_WAIT_HEADER:
            if (b == FRAME_HEADER) {
                reset_parser(parser);
                // Captura cirúrgica do tempo no primeiro byte recebido do
                // pacote -- é isso que mede a latência de ponta a ponta,
                // não o instante em que o pacote termina de ser montado.
                parser->start_timestamp_us = esp_timer_get_time();
                parser->state = STATE_WAIT_LEN_H;
            }
            break;

        case STATE_WAIT_LEN_H:
            parser->expected_payload_len = (uint16_t)(b << 8);
            parser->calculated_checksum += b;
            parser->state = STATE_WAIT_LEN_L;
            break;

        case STATE_WAIT_LEN_L:
            parser->expected_payload_len |= b;
            parser->calculated_checksum += b;
            if (parser->expected_payload_len > MAX_PAYLOAD_SIZE) {
                ESP_LOGW(TAG, "Payload excedeu tamanho maximo (%d > %d). Descartando pacote.",
                         parser->expected_payload_len, MAX_PAYLOAD_SIZE);
                reset_parser(parser);
            } else {
                parser->state = STATE_WAIT_CMD;
            }
            break;

        case STATE_WAIT_CMD:
            parser->command = b;
            parser->calculated_checksum += b;
            parser->state = STATE_WAIT_FRAME;
            break;

        case STATE_WAIT_FRAME:
            parser->frame_number = b;
            parser->calculated_checksum += b;
            parser->state = (parser->expected_payload_len > 0) ? STATE_WAIT_PAYLOAD : STATE_WAIT_CHECKSUM;
            break;

        case STATE_WAIT_PAYLOAD:
            parser->payload[parser->payload_index++] = b;
            parser->calculated_checksum += b;
            if (parser->payload_index >= parser->expected_payload_len) {
                parser->state = STATE_WAIT_CHECKSUM;
            }
            break;

        case STATE_WAIT_CHECKSUM:
            parser->received_checksum = b;
            parser->state = STATE_WAIT_FOOTER;
            break;

        case STATE_WAIT_FOOTER:
            if (b == FRAME_FOOTER) {
                if (parser->calculated_checksum == parser->received_checksum) {
                    if (callback != NULL) {
                        callback(parser->command, parser->frame_number,
                                  parser->payload, parser->expected_payload_len,
                                  parser->start_timestamp_us);
                    }
                } else {
                    ESP_LOGW(TAG, "Erro de Checksum: Calc=0x%02X, Recv=0x%02X",
                             parser->calculated_checksum, parser->received_checksum);
                }
            } else {
                ESP_LOGW(TAG, "Footer invalido: 0x%02X (esperado 0x%02X). Pacote descartado.",
                         b, FRAME_FOOTER);
            }
            reset_parser(parser);
            break;
    }
}

// ============================================================================
// TRANSMISSÃO (TX)
// ============================================================================
bool tcp_client_app_send(const uint8_t *data, size_t len)
{
    if (s_sock_mutex == NULL) {
        return false;
    }

    bool ok = false;
    xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
    if (s_sock >= 0) {
        int sent = send(s_sock, data, len, 0);
        if (sent < 0) {
            ESP_LOGW(TAG, "tcp_client_app_send falhou: errno %d", errno);
        } else {
            ok = true;
        }
    } else {
        ESP_LOGW(TAG, "tcp_client_app_send: nenhuma conexao ativa com o host remoto");
    }
    xSemaphoreGive(s_sock_mutex);
    return ok;
}

bool tcp_client_app_send_command(uint8_t command, const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len > MAX_PAYLOAD_SIZE) {
        ESP_LOGE(TAG, "tcp_client_app_send_command: payload de %d bytes excede o limite (%d)",
                 payload_len, MAX_PAYLOAD_SIZE);
        return false;
    }

    size_t total_len = 7 + payload_len; // header + len(2) + cmd + frame + checksum + footer
    uint8_t *buffer = malloc(total_len);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Falha de memoria ao montar comando 0x%02X", command);
        return false;
    }

    buffer[0] = FRAME_HEADER;
    buffer[1] = (uint8_t)((payload_len >> 8) & 0xFF);
    buffer[2] = (uint8_t)(payload_len & 0xFF);
    buffer[3] = command;
    buffer[4] = s_tx_frame_number++;

    if (payload_len > 0 && payload != NULL) {
        memcpy(&buffer[5], payload, payload_len);
    }

    uint8_t checksum = 0;
    for (size_t i = 1; i < (size_t)(5 + payload_len); i++) {
        checksum += buffer[i];
    }
    buffer[5 + payload_len] = checksum;
    buffer[6 + payload_len] = FRAME_FOOTER;

    bool ok = tcp_client_app_send(buffer, total_len);
    free(buffer);

    if (!ok) {
        ESP_LOGE(TAG, "Falha ao enviar comando 0x%02X", command);
    } else {
        ESP_LOGD(TAG, "Tx CMD: 0x%02X | Frame: %d", command, buffer[4]);
    }
    return ok;
}

// ============================================================================
// RECEPÇÃO E LÓGICA DE REDE (RX)
// ============================================================================
static void set_active_socket(int sock)
{
    xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
    s_sock = sock;
    xSemaphoreGive(s_sock_mutex);
}

// Resolve `host` (IP ou hostname, via getaddrinfo) e conecta um socket TCP
// nele em `port`. Se `bind_netif` não for NULL, faz bind() no IP dessa
// interface antes do connect() -- garante que a saída seja sempre por ela,
// independente do que a tabela de rotas do lwIP escolheria (ver README).
// Retorna o fd do socket conectado, ou -1 em erro (já logado).
static int connect_to_remote_host(const char *host, uint16_t port, esp_netif_t *bind_netif)
{
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int gai_err = getaddrinfo(host, port_str, &hints, &res);
    if (gai_err != 0 || res == NULL) {
        ESP_LOGE(TAG, "getaddrinfo(%s) falhou: %d", host, gai_err);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        freeaddrinfo(res);
        return -1;
    }

    if (bind_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(bind_netif, &ip_info) == ESP_OK) {
            struct sockaddr_in local_addr = {
                .sin_family = AF_INET,
                .sin_addr.s_addr = ip_info.ip.addr,
                .sin_port = 0, // porta local qualquer
            };
            if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) != 0) {
                ESP_LOGW(TAG, "Falha ao dar bind na interface %s (errno %d), "
                              "conexao pode sair por outra interface",
                         esp_netif_get_desc(bind_netif), errno);
            } else {
                ESP_LOGI(TAG, "Bind restrito a interface %s (" IPSTR ")",
                         esp_netif_get_desc(bind_netif), IP2STR(&ip_info.ip));
            }
        } else {
            ESP_LOGW(TAG, "Falha ao obter IP de bind_netif, saida decidida pela tabela de rotas");
        }
    }

    ESP_LOGI(TAG, "Conectando a %s:%u...", host, port);
    int conn_err = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (conn_err != 0) {
        ESP_LOGW(TAG, "Falha ao conectar em %s:%u: errno %d", host, port, errno);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "Conectado a %s:%u", host, port);
    return sock;
}

// Task principal: mantém a conexão com o host remoto, reconectando a cada
// RECONNECT_DELAY_MS se a conexão cair, e processa os bytes recebidos pela
// máquina de estados do parser.
static void tcp_client_task(void *pvParameters)
{
    tcp_client_task_args_t *args = (tcp_client_task_args_t *)pvParameters;

    uint8_t rx_buffer[128];
    protocol_parser_t parser;
    reset_parser(&parser);

    while (1) {
        int sock = connect_to_remote_host(args->host, args->port, args->bind_netif);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }
        set_active_socket(sock);

        while (1) {
            int len = recv(sock, rx_buffer, sizeof(rx_buffer), 0);
            if (len < 0) {
                ESP_LOGE(TAG, "Erro no recv(): errno %d", errno);
                break;
            }
            if (len == 0) {
                ESP_LOGW(TAG, "Conexao fechada pelo host remoto");
                break;
            }
            for (int i = 0; i < len; i++) {
                process_byte(&parser, rx_buffer[i], args->on_data_received);
            }
        }

        set_active_socket(-1);
        shutdown(sock, 0);
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
}

void tcp_client_app_start(const char *host,
                           uint16_t port,
                           esp_netif_t *bind_netif,
                           tcp_client_data_cb_t on_data_received)
{
    if (s_sock_mutex == NULL) {
        s_sock_mutex = xSemaphoreCreateMutex();
        if (s_sock_mutex == NULL) {
            ESP_LOGE(TAG, "Falha ao criar mutex");
            return;
        }
    }

    tcp_client_task_args_t *args = malloc(sizeof(tcp_client_task_args_t));
    if (args == NULL) {
        ESP_LOGE(TAG, "Falha de memoria ao iniciar tcp_client_app");
        return;
    }
    strlcpy(args->host, host, sizeof(args->host));
    args->port = port;
    args->bind_netif = bind_netif;
    args->on_data_received = on_data_received;

    xTaskCreate(tcp_client_task, "tcp_client", 4096, args, 5, NULL);
}
