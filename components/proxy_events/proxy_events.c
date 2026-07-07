#include "proxy_events.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "proxy_events";

ESP_EVENT_DEFINE_BASE(PROXY_EVENT);

#define PROXY_EVENT_QUEUE_SIZE      16
#define PROXY_EVENT_TASK_STACK      4096
#define PROXY_EVENT_TASK_PRIO       5
#define PROXY_EVENT_POST_TIMEOUT_MS 100

static esp_event_loop_handle_t s_loop = NULL;

esp_err_t proxy_events_init(void)
{
    esp_event_loop_args_t loop_args = {
        .queue_size = PROXY_EVENT_QUEUE_SIZE,
        .task_name = "proxy_evt_loop",
        .task_priority = PROXY_EVENT_TASK_PRIO,
        .task_stack_size = PROXY_EVENT_TASK_STACK,
        .task_core_id = tskNO_AFFINITY,
    };
    esp_err_t err = esp_event_loop_create(&loop_args, &s_loop);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create falhou: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t proxy_events_register_handler(int32_t event_id,
                                         esp_event_handler_t handler,
                                         void *handler_arg)
{
    if (s_loop == NULL) {
        ESP_LOGE(TAG, "proxy_events_register_handler chamado antes de proxy_events_init");
        return ESP_ERR_INVALID_STATE;
    }
    return esp_event_handler_register_with(s_loop, PROXY_EVENT, event_id, handler, handler_arg);
}

void proxy_events_post_data(int32_t event_id, const uint8_t *data, size_t len)
{
    if (s_loop == NULL) {
        ESP_LOGE(TAG, "proxy_events_post_data chamado antes de proxy_events_init");
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

    // Timestamp tirado AGORA, o mais próximo possível do recebimento real
    // na rede (quem chama essa função deve fazê-lo logo após o recv()).
    proxy_data_event_t evt = {
        .data = copy,
        .len = len,
        .timestamp_us = esp_timer_get_time(),
    };

    esp_err_t err = esp_event_post_to(s_loop, PROXY_EVENT, event_id, &evt, sizeof(evt),
                                       pdMS_TO_TICKS(PROXY_EVENT_POST_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao postar evento %d (%s), descartando %d bytes",
                 (int)event_id, esp_err_to_name(err), (int)len);
        free(copy);
    }
}
