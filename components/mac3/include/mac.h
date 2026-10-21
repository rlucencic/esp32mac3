/*
 * Macintosh LC II: 68030 at 15.67 MHz, the V8 system ASIC (RAM controller:
 * 4 MB on the board and a SIMM pair, up to 10 MB; VIA, pseudo-VIA, video
 * from its own VRAM through the Ariel colour table, 640 x 480 in 1 to 8
 * bits per pixel or 512 x 384 up to thousands of colours; the ASC's sound
 * FIFO), the Egret microcontroller (ADB keyboard and mouse, real-time
 * clock, parameter RAM). The V8 sees 24 address bits: the 68030's MMU maps
 * the 32-bit address space onto them.
 *
 * Macintosh LC III: 68030 at 25 MHz, the Sonora system ASIC (RAM at 0: 4 MB
 * on the board and a SIMM; VIA, pseudo-VIA, video from 1 MB of VRAM up to
 * thousands of colours at 640 x 480; the EASC-like stereo sound FIFOs),
 * Egret. A full 32-bit physical map.
 *
 * Disk images (any size) go through a paravirtual replacement of the ROM's
 * .Sony driver, as in ../esp32mac.
 *
 * Platform independent: the caller provides all memory and drives the
 * emulation one video frame at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MAC_MODEL_LC2,          /* 512 KB ROM, V8, Egret */
    MAC_MODEL_LC3,          /* 1 MB ROM, Sonora, Egret */
} mac_model_t;

typedef enum {
    MAC_MONITOR_13,         /* 13" RGB: 640 x 480 (LC II up to 256 colours, LC III 32,768) */
    MAC_MONITOR_12,         /* 12" RGB: 512 x 384, up to 32,768 colours */
} mac_monitor_t;

#define MAC_SCREEN_MAX_WIDTH    640
#define MAC_SCREEN_MAX_HEIGHT   480
#define MAC_SCREEN_HZ           60

#define MAC_DRIVES              3       /* Mac drive numbers 1 .. 3 */
#define MAC_PRAM_SIZE           256     /* parameter RAM */
#define MAC_VRAM_SIZE           0x100000    /* video RAM: the LC II's 512 KB, the LC III's 1 MB */
#define MAC_OPCODE_TABLES_SIZE  0x50000

typedef struct {
    uint8_t *ram;           /* ram_size bytes */
    uint32_t ram_size;      /* LC II: 4, 6, 8 or 10 MB (4 MB on the board, a SIMM pair);
                             * LC III: 4, 8, 16 or 20 MB (in one piece at 0: the sizes its
                             * ROM finds that way, as MAME's) */
    uint8_t *vram;          /* MAC_VRAM_SIZE bytes */
    const uint8_t *rom;     /* ROM image as dumped (big-endian) */
    uint32_t rom_size;      /* LC II 512 KB, LC III 1 MB */
    uint8_t *rom_copy;      /* rom_size bytes: working copy (it gets patched);
                             * may be the same buffer as rom */
    mac_monitor_t monitor;

    /* MAC_PRAM_SIZE bytes of parameter RAM, kept by the caller across
     * power-offs. The Mac writes into it; pram_changed is called (from the
     * emulation task) after it did, at most once a frame.
     */
    uint8_t *pram;
    void (*pram_changed)(void);

    /* The Mac ejected the disk in this drive (0-based), from the emulation
     * task: the file is closed already
     */
    void (*disk_ejected)(int drive);

    /* MAC_OPCODE_TABLES_SIZE bytes for copies of the CPU core's opcode
     * tables, or NULL (read-only data in flash is slow on the ESP32)
     */
    void *opcode_tables;
} mac_config_t;

typedef struct {
    uint32_t frames;        /* video frames emulated */
    uint32_t pc;
    uint16_t sr;
    uint32_t ticks;         /* Mac OS Ticks (0 before the OS is up) */
    bool halted;            /* CPU stopped by a double bus fault until a reset */
} mac_status_t;

/* Fill MAC_PRAM_SIZE bytes of parameter RAM with defaults (mac_init does
 * it for parameter RAM that isn't valid)
 */
void mac_pram_defaults(uint8_t *pram, mac_model_t model);

/* The video's parameter RAM for this monitor: kept if it is for it (its
 * depth too), else the monitor's in 256 colours (mac_init does it)
 */
void mac_pram_monitor(uint8_t *pram, mac_model_t model, mac_monitor_t monitor);

/* Name of a known ROM ("Macintosh LC II", "Macintosh LC III") and its model; NULL if the image
 * isn't a ROM this emulator knows how to patch
 */
const char *mac_rom_identify(const uint8_t *rom, uint32_t size, mac_model_t *model);

/* Set up the machine and cold-reset it. False if the ROM is unusable. */
bool mac_init(const mac_config_t *config);

/* Cold reset, as the power switch */
void mac_reset(void);

/* Programmer's interrupt switch (NMI): the debugger, if one is installed */
void mac_interrupt_switch(void);

/* Emulate one video frame (60.15 Hz) */
void mac_run_frame(void);

/* The CPU's speed, in 1/1024 of the model's own (the LC II's 68030 at
 * 15.67 MHz on its 16-bit bus, the LC III's at 25 MHz): the machine's time
 * stays real, the CPU does that much work in it. Between frames.
 */
#define MAC_SPEED_NATIVE        1024
void mac_set_cpu_speed(int speed);
int mac_cpu_speed(void);
float mac_cpu_mhz(void);    /* the speed as the 68030's clock */

void mac_get_status(mac_status_t *status);
mac_model_t mac_model(void);

/* Free RAM as the Mac OS sees it (see ../esp32mac); false before the OS is up */
bool mac_ram_free(uint32_t *free_bytes, uint32_t *system_free);

/* Read Mac memory without side effects (RAM, ROM and VRAM, else 0) */
uint16_t mac_peek16(uint32_t address);

/* ---- Screen ---- */

/* The monitor's picture: 640 x 480 or 512 x 384 */
void mac_screen_size(int *width, int *height);

/* Bits per pixel the Mac drives the screen at now (1, 2, 4, 8 or 16) */
int mac_screen_depth(void);

/* Render screen line y as RGB565, width pixels */
void mac_render_line_rgb565(int y, uint16_t *dst);

/* Changes when the picture changes (video RAM, colour table or mode written) */
uint32_t mac_screen_signature(void);

/* ---- Sound: 16-bit stereo at MAC_AUDIO_HZ (one sample per 704 CPU cycles) ---- */

#define MAC_AUDIO_HZ            22255

void mac_audio_enable(bool enable);
int mac_audio_render(int16_t *dst, int frames);
int mac_audio_pending(void);

/* ---- Disk images: drives 0 .. MAC_DRIVES - 1 (Mac drive numbers 1 ..) ----
 *
 * Raw images (.dsk, .img, .hfv: 800K, 1.4M floppies or hard disks of any
 * size), Disk Copy 4.2 images (.image), partitioned hard disk and CD
 * images, and uncompressed .dmg images. A read-only file is a locked disk. Not while mac_run_frame()
 * runs; the Mac sees the disk inserted within half a second.
 */
bool mac_disk_insert(int drive, const char *path);
void mac_disk_eject(int drive);
const char *mac_disk_path(int drive);      /* NULL when empty */
bool mac_disk_write_protected(int drive);

/* Size in bytes of the disk in the drive, 0 when empty */
uint64_t mac_disk_size(int drive);

/* The free space on the disk in the drive and its volume's size, as its
 * MFS, HFS or HFS+ volume counts them (worked out at insert and a second
 * after the last write); false if not known. Safe to call from another task.
 */
bool mac_disk_space(int drive, uint64_t *free_bytes, uint64_t *size);

/* Make a blank disk image of `bytes` (a multiple of 512) */
bool mac_disk_create(const char *path, uint32_t bytes);

typedef struct {
    int light;              /* drive accessed in the last ~0.3 s, -1 none */
    uint32_t reads[MAC_DRIVES];     /* 512-byte blocks read and written */
    uint32_t writes[MAC_DRIVES];
} mac_disk_status_t;

void mac_disk_status(mac_disk_status_t *status);

/* ---- Keyboard and mouse: safe to call from another task ---- */

/* Mac virtual key code (ADB key code, e.g. 0x00 = A, 0x37 = Command,
 * 0x7B..0x7E = arrows)
 */
void mac_key(uint8_t keycode, bool down);

/* Relative mouse motion (pixels) and the button */
void mac_mouse(int dx, int dy, bool button);

/* Put the pointer at screen position (x, y), e.g. from a touch screen */
void mac_mouse_position(int x, int y, bool button);
