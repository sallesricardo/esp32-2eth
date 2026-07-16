#include "mqtt_app.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
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

static const mqtt_app_subscription_t *s_subscriptions = NULL;
static size_t s_subscriptions_count = 0;
static mqtt_app_data_cb_t s_on_data = NULL;

// Tamanho máximo aceito pra uma mensagem recebida (reagrupada). É uma
// salvaguarda -- o total_data_len de um MQTT_EVENT_DATA vem do pacote
// PUBLISH declarado pelo broker; sem esse teto, uma mensagem
// anormalmente grande (erro de config do lado de quem publica, por
// exemplo) causaria um malloc gigante aqui. Ajuste se as configs que
// você for receber legitimamente puderem passar disso.
#define MQTT_APP_MAX_MESSAGE_LEN (8 * 1024)

// Buffer de reassembly pro payload que estiver chegando em partes (ver
// current_data_offset/total_data_len no esp-mqtt) -- só é preciso pra
// mensagens maiores que o buffer interno do client (buffer_size/
// out_buffer_size, default ~1KB). O esp-mqtt entrega todos os eventos
// DATA de uma mensagem antes de começar os de outra, então um único
// buffer estático aqui é suficiente (não multiplexa tópicos concorrentes
// entre si).
static char *s_rx_buf = NULL;
static size_t s_rx_len = 0;
static char s_rx_topic[128];
static size_t s_rx_topic_len = 0;

static void mqtt_app_resubscribe_all(void)
{
    for (size_t i = 0; i < s_subscriptions_count; i++) {
        int msg_id = esp_mqtt_client_subscribe(s_client, s_subscriptions[i].topic, s_subscriptions[i].qos);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "Falha ao assinar '%s'", s_subscriptions[i].topic);
        } else {
            ESP_LOGI(TAG, "Assinando '%s' (qos=%d)", s_subscriptions[i].topic, s_subscriptions[i].qos);
        }
    }
}

static void mqtt_app_handle_data_event(esp_mqtt_event_handle_t event)
{
    if (event->current_data_offset == 0) {
        // Primeiro fragmento desta mensagem -- (re)aloca o buffer do
        // tamanho total já avisado pelo esp-mqtt.
        free(s_rx_buf);
        s_rx_buf = NULL;
        s_rx_len = 0;

        if (event->total_data_len > MQTT_APP_MAX_MESSAGE_LEN) {
            ESP_LOGE(TAG, "Mensagem de %d bytes excede o limite de %d, descartando",
                     event->total_data_len, MQTT_APP_MAX_MESSAGE_LEN);
            return;
        }
        s_rx_buf = malloc(event->total_data_len);
        if (s_rx_buf == NULL) {
            ESP_LOGE(TAG, "Sem memoria pra reassembly de %d bytes, descartando mensagem",
                     event->total_data_len);
            return;
        }

        size_t tlen = (size_t)event->topic_len < sizeof(s_rx_topic) - 1
                          ? (size_t)event->topic_len
                          : sizeof(s_rx_topic) - 1;
        memcpy(s_rx_topic, event->topic, tlen);
        s_rx_topic[tlen] = '\0';
        s_rx_topic_len = tlen;
    }

    if (s_rx_buf == NULL) {
        return; // mensagem descartada no primeiro fragmento (erro/tamanho), ignora o resto
    }

    memcpy(s_rx_buf + event->current_data_offset, event->data, event->data_len);
    s_rx_len += event->data_len;

    if (s_rx_len < (size_t)event->total_data_len) {
        return; // ainda faltam fragmentos
    }

    ESP_LOGI(TAG, "Mensagem completa em '%.*s': %d bytes", (int)s_rx_topic_len, s_rx_topic, (int)s_rx_len);
    if (s_on_data != NULL) {
        s_on_data(s_rx_topic, s_rx_topic_len, s_rx_buf, s_rx_len);
    }

    free(s_rx_buf);
    s_rx_buf = NULL;
    s_rx_len = 0;
}

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
        // Reconexão não preserva subscriptions a menos que o broker use
        // sessão persistente -- reassina sempre que (re)conecta.
        mqtt_app_resubscribe_all();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_DATA:
        mqtt_app_handle_data_event(event);
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

    s_subscriptions = config->subscriptions;
    s_subscriptions_count = config->subscriptions_count;
    s_on_data = config->on_data;

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

    // Setado ANTES de start(): o handler de MQTT_EVENT_CONNECTED (que
    // chama mqtt_app_resubscribe_all, usando s_client) roda numa task
    // assíncrona da lib MQTT e pode disparar antes de qualquer código
    // depois de start() rodar na nossa própria task.
    s_client = client;

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));

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
