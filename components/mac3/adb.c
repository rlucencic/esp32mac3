/*
 * The ADB devices: an Apple keyboard at address 2 and a mouse at address 3,
 * behind Egret. Each ADB command comes whole: aaaa cc rr, address, command
 * (0 reset/flush, 2 listen, 3 talk), register.
 */
#include <string.h>
#include "mac_internal.h"

enum { DEV_KEYBOARD, DEV_MOUSE, DEVICES };

static const uint8_t default_address[DEVICES] = { 2, 3 };
static const uint8_t default_handler[DEVICES] = { 2, 1 };   /* extended keyboard, 100 cpi mouse */

static struct {
    uint8_t address[DEVICES];
    uint8_t handler[DEVICES];
    bool mouse_button;              /* as last reported */
    bool mouse_active;              /* the Mac talks to the mouse */
} adb;

void adb_reset(void)
{
    memset(&adb, 0, sizeof(adb));
    memcpy(adb.address, default_address, sizeof(adb.address));
    memcpy(adb.handler, default_handler, sizeof(adb.handler));
}

bool adb_mouse_active(void)
{
    return adb.mouse_active;
}

/* ADB key codes differ from the Mac's virtual ones for a few keys */
static uint8_t adb_key(uint8_t code)
{
    switch (code) {
    case 0x3B: return 0x36;     /* Control */
    case 0x7B: return 0x3B;     /* arrows: left, right, down, up */
    case 0x7C: return 0x3C;
    case 0x7D: return 0x3D;
    case 0x7E: return 0x3E;
    default:   return code;
    }
}

static int clamp63(int v)
{
    return v > 63 ? 63 : v < -63 ? -63 : v;
}

/* Register 0: what the device has to report, or -1 (no answer) */
static int talk0(int dev, uint8_t *out)
{
    if (dev == DEV_KEYBOARD) {
        uint8_t code;
        bool down;
        if (!input_next_key(&code, &down)) {
            return -1;
        }
        out[0] = adb_key(code) | (down ? 0 : 0x80);
        out[1] = 0xFF;
        if (input_next_key(&code, &down)) {
            out[1] = adb_key(code) | (down ? 0 : 0x80);
        }
        return 2;
    }
    int dx, dy;
    adb_mouse_delta(&dx, &dy);
    dx = clamp63(dx);
    dy = clamp63(dy);
    bool button = mouse_button();
    adb.mouse_active = true;
    if (!dx && !dy && button == adb.mouse_button) {
        return -1;
    }
    adb_mouse_took(dx, dy);
    adb.mouse_button = button;
    out[0] = (button ? 0x00 : 0x80) | (dy & 0x7F);
    out[1] = 0x80 | (dx & 0x7F);
    return 2;
}

static int device_at(uint8_t address)
{
    for (int d = 0; d < DEVICES; d++) {
        if (adb.address[d] == address) {
            return d;
        }
    }
    return -1;
}

int adb_command(uint8_t command, const uint8_t *data, int len, uint8_t *out)
{
    uint8_t address = command >> 4, reg = command & 3;
    int op = (command >> 2) & 3;
    if (op == 0) {
        if (reg == 0) {             /* SendReset: everybody */
            memcpy(adb.address, default_address, sizeof(adb.address));
            memcpy(adb.handler, default_handler, sizeof(adb.handler));
            return 0;
        }
        return device_at(address) >= 0 ? 0 : -1;    /* Flush */
    }
    int dev = device_at(address);
    if (dev < 0) {
        return -1;
    }
    if (op == 2) {                  /* listen */
        if (reg == 3 && len >= 2) {
            if (data[1] == 0xFE) {
                adb.address[dev] = data[0] & 0x0F;
            } else if (data[1] != 0 && data[1] != 0xFD && data[1] != 0xFF) {
                if ((dev == DEV_KEYBOARD && (data[1] == 1 || data[1] == 2 || data[1] == 3)) ||
                    (dev == DEV_MOUSE && (data[1] == 1 || data[1] == 2))) {
                    adb.handler[dev] = data[1];
                }
            }
        }
        return 0;
    }
    if (op == 3) {                  /* talk */
        switch (reg) {
        case 0:
            return talk0(dev, out);
        case 2:
            if (dev == DEV_KEYBOARD) {
                out[0] = 0xFF;      /* no modifiers down, LEDs off */
                out[1] = 0xFF;
                return 2;
            }
            return -1;
        case 3:
            out[0] = 0x60 | adb.address[dev];
            out[1] = adb.handler[dev];
            return 2;
        default:
            return -1;
        }
    }
    return -1;
}

bool adb_anything_to_report(void)
{
    int dx, dy;
    return input_key_waiting() || adb_mouse_delta(&dx, &dy) || mouse_button() != adb.mouse_button;
}

int adb_poll(uint16_t devices, uint8_t *command, uint8_t *out)
{
    for (int d = 0; d < DEVICES; d++) {
        uint8_t address = adb.address[d];
        if (devices && !(devices & (1u << address))) {
            continue;
        }
        if (d == DEV_KEYBOARD && !input_key_waiting()) {
            continue;
        }
        int n = talk0(d, out);
        if (n > 0) {
            *command = (address << 4) | 0x0C;
            return n;
        }
    }
    return 0;
}
