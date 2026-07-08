#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback chamado sempre que dados são recebidos do host remoto.
 *
 * O buffer é binário (não é uma string terminada em \0) e só é válido durante
 * a chamada — copie o que precisar se for usar depois de retornar.
 *
 * @param data Ponteiro pros bytes recebidos.
 * @param len Quantidade de bytes recebidos.
 */
typedef void (*tcp_client_data_cb_t)(const uint8_t *data, size_t len);

/**
 * @brief Cria a task que conecta (como cliente TCP) a um host remoto e
 *        mantém a conexão, reconectando automaticamente se cair.
 *
 * A cada dado recebido do host remoto, chama on_data_received (se não for
 * NULL) — útil para encaminhar os dados a outro lugar (ex: o cliente TCP
 * conectado no tcp_server_app, fechando o proxy) sem esse componente
 * precisar conhecer o destino.
 *
 * @param host Hostname ou IP do host remoto (ex: "192.168.1.100").
 * @param port Porta TCP do host remoto (ex: 50000).
 * @param bind_netif Se não for NULL, o socket faz bind() no IP dessa
 *                    interface antes do connect() — força a saída por ela
 *                    independente do que a tabela de rotas escolheria.
 *                    Se for NULL, deixa o roteamento decidir (comportamento
 *                    antigo).
 * @param on_data_received Callback opcional (pode ser NULL) chamado a cada
 *                    dado recebido do host remoto.
 */
void tcp_client_app_start(const char *host,
                           uint16_t port,
                           esp_netif_t *bind_netif,
                           tcp_client_data_cb_t on_data_received);

/**
 * @brief Envia dados para o host remoto, se a conexão estiver ativa.
 *
 * Thread-safe: pode ser chamada de outra task (ex: da task do
 * tcp_server_app, para fazer o proxy no sentido cliente local -> remoto).
 *
 * @param data Bytes a enviar.
 * @param len Quantidade de bytes.
 * @return true se enviado com sucesso, false se não há conexão ativa
 *         ou se ocorreu erro no envio.
 */
bool tcp_client_app_send(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
