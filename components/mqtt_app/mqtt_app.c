#include "mqtt_app.h"

#include <stdbool.h>
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt_app";

// Guardamos o netif que era default antes de forçarmos o ETH1, para
// restaurar assim que a conexão MQTT for confirmada — evitar restaurar
// antes disso é o que corrige a race condition do código original,
// onde esp_mqtt_client_start() é assíncrono e o "restore" acontecia
// antes da resolução DNS / handshake TCP realmente ocorrerem.
static esp_netif_t *s_previous_default_netif = NULL;
static esp_mqtt_client_handle_t s_client = NULL;

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
        if (event->error_handle != NULL) {
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "  transporte/TLS: esp_tls_last_esp_err=0x%x, esp_tls_stack_err=0x%x, sock_errno=%d",
                         event->error_handle->esp_tls_last_esp_err,
                         event->error_handle->esp_tls_stack_err,
                         event->error_handle->esp_transport_sock_errno);
            } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                ESP_LOGE(TAG, "  broker recusou conexao, connect_return_code=%d",
                         event->error_handle->connect_return_code);
            }
        }
        break;
    default:
        break;
    }

    (void)event;
}

esp_mqtt_client_handle_t mqtt_app_start(const mqtt_app_config_t *config)
{
    if (config == NULL || config->eth_netif == NULL) {
        ESP_LOGE(TAG, "config ou eth_netif NULL");
        return NULL;
    }
    if (config->ca_cert_pem == NULL) {
        ESP_LOGE(TAG, "ca_cert_pem NULL — obrigatorio para mqtts:// (pinning da CA do broker)");
        return NULL;
    }

    bool mtls = (config->client_cert_pem != NULL && config->client_key_pem != NULL);
    ESP_LOGI(TAG, "Iniciando cliente MQTT com %s", mtls ? "mTLS (cert + key do device)" : "TLS unidirecional");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI, // espera-se "mqtts://host:8883"
        .broker.verification.certificate = config->ca_cert_pem,
        .credentials.authentication.certificate = config->client_cert_pem,
        .credentials.authentication.key = config->client_key_pem,
    };

    // Força temporariamente o netif default para garantir que a resolução
    // DNS/conexão inicial saia pela interface correta (ETH1). É restaurado
    // no MQTT_EVENT_CONNECTED acima, não logo após o start().
    s_previous_default_netif = esp_netif_get_default_netif();
    esp_netif_set_default_netif(config->eth_netif);

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init falhou");
        return NULL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));

    s_client = client;
    return client;
}

int mqtt_app_publish(const char *topic, const char *data, int qos, int retain)
{
    if (s_client == NULL) {
        ESP_LOGW(TAG, "mqtt_app_publish chamado antes de mqtt_app_start");
        return -1;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, data, 0, qos, retain);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Falha ao publicar em '%s' (cliente desconectado?)", topic);
    }
    return msg_id;
}
