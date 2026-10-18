/*
 * VIA 6522 (VIA1 of the LC II and LC III): register n at + n * $200,
 * either byte.
 *
 * Timers count at CPU / 20 and are kept as (start time, start value), so
 * they cost nothing between their scheduled expiry events. T1 can drive
 * PB7. CA1, CA2, CB1 and CB2 are inputs with the edge the PCR asks for.
 * The shift register talks to a device under its clock (Egret): the device
 * side is via_sr_send() / via_sr_receive(). The machine wires the ports
 * through via_wiring_t. (From ../esp32mac's VIA.)
 */
#include <string.h>
#include "mac_internal.h"

enum {
    R_ORB, R_ORA, R_DDRB, R_DDRA, R_T1CL, R_T1CH, R_T1LL, R_T1LH,
    R_T2CL, R_T2CH, R_SR, R_ACR, R_PCR, R_IFR, R_IER, R_ORA_NH,
};

#define ACR_T1_FREE     0x40
#define ACR_T1_PB7      0x80
#define ACR_SR_MODE(a)  (((a) >> 2) & 7)
#define SR_IN_EXT       3
#define SR_OUT_EXT      7

via_t via1;

void via_init(via_t *v, const via_wiring_t *wiring, int ev_t1, int ev_t2)
{
    memset(v, 0, sizeof(*v));
    v->wiring = wiring;
    v->ev_t1 = ev_t1;
    v->ev_t2 = ev_t2;
    v->ca1 = v->ca2 = v->cb1 = v->cb2 = true;
    v->pb7 = true;
}

/* Effective output levels: inputs (DDR 0) read as pulled high; PB7 is
 * T1's when the ACR says so
 */
uint8_t via_port_a(const via_t *v)
{
    return (v->ora & v->ddra) | ~v->ddra;
}

uint8_t via_port_b(const via_t *v)
{
    uint8_t b = (v->orb & v->ddrb) | ~v->ddrb;
    if (v->acr & ACR_T1_PB7) {
        b = (b & 0x7F) | (v->pb7 ? 0x80 : 0);
    }
    return b;
}

static void update_irq(via_t *v)
{
    bool irq = (v->ifr & v->ier & 0x7F) != 0;
    if (irq != v->irq) {
        v->irq = irq;
        v->wiring->irq_changed(irq);
    }
}

void via_set_flag(via_t *v, int flag)
{
    v->ifr |= flag;
    update_irq(v);
}

static void clear_flag(via_t *v, int flag)
{
    v->ifr &= ~flag;
    update_irq(v);
}

/* ---- Control lines: inputs with the active edge from the PCR ---- */

void via_ca1(via_t *v, bool level)
{
    bool rising = (v->pcr & 0x01) != 0;
    if (level != v->ca1 && level == rising) {
        via_set_flag(v, VIA_IF_CA1);
    }
    v->ca1 = level;
}

void via_ca2(via_t *v, bool level)
{
    bool rising = (v->pcr & 0x04) != 0;
    if (!(v->pcr & 0x08) && level != v->ca2 && level == rising) {
        via_set_flag(v, VIA_IF_CA2);
    }
    v->ca2 = level;
}

void via_cb1(via_t *v, bool level)
{
    bool rising = (v->pcr & 0x10) != 0;
    if (level != v->cb1 && level == rising) {
        via_set_flag(v, VIA_IF_CB1);
    }
    v->cb1 = level;
}

void via_cb2(via_t *v, bool level)
{
    bool rising = (v->pcr & 0x40) != 0;
    if (!(v->pcr & 0x80) && level != v->cb2 && level == rising) {
        via_set_flag(v, VIA_IF_CB2);
    }
    v->cb2 = level;
}

/* ---- Timers ---- */

static uint16_t counter(uint64_t start, uint16_t value)
{
    uint64_t ticks = (mac_now() - start) / VIA_DIVIDER;
    return (uint16_t)(value - ticks);
}

static void schedule_t1(via_t *v)
{
    if (v->t1_armed || (v->acr & ACR_T1_FREE)) {
        mac_schedule(v->ev_t1, v->t1_start + ((uint64_t)v->t1_value + 1) * VIA_DIVIDER);
    } else {
        mac_schedule(v->ev_t1, EV_NEVER);
    }
}

static void schedule_t2(via_t *v)
{
    mac_schedule(v->ev_t2, v->t2_armed ?
                 v->t2_start + ((uint64_t)v->t2_value + 1) * VIA_DIVIDER : EV_NEVER);
}

static void set_pb7(via_t *v, bool level)
{
    if (level == v->pb7) {
        return;
    }
    uint8_t before = via_port_b(v);
    v->pb7 = level;
    uint8_t after = via_port_b(v);
    if (before != after) {
        v->wiring->port_b_changed(before, after);
    }
}

void via_timer_event(via_t *v, int event)
{
    uint64_t when = mac.events[event];
    if (event == v->ev_t1) {
        if (v->acr & ACR_T1_FREE) {
            /* Reload from the latches, one more tick for the reload */
            v->t1_start = when + VIA_DIVIDER;
            v->t1_value = (v->t1lh << 8) | v->t1ll;
            via_set_flag(v, VIA_IF_T1);
            set_pb7(v, !v->pb7);
        } else if (v->t1_armed) {
            v->t1_armed = false;
            via_set_flag(v, VIA_IF_T1);
            set_pb7(v, true);
        }
        schedule_t1(v);
    } else {
        if (v->t2_armed) {
            v->t2_armed = false;
            via_set_flag(v, VIA_IF_T2);
        }
        schedule_t2(v);
    }
}

/* ---- Shift register (device side) ---- */

bool via_sr_shifting_in(const via_t *v)
{
    return ACR_SR_MODE(v->acr) == SR_IN_EXT;
}

bool via_sr_shifting_out(const via_t *v)
{
    return ACR_SR_MODE(v->acr) == SR_OUT_EXT;
}

void via_sr_receive(via_t *v, uint8_t value)
{
    if (via_sr_shifting_in(v)) {
        v->sr = value;
        via_set_flag(v, VIA_IF_SR | VIA_IF_CB1);
    }
}

uint8_t via_sr_send(via_t *v)
{
    if (!via_sr_shifting_out(v)) {
        return 0;
    }
    via_set_flag(v, VIA_IF_SR | VIA_IF_CB1);
    return v->sr;
}

/* ---- Registers ---- */

static void port_a_write(via_t *v, uint8_t ora, uint8_t ddra)
{
    uint8_t before = via_port_a(v);
    v->ora = ora;
    v->ddra = ddra;
    uint8_t after = via_port_a(v);
    if (before != after && v->wiring->port_a_changed) {
        v->wiring->port_a_changed(before, after);
    }
}

/* Port B writes always reach the wiring, changed or not (the II's RTC
 * follows the data direction too)
 */
static void port_b_write(via_t *v, uint8_t orb, uint8_t ddrb)
{
    uint8_t before = via_port_b(v);
    v->orb = orb;
    v->ddrb = ddrb;
    v->wiring->port_b_changed(before, via_port_b(v));
}

void via_reset(via_t *v)
{
    uint8_t a = via_port_a(v), b = via_port_b(v);
    const via_wiring_t *wiring = v->wiring;
    int ev_t1 = v->ev_t1, ev_t2 = v->ev_t2;
    bool ca1 = v->ca1, ca2 = v->ca2, cb1 = v->cb1, cb2 = v->cb2;
    via_init(v, wiring, ev_t1, ev_t2);
    v->ca1 = ca1;
    v->ca2 = ca2;
    v->cb1 = cb1;
    v->cb2 = cb2;
    wiring->irq_changed(false);
    mac_schedule(ev_t1, EV_NEVER);
    mac_schedule(ev_t2, EV_NEVER);
    /* Everything is an input now (reads as high) */
    if (wiring->port_a_changed && a != via_port_a(v)) {
        wiring->port_a_changed(a, via_port_a(v));
    }
    if (b != via_port_b(v)) {
        wiring->port_b_changed(b, via_port_b(v));
    }
}

/* Port accesses clear CA1/CB1, and CA2/CB2 unless they are independent
 * interrupts (or outputs)
 */
static void port_a_access(via_t *v)
{
    clear_flag(v, ((v->pcr & 0x0A) == 0 ? VIA_IF_CA2 : 0) | VIA_IF_CA1);
}

static void port_b_access(via_t *v)
{
    clear_flag(v, ((v->pcr & 0xA0) == 0 ? VIA_IF_CB2 : 0) | VIA_IF_CB1);
}

uint8_t via_read(via_t *v, uint32_t address)
{
    switch ((address >> 9) & 0xF) {
    case R_ORB:
        port_b_access(v);
        return (via_port_b(v) & v->ddrb) | (v->wiring->port_b_inputs() & ~v->ddrb);
    case R_ORA:
        port_a_access(v);
        /* fall through */
    case R_ORA_NH:
        return (v->ora & v->ddra) | (v->wiring->port_a_inputs() & ~v->ddra);
    case R_DDRB:
        return v->ddrb;
    case R_DDRA:
        return v->ddra;
    case R_T1CL:
        clear_flag(v, VIA_IF_T1);
        return counter(v->t1_start, v->t1_value) & 0xFF;
    case R_T1CH:
        return counter(v->t1_start, v->t1_value) >> 8;
    case R_T1LL:
        return v->t1ll;
    case R_T1LH:
        return v->t1lh;
    case R_T2CL:
        clear_flag(v, VIA_IF_T2);
        return counter(v->t2_start, v->t2_value) & 0xFF;
    case R_T2CH:
        return counter(v->t2_start, v->t2_value) >> 8;
    case R_SR:
        clear_flag(v, VIA_IF_SR);
        return v->sr;
    case R_ACR:
        return v->acr;
    case R_PCR:
        return v->pcr;
    case R_IFR:
        return v->ifr | ((v->ifr & v->ier & 0x7F) ? 0x80 : 0);
    case R_IER:
        return v->ier | 0x80;
    }
    return 0;
}

void via_write(via_t *v, uint32_t address, uint8_t value)
{
    switch ((address >> 9) & 0xF) {
    case R_ORB:
        port_b_access(v);
        port_b_write(v, value, v->ddrb);
        break;
    case R_ORA:
        port_a_access(v);
        port_a_write(v, value, v->ddra);
        break;
    case R_ORA_NH:
        port_a_write(v, value, v->ddra);
        break;
    case R_DDRB:
        port_b_write(v, v->orb, value);
        break;
    case R_DDRA:
        port_a_write(v, v->ora, value);
        break;
    case R_T1CL:
    case R_T1LL:
        v->t1ll = value;
        break;
    case R_T1CH:
        v->t1lh = value;
        v->t1_value = (value << 8) | v->t1ll;
        v->t1_start = mac_now();
        v->t1_armed = true;
        clear_flag(v, VIA_IF_T1);
        if (!(v->acr & ACR_T1_FREE)) {
            set_pb7(v, false);      /* one-shot: PB7 low until it runs out */
        }
        schedule_t1(v);
        break;
    case R_T1LH:
        v->t1lh = value;
        clear_flag(v, VIA_IF_T1);
        break;
    case R_T2CL:
        v->t2ll = value;
        break;
    case R_T2CH:
        v->t2_value = (value << 8) | v->t2ll;
        v->t2_start = mac_now();
        v->t2_armed = true;
        clear_flag(v, VIA_IF_T2);
        schedule_t2(v);
        break;
    case R_SR:
        v->sr = value;
        clear_flag(v, VIA_IF_SR);
        if (v->wiring->sr_written) {
            v->wiring->sr_written();
        }
        break;
    case R_ACR: {
        uint8_t old = v->acr, before = via_port_b(v);
        if ((old ^ value) & ACR_T1_FREE) {
            /* Keep the count, change the mode */
            v->t1_value = counter(v->t1_start, v->t1_value);
            v->t1_start = mac_now();
        }
        v->acr = value;
        if (ACR_SR_MODE(value) == 0) {
            clear_flag(v, VIA_IF_SR);
        }
        schedule_t1(v);
        if (before != via_port_b(v)) {
            v->wiring->port_b_changed(before, via_port_b(v));
        }
        break;
    }
    case R_PCR:
        v->pcr = value;
        break;
    case R_IFR:
        v->ifr &= ~value & 0x7F;
        update_irq(v);
        break;
    case R_IER:
        if (value & 0x80) {
            v->ier |= value & 0x7F;
        } else {
            v->ier &= ~value & 0x7F;
        }
        update_irq(v);
        break;
    }
}
