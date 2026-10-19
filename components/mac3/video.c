/*
 * The built-in video, from the machine's own video RAM through a colour
 * table (pixel values to colours):
 *
 *   LC II (V8)     512 KB of VRAM, 1024 bytes a line up to 8 bits per
 *                  pixel, 2 bytes a pixel at 16; the depth in the
 *                  pseudo-VIA's register $10; the Ariel RAMDAC at $F24000
 *   LC III (Sonora) 1 MB of VRAM, the lines packed (width x depth / 8
 *                  bytes); mode registers at $50F28000 (0 the mode: $06
 *                  13", $02 12", bit 7 blanked; 1 the depth; 2 the monitor
 *                  sense lines; 4-7 the beam's position), the colour table
 *                  at $50F24000 (as MAME's mv_sonora.cpp, O. Galibert)
 *
 * The colour table: register 0 the table address (writing it, or reading
 * it on Ariel, starts a red-green-blue triple), 1 the data (red, green,
 * blue, then the next entry), 2 control, 3 key colour. With fewer bits per
 * pixel the low bits are 1s: 1 bit uses entries $7F and $FF, 2 bits
 * $3F/$7F/$BF/$FF and so on (as MAME's V8 and Sonora).
 */
#include <string.h>
#include "mac_internal.h"

static struct {
    bool sonora;
    uint8_t address, component, control;
    uint8_t clut[256][3];
    uint16_t pens[256];             /* RGB565 */
    bool changed;
    /* Sonora's mode registers */
    uint8_t mode, depth, monitor_id, vtest;
} ar;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

void video_reset(bool sonora)
{
    memset(&ar, 0, sizeof(ar));
    ar.sonora = sonora;
    /* Until the ROM sets it: a grey ramp */
    for (int i = 0; i < 256; i++) {
        ar.clut[i][0] = ar.clut[i][1] = ar.clut[i][2] = 255 - i;
        ar.pens[i] = rgb565(255 - i, 255 - i, 255 - i);
    }
    ar.mode = 0x9F;
    ar.monitor_id = 8;
    ar.changed = true;
}

void video_mode_changed(void)
{
    ar.changed = true;
}

static void next_component(void)
{
    if (++ar.component == 3) {
        ar.component = 0;
        ar.address++;
    }
}

uint8_t ariel_read(uint32_t address)
{
    switch (address & (ar.sonora ? 3 : 0x1FFF)) {
    case 0:
        if (ar.sonora) {
            return 0;
        }
        ar.component = 0;
        return ar.address;
    case 1: {
        if (ar.sonora) {
            return 0;
        }
        uint8_t v = ar.clut[ar.address][ar.component];
        next_component();
        return v;
    }
    case 2:
        return ar.control;
    case 3:
        return ar.sonora ? 0 : ar.control;
    default:
        return 0;
    }
}

void ariel_write(uint32_t address, uint8_t value)
{
    switch (address & (ar.sonora ? 3 : 0x1FFF)) {
    case 0:
        ar.address = value;
        ar.component = 0;
        break;
    case 1: {
        uint8_t *c = ar.clut[ar.address];
        c[ar.component] = value;
        if (ar.component == 2) {
            ar.pens[ar.address] = rgb565(c[0], c[1], c[2]);
            ar.changed = true;
        }
        next_component();
        break;
    }
    case 2:
    case 3:
        ar.control = value;
        break;
    }
}

/* ---- Sonora's mode registers ---- */

static int sonora_mode_id(void)
{
    return mac.monitor == MAC_MONITOR_12 ? 0x02 : 0x06;
}

/* Any of Sonora's modes runs the video timing, and with it the vertical
 * blanking interrupt (as MAME's: the ROM starts in the 13" mode, $86,
 * whatever the monitor, and waits for one before it senses the monitor)
 */
bool sonora_video_on(void)
{
    switch (ar.mode & 0x1F) {
    case 0x01: case 0x02: case 0x06: case 0x09: case 0x0B:
        return true;
    default:
        return false;
    }
}

uint8_t sonora_vctrl_read(uint32_t address)
{
    switch (address & 7) {
    case 0:
        return ar.mode;
    case 1:
        return ar.depth;
    case 2: {
        /* The monitor's sense lines: 6 (13") or 2 (12"), less what the Mac
         * drives low
         */
        uint8_t sense = sonora_mode_id();
        if (!(ar.monitor_id & 8)) {
            sense &= ar.monitor_id & 7;
        }
        return ar.monitor_id | (sense << 4);
    }
    case 3:
        return ar.vtest;
    default: {
        /* The beam: 896 pixels a line, 525 lines (13"), from the frame's time */
        uint64_t t = mac_now() - mac.frame_start;
        uint32_t line = (uint32_t)(t / LINE_CYCLES), cycle = (uint32_t)(t % LINE_CYCLES);
        uint32_t v = line * 525 / FRAME_LINES, h = cycle * 896 / LINE_CYCLES;
        switch (address & 7) {
        case 4:  return (h >> 8) & 7;
        case 5:  return h & 0xFF;
        case 6:  return (v >> 8) & 3;
        default: return v & 0xFF;
        }
    }
    }
}

void sonora_vctrl_write(uint32_t address, uint8_t value)
{
    switch (address & 7) {
    case 0:
        ar.mode = value & 0x9F;
        ar.changed = true;
        break;
    case 1:
        ar.depth = value & 7;
        ar.changed = true;
        break;
    case 2:
        ar.monitor_id = value & 0xF;
        break;
    case 3:
        ar.vtest = value & 1;
        break;
    }
}

/* ---- The picture ---- */

void video_screen_size(int *width, int *height)
{
    if (mac.monitor == MAC_MONITOR_12) {
        *width = 512;
        *height = 384;
    } else {
        *width = 640;
        *height = 480;
    }
}

/* 0-4: 1, 2, 4, 8, 16 bits per pixel */
static int depth_code(void)
{
    return ar.sonora ? ar.depth : pvia_video_config() & 7;
}

int video_screen_depth(void)
{
    int code = depth_code();
    return code <= 4 ? 1 << code : 1;
}

void video_render_line(int y, uint16_t *dst)
{
    int w, h;
    video_screen_size(&w, &h);
    int code = depth_code();
    if (y < 0 || y >= h || mac.vram == NULL || code > 4 ||
        (ar.sonora && ((ar.mode & 0x80) || (ar.mode & 0x1F) != sonora_mode_id()))) {
        memset(dst, 0, w * sizeof(uint16_t));
        return;
    }
    const uint8_t *vram = mac.vram;
    const uint16_t *pens = ar.pens;
    /* The V8: 1024 bytes a line up to 8 bits; Sonora: packed */
    uint32_t row = ar.sonora ? (uint32_t)w << code >> 3 : code == 4 ? (uint32_t)w * 2 : 1024;
    uint32_t line = (uint32_t)y * row;
    if (line + row > mac.vram_size) {
        memset(dst, 0, w * sizeof(uint16_t));
        return;
    }
    switch (code) {
    case 0:
        for (int x = 0; x < w / 8; x++) {
            uint8_t b = vram[(line + x) ^ 1];
            for (int i = 7; i >= 0; i--) {
                *dst++ = pens[0x7F | (((b >> i) & 1) << 7)];
            }
        }
        break;
    case 1:
        for (int x = 0; x < w / 4; x++) {
            uint8_t b = vram[(line + x) ^ 1];
            for (int i = 6; i >= 0; i -= 2) {
                *dst++ = pens[0x3F | (((b >> i) & 3) << 6)];
            }
        }
        break;
    case 2:
        for (int x = 0; x < w / 2; x++) {
            uint8_t b = vram[(line + x) ^ 1];
            *dst++ = pens[0x0F | (b & 0xF0)];
            *dst++ = pens[0x0F | ((b << 4) & 0xF0)];
        }
        break;
    case 3:
        for (int x = 0; x < w; x++) {
            *dst++ = pens[vram[(line + x) ^ 1]];
        }
        break;
    default:
        /* 16 bits: x RRRRR GGGGG BBBBB, not through the colour table */
        for (int x = 0; x < w; x++) {
            uint16_t p = *(const uint16_t *)(vram + line + x * 2);
            uint16_t g = (p >> 5) & 0x1F;
            *dst++ = ((p >> 10) & 0x1F) << 11 | g << 6 | (g >> 4) << 5 | (p & 0x1F);
        }
        break;
    }
}

bool video_screen_changed(void)
{
    if (mac.vram_written || ar.changed) {
        mac.vram_written = false;
        ar.changed = false;
        return true;
    }
    return false;
}
