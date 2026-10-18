/*
 * The pseudo-VIA (the V8's at $F26000, Sonora's at $50026000): a VIA-like
 * register layout with no timers or shift register, the interrupt
 * controller for the slot interrupts (the built-in video's vertical
 * blanking among them) and the sound chip; on the V8 also the RAM and
 * video configuration:
 *
 *   $00  port B: bit 3 = 1 turns the HMMU off (32-bit addresses)
 *   $01  RAM configuration (mem_set_ram_config); reads with bit 2 set
 *   $02  slot interrupts, active low (bit 6 vertical blanking); writing
 *        bit 6 acknowledges it
 *   $03  interrupt flags (bit 1 any slot, 4 sound, 7 any); write 1s to clear
 *   $10  video: bits 0-2 depth (1, 2, 4, 8, 16 bits), bits 3-5 the monitor
 *        sense lines (6 = 13" RGB, 2 = 12" RGB)
 *   $12  slot interrupt enables, $13 interrupt enables (bit 7: set / clear)
 *   + $200: port A (nothing wired)
 *
 * As MAME's V8 and Sonora pseudo-VIAs (R. Belmont, hardware tests by Doug
 * Brown): the V8's reads decode A4, A1, A0, Sonora's A4-A0; the sound
 * interrupt is level triggered. Sonora has nothing on registers 0, 1 and
 * the monitor sense bits (its video has its own registers).
 */
#include <string.h>
#include "mac_internal.h"

static struct {
    uint8_t regs[0x20];
    uint8_t config;
    bool asc;                       /* sound chip's interrupt line */
    bool sonora;
} pv;

static void recalc(void)
{
    uint8_t slots = ~pv.regs[2] & 0x78 & pv.regs[0x12];
    if (slots) {
        pv.regs[3] |= 0x02;
    } else {
        pv.regs[3] &= ~0x02;
    }
    if (pv.asc) {
        pv.regs[3] |= 0x10;
    }
    uint8_t ifr = pv.regs[3] & pv.regs[0x13] & 0x1B;
    bool irq = ifr != 0;
    if (irq) {
        pv.regs[3] = ifr | 0x80;
    } else {
        pv.regs[3] &= ~0x80;
    }
    if (irq != mac.irq2) {
        mac.irq2 = irq;
        mac_update_irq();
    }
}

void pvia_reset(bool sonora)
{
    memset(&pv, 0, sizeof(pv));
    pv.sonora = sonora;
    pv.regs[2] = 0x7F;
    mac.irq2 = false;
    recalc();
}

void pvia_vbl(bool active)
{
    if (active) {
        pv.regs[2] &= ~0x40;
    } else {
        pv.regs[2] |= 0x40;
    }
    recalc();
}

void pvia_asc_irq(bool active)
{
    if (active != pv.asc) {
        pv.asc = active;
        if (!active) {
            pv.regs[3] &= ~0x10;
        }
        recalc();
    }
}

uint8_t pvia_video_config(void)
{
    return pv.regs[0x10];
}

uint8_t pvia_read(uint32_t address)
{
    uint32_t r = address & (pv.sonora ? 0x1F : 0x13);
    uint8_t data = pv.regs[r];
    switch (r) {
    case 0x00:
        return 0;
    case 0x01:
        return pv.sonora ? 0 : pv.config | 0x04;
    case 0x10:
        if (pv.sonora) {
            return data & ~0x38;
        }
        return (data & ~0x38) | ((mac.monitor == MAC_MONITOR_12 ? 2 : 6) << 3);
    case 0x12:
    case 0x13:
        return data & 0x7F;
    default:
        return data;
    }
}

void pvia_write(uint32_t address, uint8_t v)
{
    uint32_t offset = address & 0x1FFF;
    if ((offset >> 9) == 1) {
        return;                     /* port A */
    }
    offset &= 0x1F;
    switch (offset) {
    case 0x00:
        pv.regs[0] = v;
        if (!pv.sonora) {
            lc_set_24bit(!(v & 0x08));
        }
        break;
    case 0x01:
        pv.config = v;
        if (!pv.sonora) {
            lc_set_ram_config(v);
        }
        break;
    case 0x02:
        pv.regs[2] |= v & 0x40;
        recalc();
        break;
    case 0x03:
        v &= ~0x10;                 /* the sound interrupt is a level */
        pv.regs[3] &= ~(v & 0x7F);
        recalc();
        break;
    case 0x10:
        pv.regs[0x10] = v;
        if (!pv.sonora) {
            video_mode_changed();
        }
        break;
    case 0x12:
    case 0x13:
        if (v & 0x80) {
            pv.regs[offset] |= v & 0x7F;
        } else {
            pv.regs[offset] &= ~(v & 0x7F);
        }
        recalc();
        break;
    }
}
