/*
 * Desktop build of the Macintosh LC II / LC III core, for testing without the board:
 *
 *   mac_host <rom> [frames] [out.ppm] [ram_mb]
 *
 * Environment:
 *   MAC_DISK1 .. MAC_DISK3   disk images in drives 1 .. 3 at power-on
 *   MAC_PRAM=<file>          parameter RAM, loaded and saved (256 bytes)
 *   MAC_SCRIPT=<file>        lines "<frame> <command>":
 *                              key <hex> 0|1        Mac key code down / up
 *                              type <text>          type ASCII letters/digits/space
 *                              mouse <dx> <dy> <b>  relative motion, button
 *                              pos <x> <y> <b>      pointer at x, y
 *                              shot <file.ppm>      screenshot
 *                              insert <d> <path>    disk into drive d (1..3)
 *                              eject <d>
 *                              peek <hex addr>      print the word there
 *                              dis <hex addr> [n]   disassemble n instructions
 *                              free                 the Mac's free RAM
 *                              units                the unit table's used entries
 *   MAC_STATUS=<n>           print PC, Ticks every n frames
 *   MAC_PCHIST=[<from>:]<to> the most executed instructions in those frames
 *   MAC_TRACE=<pc>[:<n>]     disassemble n instructions from when PC is reached
 *                            (the full PC or its low 24 bits); MAC_TRACE_ALL: with
 *                            all the registers
 *   MAC_RING=<pc>[-<end>]:<n> the n instructions before PC first reaches pc (or
 *                            the range); pc 0: before the first slide through
 *                            zeros (256 ori.b #0,d0 in a row: code read from
 *                            nothing); MAC_RING_JUMPS: only the instructions
 *                            that don't follow a 4-byte one
 *   MAC_DIS=<addr>[:<n>]     disassemble n instructions at addr (after loading)
 *   MAC_REGS                 the registers at the end
 *   MAC_MONITOR=12           the 12" RGB monitor (512 x 384) instead of the 13"
 *   MAC_IO=1 | <hex mask>    log I/O accesses (all, or the 8 KB blocks in the mask)
 *   MAC_EGRET                log Egret's commands
 *   MAC_WAV=<file>           record the sound (22255 Hz mono WAV)
 *   MAC_QUIET                no frame summary
 *   MAC_DEBUG=<port>         serve the debugger (esp32emu's docs/debugger.md) on
 *                            127.0.0.1:<port> and run in real time until killed
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "m68k.h"
#include "mac.h"
#include "emudbg.h"

static uint8_t s_pram[MAC_PRAM_SIZE];
static const char *s_pram_file;

static void save_pram(void)
{
    if (s_pram_file) {
        FILE *f = fopen(s_pram_file, "wb");
        if (f) {
            fwrite(s_pram, 1, sizeof(s_pram), f);
            fclose(f);
        }
    }
}

static void ejected(int drive)
{
    printf("host: the Mac ejected drive %d\n", drive + 1);
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    int w, h;
    mac_screen_size(&w, &h);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    uint16_t line[MAC_SCREEN_MAX_WIDTH];
    for (int y = 0; y < h; y++) {
        mac_render_line_rgb565(y, line);
        for (int x = 0; x < w; x++) {
            uint16_t p = line[x];
            uint8_t px[3] = { (p >> 11) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31 };
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    printf("host: screenshot %s\n", path);
}

/* ---- Tracing ---- */

static uint32_t s_trace_pc = 0xFFFFFFFF;
static int s_trace_left;
static bool s_tracing;

/* MAC_PCHIST=<frames>: the most executed instructions until then */
static uint32_t *s_hist;
static int s_hist_frames;

static void print_hist(void)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < 0x800000; i++) {
        total += s_hist[i];
    }
    printf("  %llu instructions\n", (unsigned long long)total);
    for (int k = 0; k < 24; k++) {
        uint32_t best = 0, at = 0;
        for (uint32_t i = 0; i < 0x800000; i++) {
            if (s_hist[i] > best) {
                best = s_hist[i];
                at = i;
            }
        }
        if (!best) {
            break;
        }
        char buf[100];
        m68k_disassemble(buf, at * 2, M68K_CPU_TYPE_68030);
        printf("  %8u  %06x  %s\n", best, at * 2, buf);
        s_hist[at] = 0;
    }
}

/* MAC_RING=<pc>[-<end>]:<n>: the n instructions before PC first reaches pc
 * (or the range up to end; 0 for the first slide through zeros: 256
 * ori.b #0,d0 in a row, 4 bytes each); MAC_RING_JUMPS: only the ones that
 * don't follow a 4-byte instruction
 */
static uint32_t s_ring[4096];
static uint32_t s_ring_n, s_ring_at, s_ring_pc = 0xFFFFFFFF, s_ring_end;
static bool s_ring_jumps;

static void print_ring(void)
{
    uint32_t n = s_ring_at < s_ring_n ? s_ring_at : s_ring_n;
    printf("host: the %u instructions before $%08x:\n", n, m68k_get_reg(NULL, M68K_REG_PC));
    for (uint32_t i = s_ring_at - n; i != s_ring_at; i++) {
        uint32_t pc = s_ring[i % s_ring_n];
        char buf[100];
        m68k_disassemble(buf, pc, M68K_CPU_TYPE_68030);
        printf("  %08x  %s\n", pc, buf);
    }
}

void m68k_host_instruction(unsigned int pc)
{
    if (s_ring_n) {
        static uint32_t last, run;
        run = pc == last + 4 ? run + 1 : 0;
        if (s_ring_pc ? pc - s_ring_pc <= s_ring_end - s_ring_pc : run >= 256) {
            print_ring();
            s_ring_n = 0;
        } else if (pc != last + 4 || !s_ring_jumps) {
            s_ring[s_ring_at++ % s_ring_n] = pc;
        }
        last = pc;
    }
    if (s_hist) {
        s_hist[(pc & 0xFFFFFF) >> 1]++;
    }
    if (!s_tracing && (pc == s_trace_pc || (pc & 0xFFFFFF) == s_trace_pc) && s_trace_left > 0) {
        s_tracing = true;
    }
    if (s_tracing) {
        char buf[100];
        m68k_disassemble(buf, pc, M68K_CPU_TYPE_68030);
        if (getenv("MAC_TRACE_ALL")) {
            /* All the registers */
            printf("  %08x  %-36s", pc, buf);
            for (int r = 0; r < 16; r++) {
                printf(" %c%d=%x", r < 8 ? 'd' : 'a', r & 7, m68k_get_reg(NULL, M68K_REG_D0 + r));
            }
            printf("\n");
        } else {
            printf("  %08x  %-40s D0=%08x D1=%08x A0=%08x A1=%08x A7=%08x\n", pc, buf,
                   m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
                   m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
                   m68k_get_reg(NULL, M68K_REG_A7));
        }
        if (--s_trace_left <= 0) {
            s_tracing = false;
        }
    }
}

static void disassemble(uint32_t addr, int n)
{
    for (int i = 0; i < n; i++) {
        char buf[100];
        int len = m68k_disassemble(buf, addr, M68K_CPU_TYPE_68030);
        printf("  %06x  %04x  %s\n", addr, mac_peek16(addr), buf);
        addr += len;
    }
}

/* ---- Script ---- */

typedef struct {
    uint32_t frame;
    char cmd[16];
    char arg[512];
} line_t;

static line_t *s_script;
static int s_lines, s_next;

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "no script %s\n", path);
        exit(1);
    }
    s_script = calloc(4096, sizeof(line_t));
    char buf[600];
    while (s_lines < 4096 && fgets(buf, sizeof(buf), f)) {
        line_t *l = &s_script[s_lines];
        int at = 0;
        if (buf[0] == '#' || sscanf(buf, "%u %15s %n", &l->frame, l->cmd, &at) < 2) {
            continue;
        }
        snprintf(l->arg, sizeof(l->arg), "%s", buf + at);
        l->arg[strcspn(l->arg, "\r\n")] = 0;
        s_lines++;
    }
    fclose(f);
}

/* ASCII to Mac key codes (US layout), for "type" */
static int ascii_key(char c, bool *shift)
{
    static const char *lower = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.\0 `";
    *shift = false;
    if (c >= 'A' && c <= 'Z') {
        *shift = true;
        c = c - 'A' + 'a';
    }
    for (int i = 0; lower[i] || i < 0x33; i++) {
        if (lower[i] == c && c) {
            return i;
        }
    }
    if (c == '\n') {
        return 0x24;
    }
    return -1;
}

static int s_type_delay;

static void type_text(const char *text)
{
    /* Queued as key events; the keyboard hands them out one at a time */
    for (const char *p = text; *p; p++) {
        bool shift;
        int k = ascii_key(*p, &shift);
        if (k < 0) {
            continue;
        }
        if (shift) {
            mac_key(0x38, true);
        }
        mac_key(k, true);
        mac_key(k, false);
        if (shift) {
            mac_key(0x38, false);
        }
    }
    (void)s_type_delay;
}

static void run_script(uint32_t frame)
{
    while (s_next < s_lines && s_script[s_next].frame <= frame) {
        line_t *l = &s_script[s_next++];
        if (strcmp(l->cmd, "key") == 0) {
            unsigned code = 0, down = 0;
            sscanf(l->arg, "%x %u", &code, &down);
            mac_key(code, down);
        } else if (strcmp(l->cmd, "type") == 0) {
            type_text(l->arg);
        } else if (strcmp(l->cmd, "mouse") == 0) {
            int dx = 0, dy = 0, b = 0;
            sscanf(l->arg, "%d %d %d", &dx, &dy, &b);
            mac_mouse(dx, dy, b);
        } else if (strcmp(l->cmd, "pos") == 0) {
            int x = 0, y = 0, b = 0;
            sscanf(l->arg, "%d %d %d", &x, &y, &b);
            mac_mouse_position(x, y, b);
        } else if (strcmp(l->cmd, "shot") == 0) {
            write_ppm(l->arg);
        } else if (strcmp(l->cmd, "insert") == 0) {
            int d = 0, at = 0;
            sscanf(l->arg, "%d %n", &d, &at);
            if (!mac_disk_insert(d - 1, l->arg + at)) {
                printf("host: can't insert %s\n", l->arg + at);
            }
        } else if (strcmp(l->cmd, "peek") == 0) {
            unsigned a = 0;
            sscanf(l->arg, "%x", &a);
            printf("host: frame %u: $%06x = $%04x\n", frame, a, mac_peek16(a));
        } else if (strcmp(l->cmd, "free") == 0) {
            uint32_t f, sf;
            if (mac_ram_free(&f, &sf)) {
                printf("host: frame %u: %u KB free, system heap %u KB free\n", frame, f / 1024, sf / 1024);
            } else {
                printf("host: frame %u: free memory not known yet\n", frame);
            }
        } else if (strcmp(l->cmd, "units") == 0) {
            uint32_t base = (mac_peek16(0x11C) << 16 | mac_peek16(0x11E)) & 0xFFFFFF;
            int count = (int16_t)mac_peek16(0x1D2);
            printf("host: unit table at $%06x, %d entries; used:", base, count);
            for (int u = 0; u < count; u++) {
                if (mac_peek16(base + 4 * u) || mac_peek16(base + 4 * u + 2)) {
                    printf(" %d", u);
                }
            }
            printf("\n");
        } else if (strcmp(l->cmd, "icon") == 0) {
            unsigned a = 0;
            sscanf(l->arg, "%x", &a);
            for (int y = 0; y < 32; y++) {
                uint32_t row = ((uint32_t)mac_peek16(a + y * 4) << 16) | mac_peek16(a + y * 4 + 2);
                printf("    ");
                for (int x = 31; x >= 0; x--) {
                    putchar(row >> x & 1 ? '#' : '.');
                }
                printf("\n");
            }
        } else if (strcmp(l->cmd, "dis") == 0) {
            unsigned a = 0;
            int n = 20;
            sscanf(l->arg, "%x %d", &a, &n);
            disassemble(a, n);
        } else if (strcmp(l->cmd, "eject") == 0) {
            mac_disk_eject(atoi(l->arg) - 1);
        } else {
            printf("host: unknown script command %s\n", l->cmd);
        }
    }
}

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rom> [frames] [out.ppm] [ram_mb]\n", argv[0]);
        return 1;
    }
    uint32_t rom_size;
    uint8_t *rom = read_file(argv[1], &rom_size);
    if (rom == NULL) {
        fprintf(stderr, "can't read %s\n", argv[1]);
        return 1;
    }
    int frames = argc > 2 ? atoi(argv[2]) : 600;
    const char *out = argc > 3 ? argv[3] : NULL;
    uint32_t ram_mb = argc > 4 ? atoi(argv[4]) : 4;

    s_pram_file = getenv("MAC_PRAM");
    if (s_pram_file) {
        FILE *f = fopen(s_pram_file, "rb");
        if (f) {
            fread(s_pram, 1, sizeof(s_pram), f);
            fclose(f);
        }
    }

    mac_config_t cfg = {
        .ram = malloc(ram_mb << 20),
        .ram_size = ram_mb << 20,
        .vram = malloc(MAC_VRAM_SIZE),
        .monitor = getenv("MAC_MONITOR") && atoi(getenv("MAC_MONITOR")) == 12 ? MAC_MONITOR_12 : MAC_MONITOR_13,
        .rom = rom,
        .rom_size = rom_size,
        .rom_copy = malloc(rom_size),
        .pram = s_pram,
        .pram_changed = save_pram,
        .disk_ejected = ejected,
    };
    if (!mac_init(&cfg)) {
        fprintf(stderr, "mac_init failed\n");
        return 1;
    }
    for (int d = 0; d < MAC_DRIVES; d++) {
        char name[16];
        snprintf(name, sizeof(name), "MAC_DISK%d", d + 1);
        const char *path = getenv(name);
        if (path && !mac_disk_insert(d, path)) {
            fprintf(stderr, "can't insert %s\n", path);
        }
    }
    const char *script = getenv("MAC_SCRIPT");
    if (script) {
        load_script(script);
    }
    const char *trace = getenv("MAC_TRACE");
    if (trace) {
        s_trace_left = 200;
        sscanf(trace, "%x:%d", &s_trace_pc, &s_trace_left);
    }
    const char *ring = getenv("MAC_RING");
    if (ring) {
        s_ring_n = 64;
        if (sscanf(ring, "%x-%x:%u", &s_ring_pc, &s_ring_end, &s_ring_n) < 2) {
            sscanf(ring, "%x:%u", &s_ring_pc, &s_ring_n);
            s_ring_end = s_ring_pc;
        }
        if (s_ring_n > 4096) {
            s_ring_n = 4096;
        }
        s_ring_jumps = getenv("MAC_RING_JUMPS") != NULL;
    }
    const char *dis = getenv("MAC_DIS");
    if (dis) {
        unsigned addr = 0;
        int n = 40;
        sscanf(dis, "%x:%d", &addr, &n);
        disassemble(addr, n);
        return 0;
    }
    int status_every = getenv("MAC_STATUS") ? atoi(getenv("MAC_STATUS")) : 0;
    FILE *wav = getenv("MAC_WAV") ? fopen(getenv("MAC_WAV"), "wb") : NULL;
    uint32_t wav_frames = 0;
    if (wav) {
        mac_audio_enable(true);
        fseek(wav, 44, SEEK_SET);
    }

    emudbg_host_run("MAC_DEBUG", mac_run_frame, 60);
    clock_t start = clock();
    int hist_from = 0;
    if (getenv("MAC_PCHIST")) {
        s_hist_frames = atoi(getenv("MAC_PCHIST"));
        const char *colon = strchr(getenv("MAC_PCHIST"), ':');
        if (colon) {
            hist_from = s_hist_frames;
            s_hist_frames = atoi(colon + 1);
        }
    }
    for (int f = 0; f < frames; f++) {
        if (getenv("MAC_PCHIST") && f == hist_from && !s_hist) {
            s_hist = calloc(0x800000, sizeof(uint32_t));
        }
        if (s_hist && f == s_hist_frames) {
            print_hist();
            free(s_hist);
            s_hist = NULL;
        }
        run_script(f);
        mac_run_frame();
        if (wav) {
            int16_t buf[1024 * 2];
            int n;
            while ((n = mac_audio_render(buf, 1024)) > 0) {
                for (int i = 0; i < n; i++) {
                    fwrite(&buf[i * 2], 2, 1, wav);
                }
                wav_frames += n;
            }
        }
        if (status_every && (f + 1) % status_every == 0) {
            mac_status_t s;
            mac_get_status(&s);
            printf("frame %d: PC $%08x SR $%04x Ticks %u%s\n", f + 1, s.pc, s.sr, s.ticks,
                   s.halted ? " HALTED" : "");
        }
    }
    double secs = (double)(clock() - start) / CLOCKS_PER_SEC;
    if (!getenv("MAC_QUIET")) {
        mac_status_t s;
        mac_get_status(&s);
        printf("%d frames in %.2f s (%.0f%% of real time), PC $%08x, Ticks %u, %d bits%s\n", frames, secs,
               frames / 60.15 / secs * 100, s.pc, s.ticks, mac_screen_depth(), s.halted ? ", HALTED" : "");
        if (getenv("MAC_REGS")) {
            for (int r = 0; r < 16; r++) {
                printf("%c%d=%08x%s", r < 8 ? 'D' : 'A', r & 7, m68k_get_reg(NULL, M68K_REG_D0 + r),
                       (r & 7) == 7 ? "\n" : " ");
            }
        }
        mac_disk_status_t ds;
        mac_disk_status(&ds);
        for (int d = 0; d < MAC_DRIVES; d++) {
            if (mac_disk_path(d)) {
                printf("drive %d: %u blocks read, %u written\n", d + 1, ds.reads[d], ds.writes[d]);
            }
            uint64_t free_bytes, size;
            if (mac_disk_space(d, &free_bytes, &size)) {
                printf("drive %d: %llu bytes free of %llu\n", d + 1, (unsigned long long)free_bytes,
                       (unsigned long long)size);
            }
        }
    }
    if (out) {
        write_ppm(out);
    }
    if (wav) {
        /* RIFF header: mono, 16-bit */
        uint32_t data = wav_frames * 2, rate = MAC_AUDIO_HZ, bytes = rate * 2;
        uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x01\0........\x02\0\x10\0data....";
        uint32_t riff = 36 + data;
        memcpy(h + 4, &riff, 4);
        memcpy(h + 24, &rate, 4);
        memcpy(h + 28, &bytes, 4);
        memcpy(h + 40, &data, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
        printf("host: %u sound frames\n", wav_frames);
    }
    return 0;
}
