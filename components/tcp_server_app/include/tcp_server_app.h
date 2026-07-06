#pragma once

#include <stdint.h>

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
typedef void (*tcp_client_on_received_cb_t)(size_t len, const char *data);

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
 *
 * @param port Porta TCP a escutar (ex: CONFIG_TCP_SERVER_PORT).
 * @param welcome_msg Mensagem enviada ao cliente assim que ele conecta.
 *                    Pode ser NULL para não enviar nada.
 * @param on_client_connected Callback opcional (pode ser NULL) chamado a
 *                    cada nova conexão aceita.
 */
void tcp_server_app_start(uint16_t port,
                           const char *welcome_msg,
                           tcp_client_connected_cb_t on_client_connected,
                           tcp_client_on_received_cb_t on_client_received
                        );


#ifdef __cplusplus
}
#endif
