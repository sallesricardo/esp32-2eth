#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(PROXY_EVENT);

enum {
    PROXY_EVENT_DATA_FROM_TCP_CLIENT,  // dado recebido do cliente TCP local (tcp_server_app)
    PROXY_EVENT_DATA_FROM_REMOTE_HOST, // dado recebido do host remoto (tcp_client_app)
};

/**
 * @brief Payload postado nos eventos PROXY_EVENT_DATA_*.
 *
 * `data` é um buffer alocado no heap (tamanho `len`) — o handler que
 * processar o evento é dono dele e deve dar free() depois de usar.
 *
 * `timestamp_us` é o instante (esp_timer_get_time(), microssegundos desde o
 * boot) em que o dado foi recebido na rede, capturado ANTES de entrar na
 * fila do event loop. Subtraia de esp_timer_get_time() dentro do handler
 * pra medir a latência entre recebimento e processamento.
 */
typedef struct {
    uint8_t *data;
    size_t len;
    int64_t timestamp_us;
} proxy_data_event_t;

/**
 * @brief Cria o event loop dedicado do proxy (task própria, fila própria —
 *        NÃO é o loop default usado por ETH_EVENT/IP_EVENT).
 *
 * Chame uma vez, antes de registrar handlers ou postar eventos.
 */
esp_err_t proxy_events_init(void);

/**
 * @brief Registra um handler para um dos PROXY_EVENT_DATA_* acima.
 *
 * O handler roda na task do event loop dedicado do proxy, não na task de
 * quem chamou proxy_events_post_data.
 */
esp_err_t proxy_events_register_handler(int32_t event_id,
                                         esp_event_handler_t handler,
                                         void *handler_arg);

/**
 * @brief Copia `len` bytes de `data` pro heap, tira o timestamp atual e
 *        posta um PROXY_EVENT no loop dedicado do proxy.
 *
 * Pensada pra ser chamada de dentro de uma task de I/O de rede (ex: dentro
 * do callback on_data_received do tcp_server_app/tcp_client_app) — só
 * copia e enfileira, não faz processamento pesado, então não trava a
 * leitura do socket.
 *
 * Se a fila do loop estiver cheia, espera até um timeout curto e desiste,
 * descartando o dado (loga um warning). Isso evita bloquear a task de
 * rede indefinidamente se o processamento estiver muito atrasado.
 */
void proxy_events_post_data(int32_t event_id, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
