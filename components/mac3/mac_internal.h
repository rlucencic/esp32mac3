/*
 * Shared state of the Macintosh. Not part of the public API.
 *
 * mac.c, mem.c, via.c, pvia.c, egret.c, video.c, asc.c, adb.c, input.c,
 * chips.c, sony.c are shared; lc.c (the LC II's V8) and sonora.c (the LC
 * III's Sonora) are the two machines, behind model_ops_t.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "mac.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_log.h"
#define MAC_HOT             IRAM_ATTR
#define MAC_LOGI(...)       ESP_LOGI("mac", __VA_ARGS__)
#define MAC_LOGW(...)       ESP_LOGW("mac", __VA_ARGS__)
#else
#include <stdio.h>
#define MAC_HOT
#define MAC_LOGI(fmt, ...)  printf("I mac: " fmt "\n", ##__VA_ARGS__)
#define MAC_LOGW(fmt, ...)  printf("W mac: " fmt "\n", ##__VA_ARGS__)
#endif

/* Time is kept in cycles of 31.3344 MHz / 2 (the LC II's CPU clock, the
 * VIA's times 20), in "lines" of 704, one sound sample each (22.255 kHz),
 * 370 to a 60.15 Hz frame. The monitor's vertical blanking (the last 45 of
 * its 525 lines) is the same share of the frame.
 */
#define MAC_CPU_HZ          15667200
#define LINE_CYCLES         704
#define FRAME_LINES         370
#define FRAME_CYCLES        (LINE_CYCLES * FRAME_LINES)
#define VBL_LINE            338         /* vertical blanking from here to the end */
#define VIA_DIVIDER         20          /* VIA clock: CPU / 20 */
#define MB                  0x100000u

/* Low memory globals */
#define LM_TICKS            0x016A
#define LM_MTEMP            0x0828
#define LM_RAWMOUSE         0x082C
#define LM_MOUSE            0x0830
#define LM_CRSRNEW          0x08CE
#define LM_CRSRCOUPLE       0x08CF
#define LM_JIODONE          0x08FC

/* Scheduled events (cycle times) */
enum {
    EV_VIA1_T1,
    EV_VIA1_T2,
    EV_EGRET,           /* Egret acts on the handshake lines */
    EV_ASC,             /* the sound FIFO drains */
    EV_COUNT,
};

#define EV_NEVER            UINT64_MAX

/* What differs between the machines: memory beyond the fast paths, the
 * frame's landmarks, the picture
 */
typedef struct {
    void (*reset)(void);
    void (*frame_start)(void);
    void (*vbl)(bool active);
    uint8_t (*read8)(uint32_t a);           /* a: the address after addr_mask */
    void (*write8)(uint32_t a, uint8_t v);
    uint16_t (*read16)(uint32_t a);         /* even a */
    void (*write16)(uint32_t a, uint16_t v);
    void (*write32)(uint32_t a, uint32_t v);
    /* The emulator's own access (24-bit addresses, no side effects) */
    uint8_t *(*ram_page)(uint32_t a);       /* host memory of the 1 MB page, or NULL */
    uint16_t (*peek16)(uint32_t a);         /* other than RAM */
    void (*screen_size)(int *w, int *h);
    int (*screen_depth)(void);
    void (*render_line)(int y, uint16_t *dst);
    bool (*screen_changed)(void);           /* since the last call */
} model_ops_t;

typedef struct {
    mac_model_t model;
    const model_ops_t *ops;

    /* Memory. RAM, ROM and VRAM are stored with each 16-bit word in host
     * order, so a word read is a plain load and byte n lives at index n ^ 1.
     * Fast paths (mem.c): addresses below ram_window are ram[a]; ROM at
     * rom_base .. + rom_window (repeated every rom_size); VRAM at vram_base
     * .. + vram_window. A window of 0 sends everything there to the slow
     * path (the overlay, a packed colour mode).
     */
    uint8_t *ram;
    uint32_t ram_size;
    uint32_t ram_window;
    uint8_t *rom;
    uint32_t rom_size;
    uint32_t rom_mask;
    uint32_t rom_base, rom_window;
    uint8_t *vram;
    uint32_t vram_size;
    uint32_t vram_base, vram_window;
    bool vram_written;
    bool overlay;                   /* ROM at 0 (power-on) */
    uint32_t addr_mask;             /* what the address decoder sees */
    uint32_t driver_start;          /* the replacement .Sony driver in the ROM */
    uint32_t driver_end;            /* (offsets, native calls only there) */
    uint32_t rom_mac_base;          /* where the ROM is in 24-bit mode */
    mac_monitor_t monitor;
    int cpu_scale;                  /* machine time per CPU core cycle, in 1/256 */
    int cpu_scale_native;           /* at the model's own speed */
    int cpu_speed;                  /* MAC_SPEED_NATIVE = the model's own */
    float cpu_mhz;                  /* the model's own clock */

    /* Time */
    uint64_t now;                   /* CPU cycles, as of the last sync */
    uint64_t frame_start;
    bool cpu_running;               /* inside m68k_execute() */
    int slice;                      /* machine time until the CPU stops */
    uint64_t events[EV_COUNT];

    uint32_t frames;
    bool halted;

    /* Interrupt lines */
    bool irq1;                      /* VIA1 */
    bool irq2;                      /* the pseudo-VIA */
    bool scc_irq;                   /* level 4 */
    bool nmi;

    uint8_t *pram;
    void (*pram_changed)(void);
    bool pram_dirty;
    void (*disk_ejected)(int drive);
} mac_machine_t;

extern mac_machine_t mac;
extern const model_ops_t lc_ops, sonora_ops;

/* Host build: MAC_IO=1 logs every I/O access (MAC_IO=<hex mask> of the
 * 8 KB blocks: bit n = $xF00000 + n * $2000)
 */
#ifdef ESP_PLATFORM
#define IO_LOG(a, write, v)
#else
void io_log(uint32_t a, bool write, uint8_t v);
#define IO_LOG(a, write, v) io_log(a, write, v)
#endif

/* mac.c */
uint64_t mac_now(void);
void mac_schedule(int event, uint64_t when);
void mac_update_irq(void);
void mac_restart(void);             /* a reset asked for: at the end of the frame */
void mac_power_off(void);

/* mem.c: memory access for the emulator itself (no side effects), at the
 * Mac's logical addresses (RAM maps straight on both machines)
 */
uint32_t mac_ptr(uint32_t pointer);  /* a pointer as the Mac means it: 24 bits in 24-bit mode */
uint8_t mac_peek8(uint32_t address);
uint32_t mac_peek32(uint32_t address);
void mac_poke8(uint32_t address, uint8_t value);
void mac_poke16(uint32_t address, uint16_t value);
void mac_poke32(uint32_t address, uint32_t value);
bool mac_ram_range(uint32_t address, uint32_t len);
void mac_ram_write(uint32_t address, const uint8_t *src, uint32_t len);
void mac_ram_read(uint32_t address, uint8_t *dst, uint32_t len);
uint32_t mac_ram_top(void);         /* end of RAM as the Mac sees it */

/* via.c */
typedef struct {
    uint8_t (*port_a_inputs)(void);
    uint8_t (*port_b_inputs)(void);
    void (*port_a_changed)(uint8_t old_value, uint8_t value);   /* may be NULL */
    void (*port_b_changed)(uint8_t old_value, uint8_t value);   /* PB7 from T1 too */
    void (*sr_written)(void);       /* the Mac wrote the shift register; may be NULL */
    void (*irq_changed)(bool irq);
} via_wiring_t;

typedef struct {
    const via_wiring_t *wiring;
    int ev_t1, ev_t2;
    uint8_t ora, orb, ddra, ddrb;
    uint8_t sr, acr, pcr, ifr, ier;
    uint8_t t1ll, t1lh, t2ll;
    uint64_t t1_start;              /* cycle time the counter held t1_value */
    uint16_t t1_value;
    bool t1_armed;                  /* one-shot: an interrupt still due */
    uint64_t t2_start;
    uint16_t t2_value;
    bool t2_armed;
    bool pb7;                       /* T1's PB7 output */
    bool ca1, ca2, cb1, cb2;        /* input levels */
    bool irq;
} via_t;

extern via_t via1;

void via_init(via_t *v, const via_wiring_t *wiring, int ev_t1, int ev_t2);
void via_reset(via_t *v);
uint8_t via_read(via_t *v, uint32_t address);
void via_write(via_t *v, uint32_t address, uint8_t value);
void via_timer_event(via_t *v, int event);
void via_set_flag(via_t *v, int flag);      /* VIA_IF_* */
void via_ca1(via_t *v, bool level);         /* input lines: flags on the PCR's edge */
void via_ca2(via_t *v, bool level);
void via_cb1(via_t *v, bool level);
void via_cb2(via_t *v, bool level);
uint8_t via_port_a(const via_t *v);         /* output levels as the Mac set them */
uint8_t via_port_b(const via_t *v);
/* Shift register, as the external device */
bool via_sr_shifting_in(const via_t *v);    /* ACR: shift in under external clock */
bool via_sr_shifting_out(const via_t *v);   /* ACR: shift out under external clock */
void via_sr_receive(via_t *v, uint8_t value);   /* device -> Mac: SR full, IRQ */
uint8_t via_sr_send(via_t *v);              /* Mac -> device: SR emptied, IRQ */

#define VIA_IF_CA2      0x01
#define VIA_IF_CA1      0x02
#define VIA_IF_SR       0x04
#define VIA_IF_CB2      0x08
#define VIA_IF_CB1      0x10
#define VIA_IF_T2       0x20
#define VIA_IF_T1       0x40

/* VIA1's port B: Egret's handshake (both machines) */
#define PB_EGRET_XCVR   0x08        /* in: transceiver session, active low (TREQ) */
#define PB_EGRET_FULL   0x10        /* out: VIA full, active high (TACK) */
#define PB_EGRET_SYS    0x20        /* out: system session, active high (TIP) */
extern const via_wiring_t egret_via1_wiring;    /* egret.c: VIA1, the same on both machines */
void lc_init(void);
void sonora_init(void);

/* pvia.c: the pseudo-VIA (slot and sound interrupts; on the V8 also the RAM
 * and video configuration)
 */
void pvia_reset(bool sonora);
uint8_t pvia_read(uint32_t address);
void pvia_write(uint32_t address, uint8_t value);
void pvia_vbl(bool active);
void pvia_asc_irq(bool active);
uint8_t pvia_video_config(void);    /* register $10: bits 0-2 depth */
void lc_set_ram_config(uint8_t config);
void lc_set_24bit(bool on);

/* egret.c */
void egret_reset(void);
void egret_lines(bool sys_session, bool via_full);
bool egret_xcvr_line(void);         /* PB3 level: false = session (asserted) */
void egret_event(void);
void egret_frame(void);
void egret_sr_written(void);
uint32_t egret_mac_time(void);
uint32_t host_mac_time(void);       /* seconds since 1904 in local time */

/* video.c: the built-in video, the V8's (the Ariel colour table, the mode
 * in the pseudo-VIA) or Sonora's (its own mode registers and colour table)
 */
void video_reset(bool sonora);
uint8_t ariel_read(uint32_t address);
void ariel_write(uint32_t address, uint8_t value);
uint8_t sonora_vctrl_read(uint32_t address);
void sonora_vctrl_write(uint32_t address, uint8_t value);
bool sonora_video_on(void);         /* a mode set: vertical blanking interrupts */
void video_mode_changed(void);
void video_screen_size(int *w, int *h);
int video_screen_depth(void);
void video_render_line(int y, uint16_t *dst);
bool video_screen_changed(void);

/* adb.c: ADB keyboard (address 2) and mouse (3) */
void adb_reset(void);
/* An ADB command (aaaa cc rr) with listen data: the talk reply in out
 * (up to 8 bytes) and its length; -1 when no device answered
 */
int adb_command(uint8_t command, const uint8_t *data, int len, uint8_t *out);
/* Autopoll: the device with something to report, its talk command and data */
int adb_poll(uint16_t devices, uint8_t *command, uint8_t *out);
bool adb_anything_to_report(void);
bool adb_mouse_active(void);

/* input.c: events from other tasks */
enum { INPUT_KEY, INPUT_MOUSE, INPUT_MOUSE_ABS };
typedef struct {
    uint8_t type;
    uint8_t code;                   /* key code */
    uint8_t value;                  /* key down / mouse button */
    int16_t dx, dy;                 /* motion, or position */
} input_event_t;
bool input_next_key(uint8_t *code, bool *down);
bool input_key_waiting(void);
void input_reset(void);
void mouse_frame(void);             /* once a frame: take input, move the pointer */
bool mouse_button(void);
int adb_mouse_delta(int *dx, int *dy);      /* motion owed to the ADB mouse */
void adb_mouse_took(int dx, int dy);        /* ... and the part it reported */

/* asc.c: the Apple Sound Chip, the V8's (LC II) or Sonora's (LC III) */
void asc_reset(bool sonora);
uint8_t asc_read(uint32_t address);
void asc_write(uint32_t address, uint8_t value);
void asc_event(void);
void asc_frame_end(void);
void asc_irq_changed(bool active);  /* in the machine: where the line goes */

/* chips.c */
void scc_reset(void);
uint8_t scc_read(uint32_t address);
void scc_write(uint32_t address, uint8_t value);
void swim_reset(void);
uint8_t swim_access(uint32_t address, bool write, uint8_t value);
void scsi_reset(void);
uint8_t scsi_read(uint32_t address);
void scsi_write(uint32_t address, uint8_t value);

/* sony.c: the disk driver */
void sony_reset(void);
bool sony_install(uint8_t *rom, uint32_t rom_size, uint32_t offset, uint32_t rom_mac_base);
int sony_native(uint16_t function);
void sony_frame(void);

/* rom.c */
bool rom_prepare(const uint8_t *image, uint32_t size);
