#include "device_config.h"

#include <inttypes.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "device_config";

#define NVS_KEY_HASH "cfg_hash"
#define NVS_KEY_RAW  "cfg_raw"

static nvs_handle_t s_nvs_handle;
static bool s_initialized = false;

esp_err_t device_config_init(void)
{
    esp_err_t err = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open('%s') falhou: %s", DEVICE_CONFIG_NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "NVS namespace '%s' aberto", DEVICE_CONFIG_NVS_NAMESPACE);
    return ESP_OK;
}

esp_err_t device_config_load_raw(char *out_buf, size_t buf_size, size_t *out_len)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "device_config_load_raw chamado antes de device_config_init");
        return ESP_ERR_INVALID_STATE;
    }
    if (out_buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t required = buf_size;
    esp_err_t err = nvs_get_blob(s_nvs_handle, NVS_KEY_RAW, out_buf, &required);
    if (out_len != NULL) {
        *out_len = required;
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Nenhum config salvo ainda (primeiro boot ou nunca recebido)");
    } else if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "Config salvo (%d bytes) nao cabe no buffer (%d bytes) -- truncado",
                 (int)required, (int)buf_size);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob('%s') falhou: %s", NVS_KEY_RAW, esp_err_to_name(err));
    }
    return err;
}

esp_err_t device_config_apply_if_changed(const char *payload, size_t len, bool *out_changed)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "device_config_apply_if_changed chamado antes de device_config_init");
        return ESP_ERR_INVALID_STATE;
    }
    if (payload == NULL || len == 0 || len > DEVICE_CONFIG_MAX_LEN) {
        ESP_LOGE(TAG, "payload invalido (len=%d, maximo=%d)", (int)len, DEVICE_CONFIG_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t new_hash = esp_crc32_le(0, (const uint8_t *)payload, (uint32_t)len);

    uint32_t stored_hash = 0;
    esp_err_t hash_err = nvs_get_u32(s_nvs_handle, NVS_KEY_HASH, &stored_hash);
    bool have_stored = (hash_err == ESP_OK);

    if (have_stored && stored_hash == new_hash) {
        ESP_LOGI(TAG, "Config recebido e igual ao ja salvo (hash=0x%08" PRIx32 "), ignorando (sem escrita no NVS)",
                 new_hash);
        if (out_changed != NULL) {
            *out_changed = false;
        }
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Config novo/diferente (hash 0x%08" PRIx32 " -> 0x%08" PRIx32 "), salvando no NVS (%d bytes)",
             stored_hash, new_hash, (int)len);

    esp_err_t err = nvs_set_blob(s_nvs_handle, NVS_KEY_RAW, payload, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob('%s') falhou: %s", NVS_KEY_RAW, esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u32(s_nvs_handle, NVS_KEY_HASH, new_hash);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u32('%s') falhou: %s", NVS_KEY_HASH, esp_err_to_name(err));
        return err;
    }
    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit falhou: %s", esp_err_to_name(err));
        return err;
    }

    if (out_changed != NULL) {
        *out_changed = true;
    }
    return ESP_OK;
}
