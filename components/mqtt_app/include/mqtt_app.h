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

#ifdef __cplusplus
}
#endif
