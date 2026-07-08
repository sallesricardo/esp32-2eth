#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback chamado sempre que um cliente TCP conecta, logo após o
 *        accept() (antes de começar a receber dados dele).
 *
 * @param client_ip IP do cliente que acabou de conectar (string, ex: "192.168.1.50")
 */
typedef void (*tcp_client_connected_cb_t)(const char *client_ip);

/**
 * @brief Callback chamado sempre que dados são recebidos do cliente TCP conectado.
 *
 * O buffer é binário (não é uma string terminada em \0) e só é válido durante
 * a chamada — copie o que precisar se for usar depois de retornar.
 *
 * @param data Ponteiro pros bytes recebidos.
 * @param len Quantidade de bytes recebidos.
 */
typedef void (*tcp_server_data_cb_t)(const uint8_t *data, size_t len);

/**
 * @brief Cria a task do servidor TCP na porta informada.
 *
 * Aceita uma conexão por vez (backlog=1). Para múltiplos clientes
 * simultâneos, essa é a função a evoluir para usar select()/poll()
 * ou uma task por conexão.
 *
 * Ao aceitar uma conexão, o servidor:
 *   1. Envia uma mensagem de boas-vindas ao cliente (welcome_msg).
 *   2. Chama on_client_connected (se não for NULL) — útil para, por
 *      exemplo, publicar uma notificação no MQTT sem o tcp_server_app
 *      precisar conhecer nada sobre MQTT.
 *   3. A cada dado recebido, chama on_data_received (se não for NULL) —
 *      útil para encaminhar os dados a outro lugar (ex: um proxy TCP)
 *      sem o tcp_server_app precisar conhecer o destino.
 *
 * @param port Porta TCP a escutar (ex: CONFIG_TCP_SERVER_PORT).
 * @param bind_netif Se não for NULL, o socket de escuta faz bind() no IP
 *                    dessa interface específica — só aceita conexões que
 *                    chegarem fisicamente por ela, ignorando as demais.
 *                    Se for NULL, faz bind em INADDR_ANY (aceita por
 *                    qualquer interface, comportamento antigo).
 * @param welcome_msg Mensagem enviada ao cliente assim que ele conecta.
 *                    Pode ser NULL para não enviar nada.
 * @param on_client_connected Callback opcional (pode ser NULL) chamado a
 *                    cada nova conexão aceita.
 * @param on_data_received Callback opcional (pode ser NULL) chamado a cada
 *                    dado recebido do cliente.
 */
void tcp_server_app_start(uint16_t port,
                           esp_netif_t *bind_netif,
                           const char *welcome_msg,
                           tcp_client_connected_cb_t on_client_connected,
                           tcp_server_data_cb_t on_data_received);

/**
 * @brief Envia dados para o cliente atualmente conectado (se houver).
 *
 * Thread-safe: pode ser chamada de outra task (ex: da task do tcp_client_app,
 * para fazer o proxy no sentido remoto -> cliente TCP local).
 *
 * @param data Bytes a enviar.
 * @param len Quantidade de bytes.
 * @return true se enviado com sucesso, false se não há cliente conectado
 *         ou se ocorreu erro no envio.
 */
bool tcp_server_app_send(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

