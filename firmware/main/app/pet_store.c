#include "pet_store.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "pet_store";
#define NS  "tama"
#define KEY "pet"

bool pet_store_load(pet_t *p)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    pet_t tmp = {0};
    size_t len = sizeof(tmp);
    esp_err_t err = nvs_get_blob(h, KEY, &tmp, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no save (%s)", esp_err_to_name(err));
        return false;
    }
    // Older saves are shorter; the fields they lack were zeroed above.
    if ((tmp.version == 1 && len == PET_SAVE_V1_SIZE) || (tmp.version == 2 && len == PET_SAVE_V2_SIZE)) {
        ESP_LOGI(TAG, "migrating save v%lu -> v%d", (unsigned long)tmp.version, PET_SAVE_VERSION);
        tmp.version = PET_SAVE_VERSION;
        len = sizeof(tmp);
    }
    if (tmp.version != PET_SAVE_VERSION || len != sizeof(tmp)) {
        ESP_LOGW(TAG, "unusable save (version %lu, len %u)", (unsigned long)tmp.version, (unsigned)len);
        return false;
    }
    *p = tmp;
    return true;
}

void pet_store_save(const pet_t *p)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, KEY, p, sizeof(*p));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
}
