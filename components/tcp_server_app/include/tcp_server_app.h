#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Cria a task do servidor TCP na porta informada.
 *
 * Aceita uma conexão por vez (backlog=1). Para múltiplos clientes
 * simultâneos, essa é a função a evoluir para usar select()/poll()
 * ou uma task por conexão.
 *
 * @param port Porta TCP a escutar (ex: CONFIG_TCP_SERVER_PORT).
 */
void tcp_server_app_start(uint16_t port);

#ifdef __cplusplus
}
#endif
