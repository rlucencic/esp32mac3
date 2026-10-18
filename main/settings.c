#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

#define NAMESPACE   "mac3"

static const char *TAG = "settings";

settings_t settings;

void settings_load(void)
{
    settings.model = MAC_MODEL_LC3;
    settings.ram_mb = 8;
    settings.monitor = MAC_MONITOR_13;

    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, "model", &u8) == ESP_OK && u8 <= MAC_MODEL_LC3) {
        settings.model = u8;
    }
    if (nvs_get_u16(h, "ram_mb", &u16) == ESP_OK && u16 >= 4 && u16 <= 20) {
        settings.ram_mb = u16;
    }
    if (nvs_get_u8(h, "monitor", &u8) == ESP_OK && u8 <= MAC_MONITOR_12) {
        settings.monitor = u8;
    }
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_u8(h, "model", settings.model);
    nvs_set_u16(h, "ram_mb", settings.ram_mb);
    nvs_set_u8(h, "monitor", settings.monitor);
    nvs_commit(h);
    nvs_close(h);
}

static const char *pram_key(mac_model_t model)
{
    return model == MAC_MODEL_LC3 ? "pram_lc3" : "pram_lc2";
}

bool settings_load_pram(mac_model_t model, uint8_t *pram)
{
    nvs_handle_t h;
    size_t size = MAC_PRAM_SIZE;
    bool ok = false;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, pram_key(model), pram, &size) == ESP_OK && size == MAC_PRAM_SIZE;
        nvs_close(h);
    }
    return ok;
}

void settings_save_pram(mac_model_t model, const uint8_t *pram)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, pram_key(model), pram, MAC_PRAM_SIZE);
        nvs_commit(h);
        nvs_close(h);
    }
}
