#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "mac.h"

/* Machine settings kept in NVS (namespace "mac3") */
typedef struct {
    uint8_t model;          /* mac_model_t */
    uint16_t ram_mb;        /* LC II 4, 6, 8, 10; LC III 4, 8, 16, 20 */
    uint8_t monitor;        /* mac_monitor_t */
} settings_t;

extern settings_t settings;

void settings_load(void);
void settings_save(void);

/* The model's parameter RAM (MAC_PRAM_SIZE bytes; each keeps its own, the
 * video's slot PRAM differs): false if none was saved yet
 */
bool settings_load_pram(mac_model_t model, uint8_t *pram);
void settings_save_pram(mac_model_t model, const uint8_t *pram);
