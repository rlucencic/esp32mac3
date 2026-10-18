#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "emu/sdcard.h"
#include "rom_store.h"

#define ROM_PARTITION_TYPE  0x40
#define ROM_FOLDER          SDCARD_MOUNT_POINT "/roms"

static const char *TAG = "rom";

static const char *const partitions[] = { "rom_lc2", "rom_lc3" };
static const uint32_t sizes[] = { 512 * 1024, 1024 * 1024 };

static bool from_partition(mac_model_t model, uint8_t *buf, uint32_t *size)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ROM_PARTITION_TYPE, partitions[model]);
    if (part == NULL || part->size < sizes[model]) {
        return false;
    }
    /* Through a mapping, not esp_partition_read(): that turns the caches
     * off while it reads, which stalls the LCD's refill (picture artefacts)
     */
    const void *map;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(part, 0, sizes[model], ESP_PARTITION_MMAP_DATA, &map, &handle) != ESP_OK) {
        return false;
    }
    memcpy(buf, map, sizes[model]);
    esp_partition_munmap(handle);
    mac_model_t found;
    if (mac_rom_identify(buf, sizes[model], &found) == NULL || found != model) {
        return false;
    }
    *size = sizes[model];
    return true;
}

static bool from_sd(mac_model_t model, uint8_t *buf, uint32_t *size)
{
    DIR *dir = opendir(ROM_FOLDER);
    if (dir == NULL) {
        return false;
    }
    bool ok = false;
    struct dirent *e;
    while (!ok && (e = readdir(dir)) != NULL) {
        char path[300];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", ROM_FOLDER, e->d_name);
        if (e->d_name[0] == '.' || stat(path, &st) != 0 || st.st_size != sizes[model]) {
            continue;
        }
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            continue;
        }
        mac_model_t found;
        ok = fread(buf, 1, sizes[model], f) == sizes[model] &&
             mac_rom_identify(buf, sizes[model], &found) != NULL && found == model;
        fclose(f);
        if (ok) {
            ESP_LOGI(TAG, "ROM from %s", path);
        }
    }
    closedir(dir);
    if (ok) {
        *size = sizes[model];
    }
    return ok;
}

bool rom_store_load(mac_model_t model, uint8_t *buf, uint32_t *size, const char **name)
{
    if (model > MAC_MODEL_LC3) {
        return false;
    }
    if (!from_partition(model, buf, size) && !from_sd(model, buf, size)) {
        return false;
    }
    *name = mac_rom_identify(buf, *size, NULL);
    return true;
}
