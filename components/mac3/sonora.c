/*
 * The Macintosh LC III: Sonora's memory map, the frame's landmarks.
 *
 * A full 32-bit physical map (as MAME's sonora.cpp and maclc3.cpp, R.
 * Belmont); the 68030's MMU makes the 24-bit mode's map out of it:
 *
 *   $00000000-       RAM, all of it in one piece (the ROM while the
 *                    overlay is on, up to its size)
 *   $40000000-$4FFFFFFF  ROM, repeated (the first read here ends the overlay)
 *   $50000000-$50FFFFFF  I/O, each block repeated every 1 MB (VIA1 every
 *                    256 KB):
 *                      +$00000  VIA1 (register n at + n * $200)
 *                      +$04000  SCC
 *                      +$10000  SCSI 5380
 *                      +$14000  ASC (Sonora's)
 *                      +$16000  SWIM
 *                      +$26000  pseudo-VIA
 *                    $50F24000 the colour table, $50F28000 the video's
 *                    mode registers
 *   $5FFFFFFC        the machine's ID: $A55A0001
 *   $60000000-$6FFFFFFF  video RAM (1 MB, repeated)
 *
 * Unmapped addresses read 0 (no bus errors), as in MAME.
 */
#include <string.h>
#include "m68k.h"
#include "mac_internal.h"

#define ROM_BASE            0x40000000u
#define IO_BASE             0x50000000u
#define VRAM_BASE           0x60000000u
#define SPACE               0x10000000u     /* each of them */
#define ID_ADDRESS          0x5FFFFFFCu
#define MACHINE_ID          0xA55A0001u

static void update_windows(void)
{
    mac.ram_window = mac.overlay ? 0 : mac.ram_size;
    mac.rom_window = mac.overlay ? 0 : SPACE;
}

static void set_overlay(bool on)
{
    mac.overlay = on;
    update_windows();
}

/* ---- I/O ---- */

static uint8_t io_read8(uint32_t a)
{
    uint8_t v;
    uint32_t o = a & 0xFFFFFF;
    if (o >= 0xF24000 && o < 0xF24004) {
        v = ariel_read(a);
    } else if (o >= 0xF28000 && o < 0xF28008) {
        v = sonora_vctrl_read(a);
    } else if ((o & 0x3E000) == 0) {
        v = via_read(&via1, a);
    } else {
        switch ((o >> 13) & 0x7F) {
        case 0x02: v = scc_read(a); break;
        case 0x08: v = scsi_read(a); break;
        case 0x0A: v = asc_read(a); break;
        case 0x0B: v = (a & 1) ? 0 : swim_access(a, false, 0); break;
        case 0x13: v = pvia_read(a); break;
        default:   v = 0; break;
        }
    }
    IO_LOG(a, false, v);
    return v;
}

static void io_write8(uint32_t a, uint8_t v)
{
    IO_LOG(a, true, v);
    uint32_t o = a & 0xFFFFFF;
    if (o >= 0xF24000 && o < 0xF24004) {
        ariel_write(a, v);
    } else if (o >= 0xF28000 && o < 0xF28008) {
        sonora_vctrl_write(a, v);
    } else if ((o & 0x3E000) == 0) {
        via_write(&via1, a, v);
    } else {
        switch ((o >> 13) & 0x7F) {
        case 0x02: scc_write(a, v); break;
        case 0x08: scsi_write(a, v); break;
        case 0x0A: asc_write(a, v); break;
        case 0x0B: swim_access(a, true, v); break;
        case 0x13: pvia_write(a, v); break;
        default:   break;
        }
    }
}

/* ---- Slow paths (the fast ones in mem.c) ---- */

static uint8_t read8(uint32_t a)
{
    if (a < ROM_BASE) {
        if (mac.overlay) {
            return a < mac.rom_size ? mac.rom[a ^ 1] : 0;
        }
        return a < mac.ram_size ? mac.ram[a ^ 1] : 0;
    }
    if (a < IO_BASE) {
        if (mac.overlay) {
            set_overlay(false);
        }
        return mac.rom[(a & mac.rom_mask) ^ 1];
    }
    if (a < VRAM_BASE) {
        if (a >= ID_ADDRESS) {
            return MACHINE_ID >> (8 * (3 - (a & 3)));
        }
        return a < IO_BASE + 0x1000000 ? io_read8(a) : 0;
    }
    if (a < VRAM_BASE + SPACE) {
        return mac.vram[((a - VRAM_BASE) & (mac.vram_size - 1)) ^ 1];
    }
    return 0;
}

static void write8(uint32_t a, uint8_t value)
{
    if (a < ROM_BASE) {
        if (!mac.overlay && a < mac.ram_size) {
            mac.ram[a ^ 1] = value;
        }
        return;
    }
    if (a < IO_BASE) {
        return;
    }
    if (a < VRAM_BASE) {
        if (a < IO_BASE + 0x1000000) {
            io_write8(a, value);
        }
        return;
    }
    if (a < VRAM_BASE + SPACE) {
        mac.vram[((a - VRAM_BASE) & (mac.vram_size - 1)) ^ 1] = value;
        mac.vram_written = true;
    }
}

static uint16_t read16(uint32_t a)
{
    uint16_t hi = read8(a);
    return (hi << 8) | read8(a + 1);
}

static void write16(uint32_t a, uint16_t v)
{
    write8(a, v >> 8);
    write8(a + 1, v & 0xFF);
}

static void write32(uint32_t a, uint32_t v)
{
    write16(a, v >> 16);
    write16(a + 2, v & 0xFFFF);
}

static uint8_t *ram_page(uint32_t a)
{
    return a < mac.ram_size && !mac.overlay ? mac.ram + (a & ~(MB - 1)) : NULL;
}

/* The ROM is at $40800000 too (in 32-bit mode the Mac uses that; the 24-bit
 * mode's map has it at $800000)
 */
static uint16_t peek16(uint32_t a)
{
    if ((a >= ROM_BASE && a < IO_BASE) || (a < mac.rom_size && mac.overlay) ||
        (a & 0xFFF00000) == 0x800000) {
        return *(const uint16_t *)(mac.rom + (a & mac.rom_mask));
    }
    if (a >= VRAM_BASE && a < VRAM_BASE + SPACE) {
        return *(const uint16_t *)(mac.vram + ((a - VRAM_BASE) & (mac.vram_size - 1)));
    }
    return 0;
}

/* ---- The machine ---- */

static void reset(void)
{
    set_overlay(true);
    via_reset(&via1);
    pvia_reset(true);
    egret_reset();
    video_reset(true);
    asc_reset(true);
}

static void frame_start(void)
{
    egret_frame();
    via_set_flag(&via1, VIA_IF_CA1);    /* the 60.15 Hz tick */
}

/* Vertical blanking interrupts once a mode is set */
static void vbl(bool active)
{
    pvia_vbl(active && sonora_video_on());
}

const model_ops_t sonora_ops = {
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

void sonora_init(void)
{
    mac.addr_mask = 0xFFFFFFFF;
    mac.rom_base = ROM_BASE;
    mac.rom_mac_base = 0x800000;
    mac.vram_base = VRAM_BASE;
    mac.vram_size = MAC_VRAM_SIZE;
    mac.vram_window = MAC_VRAM_SIZE;
    /* A 25 MHz 68030 on a 32-bit bus: Musashi's own timing scaled to it
     * (15.67 / 25), against the LC II's 16-bit bus at 512
     */
    mac.cpu_scale_native = 160;
    mac.cpu_mhz = 25.0f;
    via_init(&via1, &egret_via1_wiring, EV_VIA1_T1, EV_VIA1_T2);
}
