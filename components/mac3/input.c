/*
 * Keyboard and mouse input from other tasks (Bluetooth, touch, console).
 *
 * Events go through a small queue; the emulation drains it once a frame.
 * Keys wait in a second queue for the ADB keyboard. Relative motion goes
 * through the ADB mouse (so the Mac's own acceleration applies); absolute
 * positions (touch) through the low memory cursor globals, as Mini vMac
 * does. (From ../esp32mac.)
 */
#include <string.h>
#include "mac_internal.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()      portENTER_CRITICAL(&s_lock)
#define UNLOCK()    portEXIT_CRITICAL(&s_lock)
#else
#define LOCK()
#define UNLOCK()
#endif

#define QUEUE_SIZE      64      /* power of two */
#define KEY_QUEUE       32
#define BUTTON_HOLD     3       /* frames between button changes (the Mac debounces) */

static input_event_t s_queue[QUEUE_SIZE];
static volatile uint32_t s_head, s_tail;

static struct {
    uint8_t keys[KEY_QUEUE];        /* code | 0x80 when released */
    int key_head, key_tail;
    bool button;
    int button_hold;
    int dx, dy;                     /* relative motion not delivered yet */
    bool absolute;                  /* a position to put the pointer at */
    int x, y;
} in;

static void push(const input_event_t *ev)
{
    LOCK();
    if (s_head - s_tail < QUEUE_SIZE) {
        s_queue[s_head % QUEUE_SIZE] = *ev;
        __atomic_store_n(&s_head, s_head + 1, __ATOMIC_RELEASE);
    }
    UNLOCK();
}

void mac_key(uint8_t keycode, bool down)
{
    input_event_t ev = { .type = INPUT_KEY, .code = keycode & 0x7F, .value = down };
    push(&ev);
}

void mac_mouse(int dx, int dy, bool button)
{
    input_event_t ev = { .type = INPUT_MOUSE, .value = button, .dx = dx, .dy = dy };
    push(&ev);
}

void mac_mouse_position(int x, int y, bool button)
{
    input_event_t ev = { .type = INPUT_MOUSE_ABS, .value = button, .dx = x, .dy = y };
    push(&ev);
}

void input_reset(void)
{
    in.key_head = in.key_tail = 0;
    in.dx = in.dy = 0;
    in.absolute = false;
    in.button = false;
    in.button_hold = 0;
}

bool input_next_key(uint8_t *code, bool *down)
{
    if (in.key_tail == in.key_head) {
        return false;
    }
    uint8_t k = in.keys[in.key_tail % KEY_QUEUE];
    in.key_tail++;
    *code = k & 0x7F;
    *down = !(k & 0x80);
    return true;
}

bool mouse_button(void)
{
    return in.button;
}

int adb_mouse_delta(int *dx, int *dy)
{
    *dx = in.dx;
    *dy = in.dy;
    return in.dx != 0 || in.dy != 0;
}

bool input_key_waiting(void)
{
    return in.key_tail != in.key_head;
}

void adb_mouse_took(int dx, int dy)
{
    in.dx -= dx;
    in.dy -= dy;
}

/* Drain the queue: keys to the key queue, motion added up, at most one
 * button change per BUTTON_HOLD frames (the rest waits in the queue)
 */
static void drain(void)
{
    if (in.button_hold > 0) {
        in.button_hold--;
    }
    uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    while (s_tail != head) {
        input_event_t *ev = &s_queue[s_tail % QUEUE_SIZE];
        if (ev->type == INPUT_KEY) {
            if (in.key_head - in.key_tail < KEY_QUEUE) {
                in.keys[in.key_head % KEY_QUEUE] = ev->code | (ev->value ? 0 : 0x80);
                in.key_head++;
            }
        } else {
            bool button = ev->value != 0;
            if (button != in.button) {
                if (in.button_hold > 0) {
                    break;
                }
                in.button = button;
                in.button_hold = BUTTON_HOLD;
            }
            if (ev->type == INPUT_MOUSE) {
                in.dx += ev->dx;
                in.dy += ev->dy;
            } else {
                in.absolute = true;
                in.x = ev->dx;
                in.y = ev->dy;
                in.dx = in.dy = 0;
            }
        }
        __atomic_store_n(&s_tail, s_tail + 1, __ATOMIC_RELEASE);
    }
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void mouse_frame(void)
{
    drain();
    if (!adb_mouse_active()) {
        in.dx = in.dy = 0;
        in.absolute = false;
        return;
    }
    if (in.absolute) {
        in.absolute = false;
        int w, h;
        mac_screen_size(&w, &h);
        /* Low memory: MTemp, RawMouse and Mouse are (v, h) word pairs */
        uint32_t pos = ((uint32_t)(uint16_t)clamp(in.y, 0, h - 1) << 16) | (uint16_t)clamp(in.x, 0, w - 1);
        mac_poke32(LM_MTEMP, pos);
        mac_poke32(LM_RAWMOUSE, pos);
        mac_poke32(LM_MOUSE, pos);
        mac_poke8(LM_CRSRNEW, 0xFF);
    }
}
