/*
 * The ROMs this emulator knows, and the patches it makes to its copy.
 * Every patch checks the bytes it replaces first.
 *
 *   - The ROM checksum test is skipped (the copy is patched, so it would
 *     fail: the death chimes and the serial test manager).
 *   - The .Sony disk driver is replaced by the paravirtual one (sony.c).
 *
 * The 68030 core always has its FPU (both Macs take a 68882 in an add-on
 * slot), and the ROMs find it.
 */
#include <string.h>
#include "mac_internal.h"

typedef struct {
    uint32_t offset;
    uint16_t expect;
    uint16_t value;
} patch_t;

static const patch_t lc2_patches[] = {
    { 0x46AFC, 0xB981, 0xB381 },    /* checksum: eor.l d4,d1 -> eor.l d1,d1 */
};

static const patch_t lc3_patches[] = {
    { 0x47A7A, 0xB981, 0xB381 },    /* checksum: eor.l d4,d1 -> eor.l d1,d1 */
};

typedef struct {
    uint32_t checksum;
    uint32_t size;
    const char *name;
    mac_model_t model;
    uint32_t sony;                  /* the .Sony driver's header */
    const patch_t *patches;
    size_t patch_count;
} known_rom_t;

static const known_rom_t known[] = {
    { 0x35C28F5F, 0x80000, "Macintosh LC II", MAC_MODEL_LC2, 0x6C3E0,
      lc2_patches, sizeof(lc2_patches) / sizeof(lc2_patches[0]) },
    { 0xECBBC41C, 0x100000, "Macintosh LC III", MAC_MODEL_LC3, 0x6C3E0,
      lc3_patches, sizeof(lc3_patches) / sizeof(lc3_patches[0]) },
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static const known_rom_t *find(const uint8_t *rom, uint32_t size)
{
    if (rom == NULL || size < 4) {
        return NULL;
    }
    uint32_t sum = be32(rom);
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (known[i].checksum == sum && known[i].size == size) {
            return &known[i];
        }
    }
    return NULL;
}

const char *mac_rom_identify(const uint8_t *rom, uint32_t size, mac_model_t *model)
{
    const known_rom_t *k = find(rom, size);
    if (k == NULL) {
        return NULL;
    }
    if (model) {
        *model = k->model;
    }
    return k->name;
}

/* The copy keeps each 16-bit word in host order */
static uint16_t rd16(uint32_t offset)
{
    return *(uint16_t *)(mac.rom + offset);
}

static void wr16(uint32_t offset, uint16_t value)
{
    *(uint16_t *)(mac.rom + offset) = value;
}

static void apply(const patch_t *patches, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint16_t now = rd16(patches[i].offset);
        if (now == patches[i].expect) {
            wr16(patches[i].offset, patches[i].value);
        } else {
            MAC_LOGW("ROM patch at $%05lx skipped: $%04x, expected $%04x",
                     (unsigned long)patches[i].offset, now, patches[i].expect);
        }
    }
}

bool rom_prepare(const uint8_t *image, uint32_t size)
{
    const known_rom_t *k = find(image, size);
    if (k == NULL) {
        return false;
    }
    /* Byte pairs swapped (works in place too: image may be the copy) */
    for (uint32_t i = 0; i < size; i += 2) {
        uint8_t hi = image[i], lo = image[i + 1];
        mac.rom[i] = lo;
        mac.rom[i + 1] = hi;
    }
    apply(k->patches, k->patch_count);
    /* The original driver: flags $4F00 and the name ".Sony" */
    static const uint8_t name[6] = { 5, '.', 'S', 'o', 'n', 'y' };
    bool found = rd16(k->sony) == 0x4F00;
    for (int i = 0; i < 6 && found; i++) {
        found = mac.rom[(k->sony + 0x12 + i) ^ 1] == name[i];
    }
    if (!found) {
        MAC_LOGW("no .Sony driver at $%05lx", (unsigned long)k->sony);
        return false;
    }
    return sony_install(mac.rom, size, k->sony, mac.rom_mac_base);
}
