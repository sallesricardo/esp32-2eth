#pragma once

#include "esp_netif.h"
#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicia o cliente MQTT amarrado a um netif específico.
 *
 * @param eth_netif Netif (ex: da porta ETH1) que deve ser usado para a conexão MQTT.
 * @return Handle do cliente MQTT (para publish/subscribe posteriores), ou NULL em erro.
 */
esp_mqtt_client_handle_t mqtt_app_start(esp_netif_t *eth_netif);

/**
 * @brief Publica uma mensagem no broker MQTT usando o client iniciado por mqtt_app_start.
 *
 * Não bloqueia esperando o broker confirmar (qos>0 é enfileirado internamente
 * pela lib MQTT). Se o cliente ainda não estiver conectado ao broker, a
 * publicação falha silenciosamente (retorna -1) — a lib não enfileira
 * publish antes da conexão ser estabelecida.
 *
 * @param topic Tópico MQTT
 * @param data Payload (string terminada em \0)
 * @param qos 0, 1 ou 2
 * @param retain 0 ou 1
 * @return message_id em caso de sucesso, ou -1 em caso de erro/desconectado.
 */
int mqtt_app_publish(const char *topic, const char *data, int qos, int retain);

#ifdef __cplusplus
}
#endif
