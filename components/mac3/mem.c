/*
 * The Musashi bus callbacks (physical addresses, after the 68030's MMU):
 * fast paths for RAM, ROM and video RAM, the same for both machines (lc.c
 * and sonora.c set the windows), everything else through the machine's
 * slow paths (model_ops_t). The 68030 also reads and writes words and
 * longs at odd addresses: those go byte by byte.
 *
 * Also the emulator's own side-effect-free access to Mac memory, and the
 * Memory Manager's free RAM for the status page.
 */
#include <string.h>
#include "m68k.h"
#include "mac_internal.h"

static inline uint16_t rd16(const uint8_t *mem, uint32_t offset)
{
    return *(const uint16_t *)(mem + offset);
}

static inline void wr16(uint8_t *mem, uint32_t offset, uint16_t value)
{
    *(uint16_t *)(mem + offset) = value;
}

static uint16_t slow_read16(uint32_t a)
{
    if (!(a & 1)) {
        return mac.ops->read16(a);
    }
    uint16_t hi = mac.ops->read8(a);
    return (hi << 8) | mac.ops->read8((a + 1) & mac.addr_mask);
}

static void slow_write16(uint32_t a, uint16_t v)
{
    if (!(a & 1)) {
        mac.ops->write16(a, v);
        return;
    }
    mac.ops->write8(a, v >> 8);
    mac.ops->write8((a + 1) & mac.addr_mask, v & 0xFF);
}

unsigned int MAC_HOT m68k_read_memory_8(unsigned int address)
{
    uint32_t a = address & mac.addr_mask;
    if (a < mac.ram_window) {
        return mac.ram[a ^ 1];
    }
    uint32_t v = a - mac.vram_base;
    if (v < mac.vram_window) {
        return mac.vram[v ^ 1];
    }
    uint32_t r = a - mac.rom_base;
    if (r < mac.rom_window) {
        return mac.rom[(r & mac.rom_mask) ^ 1];
    }
    return mac.ops->read8(a);
}

unsigned int MAC_HOT m68k_read_memory_16(unsigned int address)
{
    uint32_t a = address & mac.addr_mask;
    if (!(a & 1)) {
        if (a < mac.ram_window) {
            return rd16(mac.ram, a);
        }
        uint32_t r = a - mac.rom_base;
        if (r < mac.rom_window) {
            return rd16(mac.rom, r & mac.rom_mask);
        }
        uint32_t v = a - mac.vram_base;
        if (v < mac.vram_window) {
            return rd16(mac.vram, v);
        }
    }
    return slow_read16(a);
}

unsigned int MAC_HOT m68k_read_memory_32(unsigned int address)
{
    uint32_t a = address & mac.addr_mask;
    if (!(a & 1)) {
        if (a + 2 < mac.ram_window) {
            return ((uint32_t)rd16(mac.ram, a) << 16) | rd16(mac.ram, a + 2);
        }
        uint32_t r = a - mac.rom_base;
        if (r < mac.rom_window) {
            uint32_t o = r & mac.rom_mask;
            if (o <= mac.rom_mask - 3) {
                return ((uint32_t)rd16(mac.rom, o) << 16) | rd16(mac.rom, o + 2);
            }
        }
        uint32_t v = a - mac.vram_base;
        if (v + 2 < mac.vram_window) {
            return ((uint32_t)rd16(mac.vram, v) << 16) | rd16(mac.vram, v + 2);
        }
    }
    uint32_t hi = slow_read16(a);
    return (hi << 16) | slow_read16((a + 2) & mac.addr_mask);
}

void MAC_HOT m68k_write_memory_8(unsigned int address, unsigned int value)
{
    uint32_t a = address & mac.addr_mask;
    if (a < mac.ram_window) {
        mac.ram[a ^ 1] = value;
        return;
    }
    uint32_t v = a - mac.vram_base;
    if (v < mac.vram_window) {
        mac.vram[v ^ 1] = value;
        mac.vram_written = true;
        return;
    }
    mac.ops->write8(a, value);
}

void MAC_HOT m68k_write_memory_16(unsigned int address, unsigned int value)
{
    uint32_t a = address & mac.addr_mask;
    if (!(a & 1)) {
        if (a < mac.ram_window) {
            wr16(mac.ram, a, value);
            return;
        }
        uint32_t v = a - mac.vram_base;
        if (v < mac.vram_window) {
            wr16(mac.vram, v, value);
            mac.vram_written = true;
            return;
        }
    }
    slow_write16(a, value);
}

void MAC_HOT m68k_write_memory_32(unsigned int address, unsigned int value)
{
    uint32_t a = address & mac.addr_mask;
    if (!(a & 1)) {
        if (a + 2 < mac.ram_window) {
            wr16(mac.ram, a, value >> 16);
            wr16(mac.ram, a + 2, value & 0xFFFF);
            return;
        }
        uint32_t v = a - mac.vram_base;
        if (v + 2 < mac.vram_window) {
            wr16(mac.vram, v, value >> 16);
            wr16(mac.vram, v + 2, value & 0xFFFF);
            mac.vram_written = true;
            return;
        }
        if (!(a & 2)) {
            mac.ops->write32(a, value);
            return;
        }
    }
    slow_write16(a, value >> 16);
    slow_write16((a + 2) & mac.addr_mask, value & 0xFFFF);
}

/* ---- Side-effect-free access for the emulator ----
 *
 * At the Mac's logical addresses: RAM is at the same physical address on
 * both machines (the ROMs' MMU tables map it straight), and the machine's
 * peek16 knows where its ROM and VRAM appear.
 */

#define LM_MMU32BIT     0x0CB2      /* 0: 24-bit mode */

uint32_t mac_ptr(uint32_t p)
{
    const uint8_t *page = mac.ops->ram_page(LM_MMU32BIT);
    bool mode32 = page && page[LM_MMU32BIT ^ 1];
    return mode32 ? p : p & 0xFFFFFF;
}

uint16_t mac_peek16(uint32_t a)
{
    a &= ~1u;
    const uint8_t *p = mac.ops->ram_page(a);
    return p ? rd16(p, a & (MB - 1)) : mac.ops->peek16(a);
}

uint8_t mac_peek8(uint32_t a)
{
    uint16_t w = mac_peek16(a & ~1u);
    return (a & 1) ? (w & 0xFF) : (w >> 8);
}

uint32_t mac_peek32(uint32_t a)
{
    return ((uint32_t)mac_peek16(a) << 16) | mac_peek16(a + 2);
}

/* Writes go to RAM only */
void mac_poke8(uint32_t a, uint8_t v)
{
    uint8_t *p = mac.ops->ram_page(a);
    if (p) {
        p[(a & (MB - 1)) ^ 1] = v;
    }
}

void mac_poke16(uint32_t a, uint16_t v)
{
    mac_poke8(a, v >> 8);
    mac_poke8(a + 1, v & 0xFF);
}

void mac_poke32(uint32_t a, uint32_t v)
{
    mac_poke16(a, v >> 16);
    mac_poke16(a + 2, v & 0xFFFF);
}

uint32_t mac_ram_top(void)
{
    return mac.ram_size;
}

/* Disk transfers: within the RAM that maps straight (all of it once the
 * ROM has configured it)
 */
bool mac_ram_range(uint32_t a, uint32_t len)
{
    return a < mac.ram_window && len <= mac.ram_window - a;
}

void mac_ram_write(uint32_t a, const uint8_t *src, uint32_t len)
{
    if ((a & 1) == 0) {
        /* Whole words: swap pairs */
        uint32_t i = 0;
        for (; i + 1 < len; i += 2) {
            mac.ram[a + i] = src[i + 1];
            mac.ram[a + i + 1] = src[i];
        }
        if (i < len) {
            mac.ram[(a + i) ^ 1] = src[i];
        }
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        mac.ram[(a + i) ^ 1] = src[i];
    }
}

void mac_ram_read(uint32_t a, uint8_t *dst, uint32_t len)
{
    if ((a & 1) == 0) {
        uint32_t i = 0;
        for (; i + 1 < len; i += 2) {
            dst[i] = mac.ram[a + i + 1];
            dst[i + 1] = mac.ram[a + i];
        }
        if (i < len) {
            dst[i] = mac.ram[(a + i) ^ 1];
        }
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        dst[i] = mac.ram[(a + i) ^ 1];
    }
}

unsigned int m68k_read_disassembler_16(unsigned int a)
{
    return mac_peek16(a);
}

unsigned int m68k_read_disassembler_32(unsigned int a)
{
    return mac_peek32(a);
}

/* ---- Free memory, from the Memory Manager's zone headers ---- */

#define LM_BUFPTR       0x010C
#define LM_HEAPEND      0x0114
#define LM_APPLLIMIT    0x0130
#define LM_SYSZONE      0x02A6
#define LM_APPLZONE     0x02AA
#define ZONE_BKLIM      0       /* end of the zone */
#define ZONE_ZCBFREE    12      /* free bytes in it */
#define ZONE_HEADER     52

static uint32_t ptr24(uint32_t a)
{
    return mac_ptr(mac_peek32(a));
}

/* A zone header at z ending at or below `top`: its free bytes, or -1 */
static int64_t zone_free(uint32_t z, uint32_t top)
{
    if (z == 0 || (z & 1) || z + ZONE_HEADER >= mac.ram_size) {
        return -1;
    }
    uint32_t lim = ptr24(z + ZONE_BKLIM);
    uint32_t free_bytes = mac_peek32(z + ZONE_ZCBFREE);
    if (lim <= z + ZONE_HEADER || lim > top || free_bytes >= lim - z) {
        return -1;
    }
    return free_bytes;
}

bool mac_ram_free(uint32_t *free_bytes, uint32_t *system_free)
{
    *free_bytes = *system_free = 0;
    if (mac.ram == NULL || mac.overlay || mac_peek32(LM_TICKS) == 0) {
        return false;
    }
    uint32_t top = ptr24(LM_BUFPTR);
    uint32_t sys = ptr24(LM_SYSZONE);
    if (top == 0 || top > mac.ram_size) {
        return false;
    }
    int64_t sys_free = zone_free(sys, top);
    if (sys_free < 0) {
        return false;
    }
    *system_free = sys_free;

    /* System 7: the Process Manager's heap follows the system heap (after
     * its 12-byte end block) and reaches up to just below BufPtr
     */
    uint32_t sys_end = ptr24(sys + ZONE_BKLIM);
    for (uint32_t z = sys_end; z <= sys_end + 64; z += 2) {
        int64_t f = zone_free(z, top);
        if (f >= 0 && ptr24(z + ZONE_BKLIM) + 0x10000 >= top) {
            *free_bytes = f;
            return true;
        }
    }

    /* Otherwise: the application heap, and the room it can still grow into */
    uint32_t app = ptr24(LM_APPLZONE);
    int64_t app_free = zone_free(app, top);
    if (app_free < 0) {
        return false;
    }
    uint32_t heap_end = ptr24(LM_HEAPEND), limit = ptr24(LM_APPLLIMIT);
    *free_bytes = app_free + (limit > heap_end && limit <= top ? limit - heap_end : 0);
    return true;
}
