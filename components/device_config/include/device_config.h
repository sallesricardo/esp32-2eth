#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Namespace NVS usado por este componente (útil se for inspecionar via
// `idf.py nvs-partition-tool` ou similar).
#define DEVICE_CONFIG_NVS_NAMESPACE "dev_cfg"

// Teto de tamanho aceito pro blob de config salvo no NVS. Uma entrada NVS
// tradicional cabe numa página de ~4KB; esse limite é uma salvaguarda
// simples enquanto o schema real da config não está definido -- ajuste se
// precisar de mais.
#define DEVICE_CONFIG_MAX_LEN 4096

/**
 * @brief Abre o namespace NVS usado por este componente.
 *
 * Chame uma vez, no boot, DEPOIS de nvs_flash_init() (que é
 * responsabilidade de quem chama -- normalmente main/app_main.c, já que
 * outros componentes também podem precisar do NVS) e ANTES de qualquer
 * outra função deste header.
 */
esp_err_t device_config_init(void);

/**
 * @brief Recupera o último config bruto salvo no NVS -- útil pra aplicar
 *        no boot, antes de qualquer mensagem MQTT nova chegar (o device
 *        não fica "sem config" só porque ainda não reconectou).
 *
 * @param out_buf   Buffer de saída, alocado por quem chama.
 * @param buf_size  Tamanho de out_buf.
 * @param out_len   [out] Tamanho real do config salvo. Se for maior que
 *                  buf_size, o conteúdo em out_buf foi truncado -- confira
 *                  antes de usar.
 * @return ESP_OK se havia algo salvo, ESP_ERR_NVS_NOT_FOUND se ainda não
 *         foi recebido nenhum config (primeiro boot), outro esp_err_t em
 *         erro real de NVS.
 */
esp_err_t device_config_load_raw(char *out_buf, size_t buf_size, size_t *out_len);

/**
 * @brief Compara o hash do payload novo com o hash do último config salvo
 *        no NVS. Se forem iguais, NÃO escreve nada na flash -- evita
 *        desgaste por reenvio (mensagem retida do MQTT, reconexão, o
 *        publicador repetindo a mesma config). Se forem diferentes, salva
 *        o payload bruto + o hash novo.
 *
 * O hash usado é CRC32 (via esp_crc32_le, função de ROM -- rápida, sem
 * heap extra). Não é criptográfico de propósito: aqui ele só detecta
 * mudança de conteúdo, não garante integridade/autenticidade -- isso já
 * vem do canal mTLS por onde a config chega (ver mqtt_app).
 *
 * @param payload     Config bruto recebido (ex: o JSON vindo do MQTT).
 * @param len         Tamanho de payload em bytes.
 * @param out_changed [out] true se o config era diferente do salvo (e foi
 *                    gravado agora); false se era igual (nada foi
 *                    escrito). Pode ser NULL se não precisar saber.
 * @return ESP_OK em sucesso (mudou ou não). ESP_ERR_INVALID_ARG se
 *         payload/len forem inválidos ou len exceder
 *         DEVICE_CONFIG_MAX_LEN. Outro esp_err_t em erro real de NVS.
 */
esp_err_t device_config_apply_if_changed(const char *payload, size_t len, bool *out_changed);

#ifdef __cplusplus
}
#endif
