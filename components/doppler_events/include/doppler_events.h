#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(DOPPLER_EVENT);

/**
 * @brief Payload postado nos eventos do doppler_events.
 *
 * `data` é um buffer alocado no heap (tamanho `len`) — o handler que
 * processar o evento é dono dele e deve dar free() depois de usar.
 *
 * `timestamp_us` NÃO é capturado por este componente: é o instante
 * (esp_timer_get_time(), microssegundos desde o boot) em que o PREÂMBULO
 * do pacote foi recebido, capturado pelo parser do tcp_client_app assim
 * que o primeiro byte do frame (header 0xDB) chega — repassado até aqui
 * via doppler_events_post_data() sem ser recalculado. Isso mede a latência
 * de ponta a ponta (início da recepção do pacote -> handler pegou o dado
 * da fila), não só o tempo de fila. Subtraia de esp_timer_get_time() dentro
 * do handler pra obter essa latência.
 */
typedef struct {
    uint8_t *data;
    size_t len;
    int64_t timestamp_us;
} doppler_data_event_t;

/**
 * @brief Cria o event loop dedicado do doppler (task própria, fila própria —
 *        NÃO é o loop default usado por ETH_EVENT/IP_EVENT).
 *
 * Chame uma vez, antes de registrar handlers ou postar eventos.
 */
esp_err_t doppler_events_init(void);

/**
 * @brief Registra um handler para um comando específico do radar.
 *
 * O `command` é o próprio byte de comando do protocolo do radar (ver
 * `components/tcp_client_app`), usado diretamente como event_id do
 * ESP-IDF — ou seja, cada comando tem seu próprio handler, sem precisar
 * inspecionar o payload pra descobrir o que fazer com ele. Use
 * `ESP_EVENT_ANY_ID` pra registrar um handler "pega-tudo" (útil enquanto
 * os comandos específicos ainda não estão todos mapeados).
 *
 * O handler roda na task do event loop dedicado do doppler, não na task de
 * quem chamou doppler_events_post_data (a task de I/O do tcp_client_app).
 */
esp_err_t doppler_events_register_handler(int32_t command,
                                           esp_event_handler_t handler,
                                           void *handler_arg);

/**
 * @brief Copia `len` bytes de `data` pro heap e posta um DOPPLER_EVENT no
 *        loop dedicado do doppler, usando `command` como event_id.
 *
 * Pensada pra ser chamada de dentro do callback on_data_received do
 * tcp_client_app, já com o pacote decodificado (comando + payload) — só
 * copia e enfileira, não faz processamento pesado, então não trava a
 * leitura do socket.
 *
 * @param command Comando do protocolo do radar (vira o event_id postado —
 *                registre handlers por comando com doppler_events_register_handler).
 * @param data Payload do pacote (só o payload, sem header/footer/checksum).
 * @param len Tamanho de `data` em bytes.
 * @param timestamp_us Instante em que o PREÂMBULO do pacote foi recebido
 *                (capturado pelo parser do tcp_client_app, não aqui) —
 *                repassado como está pro doppler_data_event_t, pra medir
 *                a latência de ponta a ponta do pipeline.
 *
 * Se a fila do loop estiver cheia, espera até um timeout curto e desiste,
 * descartando o dado (loga um warning). Isso evita bloquear a task de
 * rede indefinidamente se o processamento estiver muito atrasado.
 */
void doppler_events_post_data(uint8_t command,
                               const void *data,
                               size_t len,
                               int64_t timestamp_us);

#ifdef __cplusplus
}
#endif
