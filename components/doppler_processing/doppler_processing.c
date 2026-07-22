#include "doppler_processing.h"

#include <stddef.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "doppler_events.h"

static const char *TAG = "doppler_proc";

// ============================================================================
// Comandos do protocolo do radar
// ============================================================================
// TODO: substituir pelos comandos reais assim que o protocolo do radar
// estiver documentado (ver README / SKILL.md -- "Remote / Doppler framing").
// Os dois abaixo são só exemplo de como plugar um comando novo.
#define DOPPLER_CMD_EXEMPLO_A 0x01
#define DOPPLER_CMD_EXEMPLO_B 0x02

// Assinatura de um handler de comando: recebe o payload já decodificado
// (sem header/comando/frame/checksum/footer -- isso já foi resolvido pelo
// parser do tcp_client_app) e a latência de ponta a ponta (preâmbulo do
// pacote -> aqui). NÃO deve dar free() em `payload`: quem libera é o
// dispatcher (doppler_frame_dispatcher), depois que o handler retorna.
typedef void (*doppler_cmd_handler_t)(const uint8_t *payload, size_t len, int64_t latency_us);

typedef struct {
    uint8_t command;
    const char *name;
    doppler_cmd_handler_t handler;
} doppler_cmd_entry_t;

// ----------------------------------------------------------------------------
// Handlers por comando -- adicione um novo aqui (função + entrada na
// tabela s_handlers abaixo) pra cada comando real do protocolo.
// ----------------------------------------------------------------------------

static void handle_cmd_exemplo_a(const uint8_t *payload, size_t len, int64_t latency_us)
{
    // TODO: decodificar o payload de verdade (campos do protocolo do
    // radar) e fazer o que for necessário com ele (publicar MQTT, atualizar
    // estado local, etc). Por enquanto só loga, como placeholder.
    ESP_LOGI(TAG, "CMD_EXEMPLO_A: %d bytes, latencia preambulo->processamento: %lld us",
             (int)len, (long long)latency_us);
}

static void handle_cmd_exemplo_b(const uint8_t *payload, size_t len, int64_t latency_us)
{
    ESP_LOGI(TAG, "CMD_EXEMPLO_B: %d bytes, latencia preambulo->processamento: %lld us",
             (int)len, (long long)latency_us);
}

static const doppler_cmd_entry_t s_handlers[] = {
    { DOPPLER_CMD_EXEMPLO_A, "CMD_EXEMPLO_A", handle_cmd_exemplo_a },
    { DOPPLER_CMD_EXEMPLO_B, "CMD_EXEMPLO_B", handle_cmd_exemplo_b },
};
#define NUM_HANDLERS (sizeof(s_handlers) / sizeof(s_handlers[0]))

// ----------------------------------------------------------------------------
// Dispatcher único, registrado com ESP_EVENT_ANY_ID em doppler_events --
// roda pra QUALQUER comando, procura na tabela acima e delega. Dono de
// evt->data: libera aqui, depois que o handler (se houver) processou.
// ----------------------------------------------------------------------------
static void doppler_frame_dispatcher(void *handler_arg, esp_event_base_t base,
                                      int32_t event_id, void *event_data)
{
    (void)handler_arg;
    (void)base;
    doppler_data_event_t *evt = (doppler_data_event_t *)event_data;
    uint8_t command = (uint8_t)event_id;

    // Latência de ponta a ponta: evt->timestamp_us é o instante em que o
    // PREÂMBULO do pacote chegou (capturado no parser do tcp_client_app),
    // não o instante em que entrou nesta fila.
    int64_t latency_us = esp_timer_get_time() - evt->timestamp_us;

    const doppler_cmd_entry_t *entry = NULL;
    for (size_t i = 0; i < NUM_HANDLERS; i++) {
        if (s_handlers[i].command == command) {
            entry = &s_handlers[i];
            break;
        }
    }

    if (entry != NULL) {
        entry->handler(evt->data, evt->len, latency_us);
    } else {
        ESP_LOGW(TAG, "Comando 0x%02X sem handler registrado (%d bytes descartados)",
                 command, (int)evt->len);
    }

    free(evt->data);
}

esp_err_t doppler_processing_init(void)
{
    return doppler_events_register_handler(ESP_EVENT_ANY_ID, doppler_frame_dispatcher, NULL);
}
