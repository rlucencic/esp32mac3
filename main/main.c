/*
 * Macintosh LC II and LC III emulator on the M5Stack Tab5.
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, setup screen, web
 * page, console) is the shared runtime in ../esp32emu; this file plugs the
 * Mac into it: memory, ROM, settings, keyboard map, touch, disks, status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "emu/console.h"
#include "emu/emu.h"
#include "mac.h"
#include "rom_store.h"
#include "settings.h"

static const char *TAG = "main";

static mac_config_t s_cfg;
static uint8_t s_pram[MAC_PRAM_SIZE];
static int s_pram_model = -1;       /* whose parameter RAM s_pram holds */
static volatile bool s_pram_dirty;
static const char *s_rom_name;

/* ---- Memory ----
 *
 * The CPU core's opcode tables, video RAM, the ROM copy and the Mac's RAM
 * (up to 20 MB in one block), first thing at power-on, before the screen,
 * WiFi and Bluetooth fragment PSRAM. The rest of the board needs ~1.8 MB
 * of it at most (screen buffer, WiFi, Bluetooth, setup screen).
 */
#define PSRAM_FOR_THE_REST      (1792 * 1024)
#define MB                      (1024 * 1024)
#define RAM_CHOICES             4
#define RAM_MAX_MB              20

/* The sizes each model takes (LC II: the board's 4 MB and a SIMM pair; LC
 * III: in one piece, the sizes its ROM finds that way), and their buttons
 */
static const uint16_t ram_sizes[2][RAM_CHOICES] = { { 4, 6, 8, 10 }, { 4, 8, 16, 20 } };
static const char *const ram_names_lc2[] = { "4 MB", "6 MB", "8 MB", "10 MB", "" };
static const char *const ram_names_lc3[] = { "4 MB", "8 MB", "16 MB", "20 MB", "" };

static uint16_t s_ram_limit_mb = 4;

/* The biggest size of the model's at or below the setting (and what fits) */
static uint32_t ram_wanted(void)
{
    const uint16_t *sizes = ram_sizes[settings.model];
    uint16_t mb = sizes[0];
    for (int i = 0; i < RAM_CHOICES && sizes[i]; i++) {
        if (sizes[i] <= settings.ram_mb && sizes[i] <= s_ram_limit_mb) {
            mb = sizes[i];
        }
    }
    return mb * MB;
}

static void load_pram(void)
{
    if (s_pram_model != settings.model) {
        s_pram_model = settings.model;
        if (!settings_load_pram(settings.model, s_pram)) {
            mac_pram_defaults(s_pram, settings.model);
        }
    }
}

static void reserve_memory(void)
{
    settings_load();
    load_pram();
    s_cfg.opcode_tables = heap_caps_malloc(MAC_OPCODE_TABLES_SIZE, MALLOC_CAP_SPIRAM);
    s_cfg.vram = heap_caps_malloc(MAC_VRAM_SIZE, MALLOC_CAP_SPIRAM);
    s_cfg.rom_copy = heap_caps_malloc(ROM_MAX_SIZE, MALLOC_CAP_SPIRAM);

    size_t room = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    room = room > PSRAM_FOR_THE_REST ? room - PSRAM_FOR_THE_REST : 0;
    s_ram_limit_mb = room / MB > RAM_MAX_MB ? RAM_MAX_MB : room / MB;
    ESP_LOGI(TAG, "PSRAM: %u KB in the largest block, room for %u MB of Mac RAM",
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024), (unsigned)s_ram_limit_mb);
    uint32_t ram_size = ram_wanted();
    if (ram_size / MB != settings.ram_mb) {
        ESP_LOGW(TAG, "%u MB of Mac RAM wanted, using %u MB", (unsigned)settings.ram_mb,
                 (unsigned)(ram_size / MB));
    }
    s_cfg.ram = heap_caps_malloc(ram_size, MALLOC_CAP_SPIRAM);
    s_cfg.ram_size = ram_size;
}

/* ---- Parameter RAM: saved from a core 0 task, not the emulation ---- */

static void pram_changed(void)
{
    s_pram_dirty = true;
}

static void every_second(void)
{
    if (s_pram_dirty) {
        s_pram_dirty = false;
        settings_save_pram(s_pram_model, s_pram);
    }
}

static void disk_ejected(int drive)
{
    disks_ejected(drive);
}

/* ---- Starting ---- */

static bool start(void)
{
    uint32_t rom_size;
    bool lc3 = settings.model == MAC_MODEL_LC3;
    if (s_cfg.rom_copy == NULL || !rom_store_load(settings.model, s_cfg.rom_copy, &rom_size, &s_rom_name)) {
        emu_set_message(lc3 ? "No Macintosh LC III ROM (1 MB): put it in roms/lc3.rom and flash with idf.py "
                              "flash, or copy it into the \"roms\" folder on the SD card."
                            : "No Macintosh LC II ROM (512 KB): put it in roms/lc2.rom and flash with idf.py "
                              "flash, or copy it into the \"roms\" folder on the SD card.");
        return false;
    }
    if (s_pram_dirty) {
        s_pram_dirty = false;
        settings_save_pram(s_pram_model, s_pram);
    }
    load_pram();
    uint32_t ram_size = ram_wanted();
    if (s_cfg.ram == NULL || s_cfg.ram_size != ram_size) {
        free(s_cfg.ram);
        s_cfg.ram = heap_caps_malloc(ram_size, MALLOC_CAP_SPIRAM);
        if (s_cfg.ram == NULL) {
            /* PSRAM is in pieces by now: restart the board, which takes the
             * memory first and goes straight into the Mac (the setting is saved)
             */
            ESP_LOGW(TAG, "no %u MB block for the Mac RAM now: restarting the board",
                     (unsigned)(ram_size / MB));
            emu_reboot_into_machine();
        }
        s_cfg.ram_size = ram_size;
    }
    s_cfg.rom = s_cfg.rom_copy;     /* patched in place */
    s_cfg.rom_size = rom_size;
    s_cfg.monitor = settings.monitor;
    s_cfg.pram = s_pram;
    s_cfg.pram_changed = pram_changed;
    s_cfg.disk_ejected = disk_ejected;
    if (!mac_init(&s_cfg)) {
        emu_set_message("The ROM could not be prepared: see the log.");
        return false;
    }
    return true;
}

/* ---- The 68030's speed ----
 *
 * The model's own (the LC II's 15.67 MHz, the LC III's 25 MHz) when the
 * board keeps up, else as fast as the frames still come in time: the
 * emulation taking up to SPEED_SHARE of each frame's period on core 1 (as
 * esp32ste's Fastest). The Mac's time stays real (sound, clock, mouse); it
 * slows down quickly and speeds up slowly.
 */
#define SPEED_SHARE     85                  /* % of the frame period */

static void run_frame(void)
{
    int64_t start = esp_timer_get_time();
    mac_run_frame();
    int64_t used = esp_timer_get_time() - start;
    int64_t budget = 1000000LL * SPEED_SHARE / (100 * MAC_SCREEN_HZ);
    int64_t speed = mac_cpu_speed();
    /* The speed this frame would have just fitted at, part of the way */
    int64_t fit = used > 0 ? speed * budget / used : MAC_SPEED_NATIVE;
    speed += fit < speed ? (fit - speed) / 2 : (fit - speed) / 8;
    mac_set_cpu_speed((int)speed);
}

static bool halted(void)
{
    mac_status_t s;
    mac_get_status(&s);
    return s.halted;
}

static void screen_mode(emu_screen_t *mode)
{
    mac_screen_size(&mode->width, &mode->height);
    mode->hz = MAC_SCREEN_HZ;
}

/* ---- Keyboard, mouse, touch ---- */

/* HID keyboard usages 0x00-0x67 to Mac key codes (US positions); K() marks
 * an entry (Mac key code 0 is A), 0 = no Mac key
 */
#define K(code)     ((code) | 0x80)
static const uint8_t usage_to_mac[0x68] = {
    [0x04] = K(0x00), [0x05] = K(0x0B), [0x06] = K(0x08), [0x07] = K(0x02),     /* a b c d */
    [0x08] = K(0x0E), [0x09] = K(0x03), [0x0A] = K(0x05), [0x0B] = K(0x04),     /* e f g h */
    [0x0C] = K(0x22), [0x0D] = K(0x26), [0x0E] = K(0x28), [0x0F] = K(0x25),     /* i j k l */
    [0x10] = K(0x2E), [0x11] = K(0x2D), [0x12] = K(0x1F), [0x13] = K(0x23),     /* m n o p */
    [0x14] = K(0x0C), [0x15] = K(0x0F), [0x16] = K(0x01), [0x17] = K(0x11),     /* q r s t */
    [0x18] = K(0x20), [0x19] = K(0x09), [0x1A] = K(0x0D), [0x1B] = K(0x07),     /* u v w x */
    [0x1C] = K(0x10), [0x1D] = K(0x06),                                   /* y z */
    [0x1E] = K(0x12), [0x1F] = K(0x13), [0x20] = K(0x14), [0x21] = K(0x15),     /* 1 2 3 4 */
    [0x22] = K(0x17), [0x23] = K(0x16), [0x24] = K(0x1A), [0x25] = K(0x1C),     /* 5 6 7 8 */
    [0x26] = K(0x19), [0x27] = K(0x1D),                                   /* 9 0 */
    [0x28] = K(0x24),      /* Return */
    [0x29] = K(0x35),      /* Esc */
    [0x2A] = K(0x33),      /* Backspace -> Delete */
    [0x2B] = K(0x30),      /* Tab */
    [0x2C] = K(0x31),      /* Space */
    [0x2D] = K(0x1B),      /* - */
    [0x2E] = K(0x18),      /* = */
    [0x2F] = K(0x21),      /* [ */
    [0x30] = K(0x1E),      /* ] */
    [0x31] = K(0x2A),      /* \ */
    [0x32] = K(0x2A),      /* non-US # */
    [0x33] = K(0x29),      /* ; */
    [0x34] = K(0x27),      /* ' */
    [0x35] = K(0x32),      /* ` */
    [0x36] = K(0x2B),      /* , */
    [0x37] = K(0x2F),      /* . */
    [0x38] = K(0x2C),      /* / */
    [0x39] = K(0x39),      /* Caps Lock */
    [0x3A] = K(0x7A), [0x3B] = K(0x78), [0x3C] = K(0x63), [0x3D] = K(0x76),     /* F1-F4 */
    [0x3E] = K(0x60), [0x3F] = K(0x61), [0x40] = K(0x62), [0x41] = K(0x64),     /* F5-F8 */
    [0x42] = K(0x65), [0x43] = K(0x6D), [0x44] = K(0x67), [0x45] = K(0x6F),     /* F9-F12 */
    [0x46] = K(0x69), [0x47] = K(0x6B), [0x48] = K(0x71),                    /* F13-F15 */
    [0x49] = K(0x72),      /* Insert -> Help */
    [0x4A] = K(0x73),      /* Home */
    [0x4B] = K(0x74),      /* Page Up */
    [0x4C] = K(0x75),      /* Delete -> forward delete */
    [0x4D] = K(0x77),      /* End */
    [0x4E] = K(0x79),      /* Page Down */
    [0x4F] = K(0x7C),      /* Right */
    [0x50] = K(0x7B),      /* Left */
    [0x51] = K(0x7D),      /* Down */
    [0x52] = K(0x7E),      /* Up */
    [0x53] = K(0x47),      /* Num Lock -> Clear */
    [0x54] = K(0x4B),      /* keypad / */
    [0x55] = K(0x43),      /* keypad * */
    [0x56] = K(0x4E),      /* keypad - */
    [0x57] = K(0x45),      /* keypad + */
    [0x58] = K(0x4C),      /* keypad Enter */
    [0x59] = K(0x53), [0x5A] = K(0x54), [0x5B] = K(0x55),                    /* keypad 1 2 3 */
    [0x5C] = K(0x56), [0x5D] = K(0x57), [0x5E] = K(0x58),                    /* keypad 4 5 6 */
    [0x5F] = K(0x59), [0x60] = K(0x5B), [0x61] = K(0x5C),                    /* keypad 7 8 9 */
    [0x62] = K(0x52),      /* keypad 0 */
    [0x63] = K(0x41),      /* keypad . */
    [0x64] = K(0x0A),      /* non-US \ (ISO key left of Z) */
    [0x67] = K(0x51),      /* keypad = */
};

/* Modifier usages $E0-$E7: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI ->
 * Control, Shift, Option, Command (one key each on the Mac)
 */
static const uint8_t modifier_to_mac[8] = { 0x3B, 0x38, 0x3A, 0x37, 0x3B, 0x38, 0x3A, 0x37 };

static uint8_t s_modifiers;

static void hid_key(uint8_t usage, bool pressed)
{
    if (usage >= 0xE0 && usage <= 0xE7) {
        int bit = usage - 0xE0;
        uint8_t code = modifier_to_mac[bit];
        uint8_t mask = 0;
        for (int i = 0; i < 8; i++) {
            mask |= modifier_to_mac[i] == code ? 1u << i : 0;
        }
        bool was = (s_modifiers & mask) != 0;
        s_modifiers = pressed ? s_modifiers | (1u << bit) : s_modifiers & ~(1u << bit);
        bool now = (s_modifiers & mask) != 0;
        if (was != now) {
            mac_key(code, now);
        }
        return;
    }
    if (usage < sizeof(usage_to_mac) && usage_to_mac[usage]) {
        mac_key(usage_to_mac[usage] & 0x7F, pressed);
    }
}

static void hid_mouse(int dx, int dy, uint8_t buttons)
{
    /* The Mac has one button: any of them */
    mac_mouse(dx, dy, buttons != 0);
}

/* A finger on the picture is the pointer: down there clicks */
static void touch(int x, int y, bool pressed)
{
    mac_mouse_position(x, y, pressed);
}

/* ---- Setup screen choices ---- */

enum { CHOICE_MODEL, CHOICE_MEMORY, CHOICE_MONITOR, CHOICES };

static const char *const model_names[] = { "LC II", "LC III", "" };
static const char *const monitor_names[] = { "640 x 480", "512 x 384", "" };

static int model_get(void)
{
    return settings.model;
}

static void model_set(int i)
{
    if (i == MAC_MODEL_LC2 || i == MAC_MODEL_LC3) {
        settings.model = i;
        settings_save();
    }
}

/* The model's size at or below the setting */
static int ram_get(void)
{
    const uint16_t *sizes = ram_sizes[settings.model];
    int at = 0;
    for (int i = 0; i < RAM_CHOICES && sizes[i]; i++) {
        if (sizes[i] <= settings.ram_mb) {
            at = i;
        }
    }
    return at;
}

static void ram_set(int i)
{
    if (i >= 0 && i < RAM_CHOICES && ram_sizes[settings.model][i]) {
        settings.ram_mb = ram_sizes[settings.model][i];
        settings_save();
    }
}

static int monitor_get(void)
{
    return settings.monitor;
}

static void monitor_set(int i)
{
    if (i == MAC_MONITOR_13 || i == MAC_MONITOR_12) {
        settings.monitor = i;
        settings_save();
    }
}

static const emu_choice_t choices[CHOICES] = {
    { "Model", model_names, model_get, model_set, false },
    { "Memory", ram_names_lc2, ram_get, ram_set, false },
    { "Monitor", monitor_names, monitor_get, monitor_set, false },
};

/* The Memory row's buttons follow the model */
static const char *const *choice_names(int i)
{
    if (i == CHOICE_MEMORY) {
        return settings.model == MAC_MODEL_LC3 ? ram_names_lc3 : ram_names_lc2;
    }
    return NULL;
}

/* ---- Disks ---- */

static const char *const extensions[] = {
    ".dsk", ".img", ".image", ".hfv", ".hda", ".iso", ".toast", ".dmg", NULL,
};
static const char *const drive_names[] = { "Drive 1", "Drive 2", "Drive 3" };
static const disks_size_t sizes[] = {
    { 800 * 1024, "800 KB floppy" },
    { 1440 * 1024, "1.4 MB floppy" },
    { 20 * 1024 * 1024, "20 MB hard disk" },
    { 100 * 1024 * 1024, "100 MB hard disk" },
    { 500 * 1024 * 1024, "500 MB hard disk" },
};

static const disks_config_t disks = {
    .folder = "disks",
    .extensions = extensions,
    .description = "Disk images (.DSK, .IMG, .IMAGE, .HFV, .ISO, .DMG)",
    .title = "Disks",
    .button = "Disks",
    .nvs_namespace = "mac3disks",
    .drives = MAC_DRIVES,
    .drive_names = drive_names,
    .new_prefix = "BLANK",
    .new_extension = ".dsk",
    .sizes = sizes,
    .size_count = sizeof(sizes) / sizeof(sizes[0]),
    .max_upload = 2000ull * 1024 * 1024,
    .insert = mac_disk_insert,
    .eject = mac_disk_eject,
    .path = mac_disk_path,
    .write_protected = mac_disk_write_protected,
    .create = mac_disk_create,
};

/* ---- Status ---- */

static void size_text(char *buf, size_t len, uint64_t bytes)
{
    if (bytes >= 1024 * 1024 && bytes % (1024 * 1024) == 0) {
        snprintf(buf, len, "%llu MB", (unsigned long long)(bytes >> 20));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, len, "%.1f MB", bytes / 1048576.0);
    } else {
        snprintf(buf, len, "%llu KB", (unsigned long long)(bytes >> 10));
    }
}

static const char *depth_text(int bits)
{
    switch (bits) {
    case 1:  return "black and white";
    case 2:  return "4 colours";
    case 4:  return "16 colours";
    case 8:  return "256 colours";
    case 16: return "thousands of colours";
    default: return "millions of colours";
    }
}

static void status(emu_json_t *j, int section)
{
    char text[160];
    if (section == EMU_STATUS_ROWS) {
        snprintf(text, sizeof(text), "Macintosh %s, %lu MB", mac_model() == MAC_MODEL_LC3 ? "LC III" : "LC II",
                 (unsigned long)(s_cfg.ram_size >> 20));
        emu_json_row(j, "System", text);
        emu_json_row(j, "ROM", s_rom_name ? s_rom_name : "-");
        snprintf(text, sizeof(text), "68030 at %.1f MHz <span class=\"dim\">(%d%% of the %s's)</span>",
                 mac_cpu_mhz(), mac_cpu_speed() * 100 / MAC_SPEED_NATIVE,
                 mac_model() == MAC_MODEL_LC3 ? "LC III" : "LC II");
        emu_json_row(j, "CPU", text);
        int w, h;
        mac_screen_size(&w, &h);
        snprintf(text, sizeof(text), "%d × %d, %s, %d Hz", w, h, depth_text(mac_screen_depth()), MAC_SCREEN_HZ);
        emu_json_row(j, "Screen", text);
        uint32_t ram_free, system_free;
        if (mac_ram_free(&ram_free, &system_free)) {
            snprintf(text, sizeof(text), "%lu KB of %lu MB <span class=\"dim\">(system heap %lu KB free)</span>",
                     (unsigned long)(ram_free / 1024), (unsigned long)(s_cfg.ram_size >> 20),
                     (unsigned long)(system_free / 1024));
            emu_json_row_bar(j, "Free RAM", text, 100 - (int)((uint64_t)ram_free * 100 / s_cfg.ram_size));
        }
        return;
    }
    mac_disk_status_t ds;
    mac_disk_status(&ds);
    for (int d = 0; d < MAC_DRIVES; d++) {
        const char *name = disks_name(d);
        size_text(text, sizeof(text), mac_disk_size(d));
        emu_json_drive(j, drive_names[d], name, mac_disk_write_protected(d), ds.light == d,
                       ds.reads[d], ds.writes[d], name ? text : NULL);
    }
}

/* ---- Console ---- */

static void console_info(char *buf, size_t len)
{
    mac_status_t s;
    mac_get_status(&s);
    snprintf(buf, len, "frames %lu PC $%08lx SR $%04x Ticks %lu%s", (unsigned long)s.frames,
             (unsigned long)s.pc, s.sr, (unsigned long)s.ticks, s.halted ? " HALTED" : "");
}

/*   k <hex>            tap Mac key code <hex> (e.g. 24 Return)
 *   K <hex> <0|1>      Mac key code down (1) or up (0)
 *   m <dx> <dy> <btn>  move the mouse, button 0/1
 *   P <x> <y> <btn>    pointer at screen position x, y (p is the runtime's profiler)
 *   R <model> <MB> [m] restart as model 0 LC II, 1 LC III, with that RAM (LC II
 *                      4, 6, 8, 10; LC III 4, 8, 16, 20) and monitor (0 13", 1 12")
 *   n                  the programmer's interrupt switch
 */
static bool console_command(const char *line)
{
    switch (line[0]) {
    case 'k': {
        unsigned code = strtoul(line + 1, NULL, 16);
        mac_key(code, true);
        vTaskDelay(pdMS_TO_TICKS(80));
        mac_key(code, false);
        return true;
    }
    case 'K': {
        unsigned code = 0, down = 0;
        if (sscanf(line + 1, "%x %u", &code, &down) == 2) {
            mac_key(code, down);
        }
        return true;
    }
    case 'm': {
        int dx = 0, dy = 0, button = 0;
        sscanf(line + 1, "%d %d %d", &dx, &dy, &button);
        mac_mouse(dx, dy, button);
        return true;
    }
    case 'P': {
        int x = 0, y = 0, button = 0;
        sscanf(line + 1, "%d %d %d", &x, &y, &button);
        mac_mouse_position(x, y, button);
        return true;
    }
    case 'R': {
        int model = 0, ram_mb = 0, monitor = settings.monitor;
        int n = sscanf(line + 1, "%d %d %d", &model, &ram_mb, &monitor);
        bool ok = n >= 2 && (model == MAC_MODEL_LC2 || model == MAC_MODEL_LC3) &&
                  (monitor == MAC_MONITOR_13 || monitor == MAC_MONITOR_12);
        for (int i = 0; ok && i < RAM_CHOICES; i++) {
            if (ram_sizes[model][i] == ram_mb) {
                settings.model = model;
                settings.ram_mb = ram_mb;
                settings.monitor = monitor;
                settings_save();
                emu_restart();
                break;
            }
        }
        return true;
    }
    case 'n':
        mac_interrupt_switch();
        return true;
    default:
        return false;
    }
}

/* ---- The machine ---- */

#if CONFIG_EMU_WEB
/* web/macfiles.js: StuffIt, Compact Pro, BinHex and MacBinary files dropped
 * on the web page become HFS disk images in the browser */
extern const char macfiles_js_start[] asm("_binary_macfiles_js_start");
extern const char macfiles_js_end[] asm("_binary_macfiles_js_end");
#endif

static const emu_machine_t machine = {
    .name = "Macintosh",
    .short_name = "Mac",
    .view_width = 800,          /* 640 x 480 (or 512 x 384) at 4:3: 960 x 720 on the Tab5 */
    .scale = EMU_SCALE_SMOOTH,
    .reserve_memory = reserve_memory,
    .start = start,
    .reset = mac_reset,
    .run_frame = run_frame,
    .halted = halted,
    .screen_mode = screen_mode,
    .screen_signature = mac_screen_signature,
    .render_line = mac_render_line_rgb565,
    .audio_hz = MAC_AUDIO_HZ,
    .audio_enable = mac_audio_enable,
    .audio_render = mac_audio_render,
    .audio_pending = mac_audio_pending,
    .hid_key = hid_key,
    .hid_mouse = hid_mouse,
    .touch = touch,
    .choices = choices,
    .choice_count = CHOICES,
    .choice_names = choice_names,
    .disks = &disks,
#if CONFIG_EMU_WEB
    .web_script = macfiles_js_start,
    .web_script_end = macfiles_js_end,
#endif
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
    .every_second = every_second,
};

void app_main(void)
{
    emu_run(&machine);
}
