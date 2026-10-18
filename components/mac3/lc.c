/*
 * The Macintosh LC II: the V8's memory map, its RAM configuration, the
 * frame's landmarks.
 *
 * The V8 decodes A23-A0 (and A31: an address with A31 set reaches
 * nothing); the 68030's MMU maps the 32-bit address space onto that (the
 * ROM's tables put the ROM at $40A00000 and the I/O at $50F00000):
 *
 *   $000000-$9FFFFF  RAM, as the RAM configuration places it (the ROM while
 *                    the overlay is on); the first 2 MB on the board are
 *                    always also at $800000
 *   $A00000-$AFFFFF  ROM, repeated (the first read here ends the overlay)
 *   $F00000-$F01FFF  VIA1 (register n at + n * $200)
 *   $F04000-$F05FFF  SCC
 *   $F10000-$F11FFF  SCSI 5380 (register n at + n * $10)
 *   $F14000-$F15FFF  ASC
 *   $F16000-$F17FFF  SWIM (register n at + n * $200)
 *   $F24000-$F25FFF  Ariel colour table
 *   $F26000-$F27FFF  pseudo-VIA
 *   $F40000-$FBFFFF  video RAM
 *
 * Unmapped addresses read 0 (no bus errors), as in MAME, whose V8 the ROM
 * is happy with.
 */
#include <string.h>
#include "m68k.h"
#include "mac_internal.h"

#define ROM_BASE            0xA00000
#define VRAM_BASE           0xF40000
#define LC_VRAM_SIZE        0x80000     /* 512 KB */
#define RAM_PAGES           10          /* $000000-$9FFFFF */
#define BOARD_RAM           (4 * MB)

static struct {
    /* The host buffer: the SIMMs first, then the RAM on the board, so that
     * the usual configuration (SIMMs at 0, the board's RAM after them) maps
     * address a to ram[a]. With 10 MB (an 8 MB pair) only 2 MB of the
     * board's are used, as in MAME.
     */
    uint32_t simm_size;
    uint32_t board_size;
    uint8_t *ram_map[RAM_PAGES];    /* host memory of each 1 MB page, NULL = none */
} lc;

/* ---- RAM configuration (pseudo-VIA register 1) ---- */

static void update_windows(void)
{
    uint32_t pages = 0;
    if (!mac.overlay) {
        while (pages < RAM_PAGES && (pages + 1) * MB <= mac.ram_size &&
               lc.ram_map[pages] == mac.ram + pages * MB) {
            pages++;
        }
    }
    mac.ram_window = pages * MB;
    mac.rom_window = mac.overlay ? 0 : MB;
}

static void map_ram(uint32_t address, uint8_t *host, uint32_t size)
{
    for (uint32_t a = address; a < address + size && a < RAM_PAGES * MB; a += MB) {
        lc.ram_map[a / MB] = host + (a - address);
    }
}

/* Bits 6-7: SIMM size (none, 2, 4, 8 MB), bit 5: only 2 MB on the board.
 * The SIMMs go at 0, the board's RAM after the size the bits say (as in
 * MAME's V8, which the ROM's RAM sizing works with).
 */
void lc_set_ram_config(uint8_t config)
{
    if (mac.overlay) {
        return;
    }
    static const uint32_t simm_sizes[4] = { 0, 2 * MB, 4 * MB, 8 * MB };
    uint8_t *board = mac.ram + lc.simm_size;
    memset(lc.ram_map, 0, sizeof(lc.ram_map));
    map_ram(0x800000, board, 2 * MB);
    uint32_t board_at = 0;
    if (lc.simm_size > 0 && (config & 0xC0)) {
        map_ram(0, mac.ram, lc.simm_size);
        board_at = simm_sizes[config >> 6];
    }
    if ((config & 0xC0) != 0xC0) {
        uint32_t size = (config & 0x20) ? 2 * MB : BOARD_RAM;
        map_ram(board_at, board, size < lc.board_size ? size : lc.board_size);
    }
    update_windows();
}

static void set_overlay(bool on)
{
    if (on == mac.overlay) {
        return;
    }
    mac.overlay = on;
    if (on) {
        memset(lc.ram_map, 0, sizeof(lc.ram_map));
        update_windows();
    } else {
        lc_set_ram_config(0xC0);
    }
}

void lc_set_24bit(bool on)
{
    mac.addr_mask = on ? 0x00FFFFFF : 0x80FFFFFF;
}

/* ---- I/O ---- */

static uint8_t io_read8(uint32_t a)
{
    uint8_t v;
    switch ((a >> 13) & 0x7F) {
    case 0x00: v = via_read(&via1, a); break;
    case 0x02: v = scc_read(a); break;
    case 0x08: v = scsi_read(a); break;
    case 0x0A: v = asc_read(a); break;
    case 0x0B: v = (a & 1) ? 0 : swim_access(a, false, 0); break;
    case 0x12: v = ariel_read(a); break;
    case 0x13: v = pvia_read(a); break;
    default:   v = 0; break;
    }
    IO_LOG(a, false, v);
    return v;
}

static void io_write8(uint32_t a, uint8_t v)
{
    IO_LOG(a, true, v);
    switch ((a >> 13) & 0x7F) {
    case 0x00: via_write(&via1, a, v); break;
    case 0x02: scc_write(a, v); break;
    case 0x08: scsi_write(a, v); break;
    case 0x0A: asc_write(a, v); break;
    case 0x0B: swim_access(a, true, v); break;
    case 0x12: ariel_write(a, v); break;
    case 0x13: pvia_write(a, v); break;
    default:   break;
    }
}

/* ---- Slow paths (the fast ones in mem.c) ---- */

static uint8_t read8(uint32_t a)
{
    if (a >= 0x80000000) {
        return 0;
    }
    if (a < ROM_BASE) {
        if (mac.overlay) {
            return mac.rom[(a & mac.rom_mask) ^ 1];
        }
        const uint8_t *p = lc.ram_map[a >> 20];
        return p ? p[(a & (MB - 1)) ^ 1] : 0;
    }
    if (a < ROM_BASE + MB) {
        if (mac.overlay) {
            set_overlay(false);
        }
        return mac.rom[(a & mac.rom_mask) ^ 1];
    }
    if (a < 0xF00000) {
        return 0;
    }
    uint32_t v = a - VRAM_BASE;
    if (v < LC_VRAM_SIZE) {
        return mac.vram[v ^ 1];
    }
    return io_read8(a);
}

static void write8(uint32_t a, uint8_t value)
{
    if (a >= 0x80000000 || (a >= ROM_BASE && a < 0xF00000)) {
        return;
    }
    if (a < ROM_BASE) {
        uint8_t *p = mac.overlay ? NULL : lc.ram_map[a >> 20];
        if (p) {
            p[(a & (MB - 1)) ^ 1] = value;
        }
        return;
    }
    uint32_t v = a - VRAM_BASE;
    if (v < LC_VRAM_SIZE) {
        mac.vram[v ^ 1] = value;
        mac.vram_written = true;
        return;
    }
    io_write8(a, value);
}

static uint16_t read16(uint32_t a)
{
    uint16_t hi = read8(a);
    return (hi << 8) | read8((a + 1) & mac.addr_mask);
}

static void write16(uint32_t a, uint16_t v)
{
    write8(a, v >> 8);
    write8((a + 1) & mac.addr_mask, v & 0xFF);
}

static void write32(uint32_t a, uint32_t v)
{
    write16(a, v >> 16);
    write16((a + 2) & mac.addr_mask, v & 0xFFFF);
}

static uint8_t *ram_page(uint32_t a)
{
    a &= 0xFFFFFF;
    return a < ROM_BASE && !mac.overlay ? lc.ram_map[a >> 20] : NULL;
}

static uint16_t peek16(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a < ROM_BASE + MB && (a >= ROM_BASE || mac.overlay)) {
        return *(const uint16_t *)(mac.rom + (a & mac.rom_mask));
    }
    uint32_t v = a - VRAM_BASE;
    if (v < LC_VRAM_SIZE) {
        return *(const uint16_t *)(mac.vram + v);
    }
    return 0;
}

/* ---- The machine ---- */

static void reset(void)
{
    mac.overlay = false;
    set_overlay(true);
    lc_set_24bit(false);
    via_reset(&via1);
    pvia_reset(false);
    egret_reset();
    video_reset(false);
    asc_reset(false);
}

static void frame_start(void)
{
    egret_frame();
    via_set_flag(&via1, VIA_IF_CA1);    /* the 60.15 Hz tick */
}

static void vbl(bool active)
{
    pvia_vbl(active);
}

const model_ops_t lc_ops = {
    .reset = reset,
    .frame_start = frame_start,
    .vbl = vbl,
    .read8 = read8,
    .write8 = write8,
    .read16 = read16,
    .write16 = write16,
    .write32 = write32,
    .ram_page = ram_page,
    .peek16 = peek16,
    .screen_size = video_screen_size,
    .screen_depth = video_screen_depth,
    .render_line = video_render_line,
    .screen_changed = video_screen_changed,
};

void lc_init(void)
{
    lc.simm_size = mac.ram_size == 10 * MB ? 8 * MB : mac.ram_size - BOARD_RAM;
    lc.board_size = mac.ram_size - lc.simm_size;
    mac.rom_base = ROM_BASE;
    mac.rom_mac_base = ROM_BASE;
    mac.vram_base = VRAM_BASE;
    mac.vram_size = LC_VRAM_SIZE;
    mac.vram_window = LC_VRAM_SIZE;
    mac.cpu_scale_native = 512;
    mac.cpu_mhz = 15.6672f;
    via_init(&via1, &egret_via1_wiring, EV_VIA1_T1, EV_VIA1_T2);
}
