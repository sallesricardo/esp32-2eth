#pragma once

#include <stddef.h>
#include "esp_netif.h"
#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback chamado quando uma mensagem chega num tópico assinado
 *        (ver `subscriptions` em mqtt_app_config_t).
 *
 * Roda na task interna do esp-mqtt (a mesma que despacha MQTT_EVENT_*) —
 * não é a task de nenhum dos outros componentes. Se o processamento for
 * pesado, copie os dados e delegue pra outra task/fila (o mesmo motivo
 * pelo qual o proxy TCP usa um event loop dedicado — ver proxy_events.h).
 *
 * `topic`/`data` são reagrupados internamente pelo mqtt_app a partir dos
 * fragmentos MQTT_EVENT_DATA (o esp-mqtt fragmenta mensagens maiores que o
 * buffer interno do client) — chegam completos aqui, mas SEM terminador
 * nulo garantido; use topic_len/data_len, não strlen().
 */
typedef void (*mqtt_app_data_cb_t)(const char *topic, size_t topic_len,
                                    const char *data, size_t data_len);

/**
 * @brief Um tópico a assinar automaticamente sempre que o cliente
 *        (re)conectar ao broker (reconexão não preserva subscriptions a
 *        menos que o broker use sessão persistente, então o mqtt_app
 *        reassina em todo MQTT_EVENT_CONNECTED).
 */
typedef struct {
    const char *topic;
    int qos;
} mqtt_app_subscription_t;

/**
 * @brief Configuração do cliente MQTT, incluindo mTLS.
 *
 * Os campos de certificado esperam PEM terminado em \0 (é exatamente o que
 * EMBED_TXTFILES gera — ver components/mqtt_app/CMakeLists.txt e os
 * símbolos extern usados em main/app_main.c).
 */
typedef struct {
    esp_netif_t *eth_netif;  // netif que deve ser usado para a conexão MQTT

    // Certificado da CA que assinou o certificado do broker. O ESP32 usa
    // isso pra validar que está falando com o broker de verdade (pinning
    // via CA própria, não via cadeia pública). Obrigatório para mqtts://.
    const char *ca_cert_pem;

    // Certificado + chave privada do próprio ESP32, assinados pela mesma
    // CA. Usados pro broker validar que é o ESP32 de verdade quem está
    // conectando (mTLS). Deixe ambos NULL para TLS unidirecional (sem
    // autenticação por certificado do lado do device).
    const char *client_cert_pem;
    const char *client_key_pem;

    // Tópicos a assinar automaticamente (pode ser NULL/0 se o device só
    // publica e nunca recebe nada). O array não é copiado -- precisa
    // permanecer válido pela vida do programa (o normal ao usar arrays
    // estáticos/globais em main/app_main.c).
    const mqtt_app_subscription_t *subscriptions;
    size_t subscriptions_count;

    // Chamado a cada mensagem recebida em qualquer tópico assinado. Pode
    // ser NULL se subscriptions_count == 0.
    mqtt_app_data_cb_t on_data;
} mqtt_app_config_t;

/**
 * @brief Inicia o cliente MQTT com TLS mútuo, amarrado a um netif específico.
 *
 * @param config Configuração (netif + certificados). Não é copiada; os
 *               ponteiros devem permanecer válidos pela vida do programa
 *               (o caso normal ao usar EMBED_TXTFILES, que gera símbolos
 *               estáticos).
 * @return Handle do cliente MQTT (para publish/subscribe posteriores), ou NULL em erro.
 */
esp_mqtt_client_handle_t mqtt_app_start(const mqtt_app_config_t *config);

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

