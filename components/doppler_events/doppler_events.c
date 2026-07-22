#include "doppler_events.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "doppler_events";

ESP_EVENT_DEFINE_BASE(DOPPLER_EVENT);

#define DOPPLER_EVENT_QUEUE_SIZE      16
#define DOPPLER_EVENT_TASK_STACK      4096
#define DOPPLER_EVENT_TASK_PRIO       5
#define DOPPLER_EVENT_POST_TIMEOUT_MS 100

static esp_event_loop_handle_t s_loop = NULL;

esp_err_t doppler_events_init(void)
{
    esp_event_loop_args_t loop_args = {
        .queue_size = DOPPLER_EVENT_QUEUE_SIZE,
        .task_name = "doppler_evt_loop",
        .task_priority = DOPPLER_EVENT_TASK_PRIO,
        .task_stack_size = DOPPLER_EVENT_TASK_STACK,
        .task_core_id = tskNO_AFFINITY,
    };
    esp_err_t err = esp_event_loop_create(&loop_args, &s_loop);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create falhou: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t doppler_events_register_handler(int32_t command,
                                           esp_event_handler_t handler,
                                           void *handler_arg)
{
    if (s_loop == NULL) {
        ESP_LOGE(TAG, "doppler_events_register_handler chamado antes de doppler_events_init");
        return ESP_ERR_INVALID_STATE;
    }
    return esp_event_handler_register_with(s_loop, DOPPLER_EVENT, command, handler, handler_arg);
}

void doppler_events_post_data(uint8_t command,
                               const void *data,
                               size_t len,
                               int64_t timestamp_us)
{
    if (s_loop == NULL) {
        ESP_LOGE(TAG, "doppler_events_post_data chamado antes de doppler_events_init");
        return;
    }
    if (len == 0) {
        return;
    }

    uint8_t *copy = malloc(len);
    if (copy == NULL) {
        ESP_LOGE(TAG, "Sem memoria pra copiar %d bytes, descartando", (int)len);
        return;
    }
    memcpy(copy, data, len);

    // timestamp_us vem do preâmbulo (capturado no tcp_client_app), não é
    // recalculado aqui -- ver doc de doppler_events_post_data no .h.
    doppler_data_event_t evt = {
        .data = copy,
        .len = len,
        .timestamp_us = timestamp_us,
    };

    esp_err_t err = esp_event_post_to(
        s_loop,
        DOPPLER_EVENT,
        (int32_t)command,
        &evt,
        sizeof(evt),
        pdMS_TO_TICKS(DOPPLER_EVENT_POST_TIMEOUT_MS)
    );
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao postar comando 0x%02X (%s), descartando %d bytes",
                 command, esp_err_to_name(err), (int)len);
        free(copy);
    }
}
