#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Registra o processamento dos payloads do radar no event loop
 *        dedicado do doppler_events, roteando por comando.
 *
 * Chame uma vez em main/app_main.c, depois de doppler_events_init() e antes
 * de tcp_client_app_start() (não é estritamente necessário nessa ordem,
 * mas evita perder o primeiro pacote se ele chegar rápido demais).
 *
 * Pra adicionar o processamento de um novo comando: adicione uma entrada
 * na tabela s_handlers em doppler_processing.c (comando + função), não
 * precisa mexer em app_main.c nem em doppler_events.
 */
esp_err_t doppler_processing_init(void);

#ifdef __cplusplus
}
#endif
