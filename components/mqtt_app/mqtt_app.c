#include "mqtt_app.h"

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt_app";

// Guardamos o netif que era default antes de forçarmos o ETH1, para
// restaurar assim que a conexão MQTT for confirmada — evitar restaurar
// antes disso é o que corrige a race condition do código original,
// onde esp_mqtt_client_start() é assíncrono e o "restore" acontecia
// antes da resolução DNS / handshake TCP realmente ocorrerem.
static esp_netif_t *s_previous_default_netif = NULL;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        if (s_previous_default_netif) {
            esp_netif_set_default_netif(s_previous_default_netif);
            s_previous_default_netif = NULL;
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        break;
    }

    (void)event;
}

esp_mqtt_client_handle_t mqtt_app_start(esp_netif_t *eth_netif)
{
    if (eth_netif == NULL) {
        ESP_LOGE(TAG, "eth_netif NULL");
        return NULL;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
    };

    // Força temporariamente o netif default para garantir que a resolução
    // DNS/conexão inicial saia pela interface correta (ETH1). É restaurado
    // no MQTT_EVENT_CONNECTED acima, não logo após o start().
    s_previous_default_netif = esp_netif_get_default_netif();
    esp_netif_set_default_netif(eth_netif);

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init falhou");
        return NULL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));

    return client;
}
