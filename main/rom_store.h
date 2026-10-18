#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "mac.h"

#define ROM_MAX_SIZE    (1024 * 1024)

/* Load the ROM for `model` into buf (ROM_MAX_SIZE bytes): from its flash
 * partition (rom_lc2, rom_lc3: roms/lc2.rom, roms/lc3.rom, written by
 * idf.py flash), else from a file in the SD card's "roms" folder whose
 * checksum is a ROM of that model. Its size in *size, its name in *name.
 */
bool rom_store_load(mac_model_t model, uint8_t *buf, uint32_t *size, const char **name);
