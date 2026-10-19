/*
 * Egret, the LC II's and LC III's system microcontroller (a 68HC05 with its own firmware),
 * emulated at the level of its packets: the ADB keyboard and mouse (with
 * autopoll), the real-time clock, parameter RAM, restart and power off.
 *
 * It talks to the Mac through VIA1's shift register, which Egret clocks
 * (CB1), and three handshake lines:
 *
 *   PB5 (out)  system session: the Mac sends a command or reads a packet
 *   PB4 (out)  VIA full: the Mac has put the next byte in the SR, or took one
 *   PB3 (in)   transceiver session, active low: Egret has a packet
 *
 * Mac to Egret: the Mac puts the SR in shift-out mode, writes the first
 * byte and raises system session and VIA full; Egret clocks it out (SR
 * interrupt). For each further byte the Mac writes the SR and pulses VIA
 * full (low, then high). Dropping system session ends the command, and
 * Egret answers with a packet.
 * Egret to Mac: Egret asserts transceiver session and clocks in an
 * attention byte (SR interrupt). The Mac raises system session; Egret
 * clocks in the packet's first byte. The Mac raises VIA full, reads the
 * SR and drops VIA full for the next byte. Egret drops transceiver session
 * with the last byte (the Mac checks it after each one), and the Mac drops
 * system session.
 *
 * Commands are [type][command][data]: type 0 ADB (the ADB command byte),
 * type 1 pseudo-command. Packets to the Mac are [type][flags][command]
 * [data]: type 0 ADB (flags $40 autopoll data, $02 no device answered),
 * 1 a pseudo-command's reply, 2 an error.
 *
 * The handshake as the LC ROMs' own Egret code does it (Linux's
 * via-cuda.c in Egret mode agrees); command numbers as Linux's and QEMU's
 * Cuda (the same as Egret's).
 */
#include <string.h>
#include <time.h>
#include "mac_internal.h"

#define DELAY           2500        /* cycles (160 us) for each step Egret takes */
#define MAX_CMD         32
#define MAX_PACKET      40

enum { E_IDLE, E_RECEIVE, E_ATTENTION, E_SEND };
enum { ACT_NONE, ACT_TAKE, ACT_ATTENTION, ACT_GIVE };

static struct {
    int state;
    int action;                     /* what EV_EGRET does */
    bool sys, full;                 /* the Mac's lines as last seen */
    bool xcvr;                      /* transceiver session asserted (PB3 low) */
    bool attention_sent;            /* the packet's attention byte is in the Mac's SR */
    bool sr_written;                /* the Mac put a byte in the SR */
    uint8_t cmd[MAX_CMD];
    int cmd_len;
    uint8_t packet[MAX_PACKET];
    int packet_len, packet_pos;
    bool packet_ready;              /* waiting to go out */
    bool autopoll;
    uint16_t devices;               /* autopoll device list (bit n = address n) */
    uint8_t autopoll_ms;
    uint8_t tick_mode;              /* $1B: one-second tick packets (0 off) */
    uint64_t next_tick;
    bool tick_due;
    int64_t time_offset;            /* Mac time minus host time, seconds */
} eg;

#ifdef ESP_PLATFORM
#define EGRET_LOG(...)
#else
#include <stdlib.h>
static int s_log = -1;
#define EGRET_LOG(...)  do { if (s_log < 0) s_log = getenv("MAC_EGRET") != NULL; \
                             if (s_log) printf("egret f%u: ", (unsigned)mac.frames), printf(__VA_ARGS__); } while (0)
#endif

/* ---- Clock: the host's local time; a time the Mac sets is an offset ---- */

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Seconds since 1904-01-01 in local time */
uint32_t host_mac_time(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int64_t days = days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) -
                   days_from_civil(1904, 1, 1);
    return (uint32_t)(days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
}

uint32_t egret_mac_time(void)
{
    return (uint32_t)(host_mac_time() + eg.time_offset);
}

/* ---- Parameter RAM ---- */

/* As a freshly set up Mac has it (../esp32mac's defaults, as Mini vMac's):
 * valid, both serial ports asynchronous (no AppleTalk)
 */
void mac_pram_defaults(uint8_t *pram, mac_model_t model)
{
    memset(pram, 0, MAC_PRAM_SIZE);
    pram[0x10] = 0xA8;                  /* valid */
    pram[0x13] = 0x22;                  /* serial ports A and B: async */
    pram[0x14] = 204;                   /* port A, B configuration */
    pram[0x15] = 10;
    pram[0x16] = 204;
    pram[0x17] = 10;
    pram[0x1D] = 2;                     /* application font: Geneva */
    pram[0x1E] = 0x63;                  /* key repeat delay, rate */
    pram[0x08] = 0x03;                  /* speaker volume */
    pram[0x09] = 0x88;                  /* caret blink, double-click time */
    pram[0x0A] = 0x04;                  /* disk cache 128 KB */
    pram[0x0B] = 0x0C;                  /* menu blink 3 */
    pram[0x0C] = 'N';                   /* extended PRAM valid */
    pram[0x0D] = 'u';
    pram[0x0E] = 'M';
    pram[0x0F] = 'c';
    pram[0x01] = 0x80;
    pram[0x02] = 0x4F;
    mac_pram_monitor(pram, model, MAC_MONITOR_13);
    pram[0x78] = 0x00;                  /* startup disk: none chosen */
    pram[0x79] = 0x01;
    pram[0x7A] = 0xFF;
    pram[0x7B] = 0xFE;
}

/* The LC II's video slot PRAM, as the ROM writes it for the monitor but in
 * 256 colours ($83; $80 is black and white): the built-in video (board
 * $0029, the monitor's sense at $5A: 6 the 13", 2 the 12"). The ROM keeps
 * it while the board and the monitor match; for another monitor it would
 * start in black and white: then this one's entry, in colour. The LC III's
 * ROM starts either monitor in 256 colours by itself.
 */
void mac_pram_monitor(uint8_t *pram, mac_model_t model, mac_monitor_t monitor)
{
    if (model != MAC_MODEL_LC2) {
        return;
    }
    bool m12 = monitor == MAC_MONITOR_12;
    uint8_t sense = m12 ? 0x02 : 0x06;
    if (pram[0x57] != 0x29 || pram[0x5A] != sense) {
        pram[0x56] = 0x00;
        pram[0x57] = 0x29;
        pram[0x58] = 0x83;
        pram[0x59] = m12 ? 0xA2 : 0xA6;
        pram[0x5A] = sense;
    }
}

static void pram_write(uint8_t address, uint8_t value)
{
    if (mac.pram[address] != value) {
        mac.pram[address] = value;
        mac.pram_dirty = true;
    }
}

/* ---- Packets ---- */

static void act(int action, uint64_t delay)
{
    eg.action = action;
    mac_schedule(EV_EGRET, action == ACT_NONE ? EV_NEVER : mac_now() + delay);
}

/* A packet for the Mac: transceiver session now, the attention byte soon */
static void send_packet(void)
{
    eg.packet_ready = false;
    eg.packet_pos = 0;
    eg.attention_sent = false;
    eg.xcvr = true;
    eg.state = E_ATTENTION;
    act(ACT_ATTENTION, DELAY);
}

static void packet(uint8_t type, uint8_t flags, uint8_t command, const uint8_t *data, int len)
{
    if (len > MAX_PACKET - 3) {
        len = MAX_PACKET - 3;
    }
    eg.packet[0] = type;
    eg.packet[1] = flags;
    eg.packet[2] = command;
    if (len > 0) {
        memcpy(eg.packet + 3, data, len);
    }
    eg.packet_len = 3 + len;
    eg.packet_ready = true;
}

static void pseudo_command(uint8_t command, const uint8_t *data, int len)
{
    uint8_t out[MAX_PACKET];
    int n = 0;
    switch (command) {
    case 0x01:      /* autopoll on / off */
        eg.autopoll = len > 0 && data[0] != 0;
        break;
    case 0x02: {    /* read Egret's memory: parameter RAM is at $0100-$01FF */
        uint16_t a = len >= 2 ? (data[0] << 8 | data[1]) : 0;
        for (; n < MAX_PACKET - 3; n++, a++) {
            out[n] = (a >> 8) == 1 ? mac.pram[a & 0xFF] : 0;
        }
        break;
    }
    case 0x08: {    /* write Egret's memory */
        uint16_t a = len >= 2 ? (data[0] << 8 | data[1]) : 0;
        for (int i = 2; i < len; i++, a++) {
            if ((a >> 8) == 1) {
                pram_write(a & 0xFF, data[i]);
            }
        }
        break;
    }
    case 0x03: {    /* read the clock */
        uint32_t t = egret_mac_time();
        out[0] = t >> 24;
        out[1] = t >> 16;
        out[2] = t >> 8;
        out[3] = t;
        n = 4;
        break;
    }
    case 0x07: {    /* read parameter RAM from an address on */
        uint16_t a = len >= 2 ? (data[0] << 8 | data[1]) : 0;
        for (; n < MAX_PACKET - 3 && a + n < MAC_PRAM_SIZE; n++) {
            out[n] = mac.pram[a + n];
        }
        break;
    }
    case 0x09:      /* set the clock */
        if (len >= 4) {
            uint32_t t = (uint32_t)data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
            eg.time_offset = (int64_t)t - host_mac_time();
        }
        break;
    case 0x0A:      /* power off */
        mac_power_off();
        break;
    case 0x0C: {    /* write parameter RAM */
        uint16_t a = len >= 2 ? (data[0] << 8 | data[1]) : 0;
        for (int i = 2; i < len && a < MAC_PRAM_SIZE; i++, a++) {
            pram_write(a, data[i]);
        }
        break;
    }
    case 0x11:      /* restart */
        mac_restart();
        break;
    case 0x14:      /* autopoll rate (ms) */
        if (len > 0) {
            eg.autopoll_ms = data[0];
        }
        break;
    case 0x16:      /* ... read it back */
        out[0] = eg.autopoll_ms;
        n = 1;
        break;
    case 0x19:      /* autopoll device list */
        if (len >= 2) {
            eg.devices = data[0] << 8 | data[1];
        }
        break;
    case 0x1A:
        out[0] = eg.devices >> 8;
        out[1] = eg.devices & 0xFF;
        n = 2;
        break;
    case 0x1B:      /* one-second ticks: the OS's one-second interrupt */
        eg.tick_mode = len > 0 ? data[0] : 0;
        eg.next_tick = mac_now() + MAC_CPU_HZ;
        break;
    case 0x0E:      /* the sound chip's (DFAC) settings */
    case 0x12:      /* interrupt level */
    case 0x13:      /* file server flag */
    default:
        break;
    }
    packet(1, 0, command, out, n);
}

static void command_done(void)
{
    eg.state = E_IDLE;
    if (eg.cmd_len < 2) {
        EGRET_LOG("short command (%d bytes)\n", eg.cmd_len);
        return;
    }
#ifndef ESP_PLATFORM
    char text[3 * MAX_CMD + 1];
    for (int i = 0; i < eg.cmd_len; i++) {
        snprintf(text + 3 * i, 4, " %02x", eg.cmd[i]);
    }
    EGRET_LOG("command%s\n", text);
#endif
    uint8_t type = eg.cmd[0], command = eg.cmd[1];
    if (type == 0) {
        uint8_t out[8];
        int n = adb_command(command, eg.cmd + 2, eg.cmd_len - 2, out);
        packet(0, n < 0 ? 0x02 : 0x00, command, out, n < 0 ? 0 : n);
    } else if (type == 1) {
        pseudo_command(command, eg.cmd + 2, eg.cmd_len - 2);
    } else {
        uint8_t out[2] = { type, command };
        packet(2, 1, 0, out, 2);
    }
    send_packet();
}

/* ---- The handshake ---- */

void egret_lines(bool sys, bool full)
{
    bool sys_rose = sys && !eg.sys, sys_fell = !sys && eg.sys;
    bool full_rose = full && !eg.full, full_fell = !full && eg.full;
    EGRET_LOG("%llu lines sys %d full %d state %d sr %s\n", (unsigned long long)(mac_now() - mac.frame_start),
              sys, full, eg.state, via_sr_shifting_out(&via1) ? "out" : via_sr_shifting_in(&via1) ? "in" : "-");
    eg.sys = sys;
    eg.full = full;
    if (sys_rose) {
        if (eg.state == E_ATTENTION && eg.attention_sent && !via_sr_shifting_out(&via1)) {
            /* The Mac takes the packet */
            eg.state = E_SEND;
            act(ACT_GIVE, DELAY);
        } else if (eg.state == E_ATTENTION) {
            /* A command crossing a packet whose attention byte is still to
             * come (the ROM checked transceiver session just before Egret
             * raised it, and puts the SR in shift-out mode after raising
             * system session): Egret takes the first byte, whose interrupt
             * the Mac then counts as the attention byte (it finds
             * transceiver session asserted, drops system session and
             * raises it again to read the packet before it tries again)
             */
            eg.cmd_len = 0;
            act(eg.sr_written && via_sr_shifting_out(&via1) ? ACT_TAKE : ACT_NONE, DELAY);
        } else {
            /* A command: the ROM raises system session first, then puts
             * the SR in shift-out mode, writes the byte and raises VIA
             * full; Linux has the byte in place already. (If a packet
             * waits, the Mac finds transceiver session asserted after the
             * first byte and reads the packet instead.)
             */
            if (eg.state != E_ATTENTION) {
                eg.state = E_RECEIVE;
            }
            eg.cmd_len = 0;
            if (eg.sr_written && via_sr_shifting_out(&via1)) {
                act(ACT_TAKE, DELAY);
            }
        }
        return;
    }
    if (sys_fell) {
        eg.sr_written = false;
        if (eg.state == E_ATTENTION) {
            if (!eg.attention_sent) {
                act(ACT_ATTENTION, DELAY);  /* the Mac gave up before a byte */
            }
        } else if (eg.state == E_RECEIVE) {
            act(ACT_NONE, 0);
            command_done();
        } else if (eg.state == E_SEND) {
            if (eg.packet_pos < eg.packet_len) {
                EGRET_LOG("the Mac stopped the packet after %d of %d bytes\n", eg.packet_pos, eg.packet_len);
            }
            eg.state = E_IDLE;
            eg.xcvr = false;
            act(ACT_NONE, 0);
            if (eg.packet_ready) {
                send_packet();
            }
        }
        return;
    }
    /* The Mac raises VIA full when it has written the next byte, and
     * drops it to ask for the next one (raising it meanwhile to say it got
     * the last)
     */
    if (full_rose && sys && (eg.state == E_RECEIVE || (eg.state == E_ATTENTION && via_sr_shifting_out(&via1)))) {
        act(ACT_TAKE, DELAY);
    }
    if (full_fell && sys && eg.state == E_SEND) {
        act(ACT_GIVE, DELAY);
    }
}

void egret_event(void)
{
    int action = eg.action;
    eg.action = ACT_NONE;
    switch (action) {
    case ACT_TAKE:
        if (eg.sr_written && via_sr_shifting_out(&via1)) {
            eg.sr_written = false;
            uint8_t b = via_sr_send(&via1);
            EGRET_LOG("%llu took %02x\n", (unsigned long long)(mac_now() - mac.frame_start), b);
            if (eg.state == E_RECEIVE && eg.cmd_len < MAX_CMD) {
                eg.cmd[eg.cmd_len++] = b;
            } else if (eg.state == E_ATTENTION) {
                eg.attention_sent = true;   /* a crossing command's byte */
            }
        }
        break;
    case ACT_ATTENTION:
        if (!via_sr_shifting_in(&via1)) {
            act(ACT_ATTENTION, DELAY);      /* the Mac isn't listening yet */
            break;
        }
        EGRET_LOG("%llu attention\n", (unsigned long long)(mac_now() - mac.frame_start));
        eg.attention_sent = true;
        via_sr_receive(&via1, 0x00);
        break;
    case ACT_GIVE:
        if (eg.packet_pos < eg.packet_len) {
            if (eg.packet_pos == eg.packet_len - 1) {
                eg.xcvr = false;            /* this is the last */
            }
            EGRET_LOG("%llu gave %02x%s\n", (unsigned long long)(mac_now() - mac.frame_start),
                      eg.packet[eg.packet_pos], eg.xcvr ? "" : " (last)");
            via_sr_receive(&via1, eg.packet[eg.packet_pos++]);
        }
        break;
    }
}

void egret_sr_written(void)
{
    eg.sr_written = true;
}

bool egret_xcvr_line(void)
{
    return !eg.xcvr;
}

/* Once a frame: a one-second tick (packet type 3, which the ROM hands to
 * the one-second interrupt handler), or autopoll the ADB devices, while
 * the bus is quiet
 */
void egret_frame(void)
{
    if (eg.tick_mode && mac.now >= eg.next_tick) {
        eg.next_tick += MAC_CPU_HZ;
        eg.tick_due = true;
    }
    if (eg.state != E_IDLE || eg.packet_ready || eg.sys) {
        return;
    }
    if (eg.tick_due) {
        eg.tick_due = false;
        packet(3, 0, 0, NULL, 0);
        send_packet();
        return;
    }
    if (!eg.autopoll) {
        return;
    }
    uint8_t command, out[8];
    int n = adb_poll(eg.devices, &command, out);
    if (n > 0) {
        packet(0, 0x40, command, out, n);
        send_packet();
    }
}

void egret_reset(void)
{
    int64_t offset = eg.time_offset;
    memset(&eg, 0, sizeof(eg));
    eg.time_offset = offset;
    eg.autopoll_ms = 11;
}

/* ---- VIA1: PA the machine's ID bits, PB3-PB5 Egret's handshake ---- */

static uint8_t via1_port_a_inputs(void)
{
    return mac.model == MAC_MODEL_LC3 ? 0x80 : 0xD5;   /* as MAME's Sonora and V8 */
}

static uint8_t via1_port_b_inputs(void)
{
    return egret_xcvr_line() ? PB_EGRET_XCVR : 0;
}

static void via1_port_b_changed(uint8_t old_value, uint8_t value)
{
    if ((old_value ^ value) & (PB_EGRET_FULL | PB_EGRET_SYS)) {
        egret_lines((value & PB_EGRET_SYS) != 0, (value & PB_EGRET_FULL) != 0);
    }
}

static void via1_irq(bool irq)
{
    mac.irq1 = irq;
    mac_update_irq();
}

const via_wiring_t egret_via1_wiring = {
    .port_a_inputs = via1_port_a_inputs,
    .port_b_inputs = via1_port_b_inputs,
    .port_b_changed = via1_port_b_changed,
    .sr_written = egret_sr_written,
    .irq_changed = via1_irq,
};
