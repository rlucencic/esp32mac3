/*
 * The Apple Sound Chip: FIFOs of 1 KB and registers at $800, played at
 * 22.257 kHz (here one sample per line, 22.255 kHz). Two of them, as MAME's
 * asc.cpp has them (R. Belmont; Doug Brown's ASCTester results):
 *
 *   V8 (LC II)      FIFO A only (8-bit offset binary, mono), version $E8;
 *                   mode, control and FIFO mode read 1. The interrupt is a
 *                   level: on while FIFO A is less than half full.
 *   Sonora (LC III) FIFO A (left) and B (right), version $BC; mode reads 1,
 *                   control, FIFO mode, wavetable control and clock 0. The
 *                   status: bit 1 FIFO A empty (always so in playback, the
 *                   only mode used), 2 a FIFO less than half full, 3 a FIFO
 *                   empty. The interrupt is a level too: a FIFO less than
 *                   half full unless $F29 bit 0 masks it, or playback unless
 *                   $F09 bit 0 does.
 *
 *   $000-$3FF  FIFO A (each write appends a sample), $400-$7FF FIFO B
 *   $800 version, $801 mode, $802 control, $803 FIFO mode (bit 7 clears
 *   the FIFOs), $804 FIFO status, $80A play/record (bit 0 record)
 *
 * The samples a frame plays are collected and go into a ring at the end of
 * the frame, which mac_audio_render() drains from the audio task.
 */
#include <stdlib.h>
#include <string.h>
#include "mac_internal.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define FIFO_SIZE   0x400
#define HALF        0x200

enum { R_VERSION, R_MODE, R_CONTROL, R_FIFOMODE, R_FIFOSTAT, R_WTCONTROL, R_VOLUME, R_CLOCK,
       R_PLAYREC = 0x0A, R_FIFOA_IRQCTRL = 0x709, R_FIFOB_IRQCTRL = 0x729 };

#define STAT_HALF_A     0x01
#define STAT_EDGE_A     0x02        /* empty or full */
#define STAT_HALF_B     0x04
#define STAT_EDGE_B     0x08

static struct {
    bool sonora;
    uint8_t fifo[2][FIFO_SIZE];
    uint16_t rd[2], wr[2], count[2];
    uint8_t regs[0x30];             /* $800-$82F */
    uint8_t irq_mask[2];            /* Sonora: $F09, $F29 */
    bool irq;
    int16_t last[2];                /* the sample playing (held when empty) */
    int32_t dc_in[2], dc_out[2];    /* the output's DC blocker */
    uint32_t pos;                   /* samples of this frame played */
    int16_t frame[FRAME_LINES][2];
} asc;

static void set_irq(bool active)
{
    if (active != asc.irq) {
        asc.irq = active;
        asc_irq_changed(active);
    }
}

static int channels(void)
{
    return asc.sonora ? 2 : 1;
}

/* The status and the interrupt follow the FIFOs' levels */
static void update_status(void)
{
    uint8_t s = 0;
    if (asc.sonora) {
        bool playback = !(asc.regs[R_PLAYREC] & 1);
        bool half = asc.count[0] < HALF || asc.count[1] < HALF;
        if (playback) {
            s |= STAT_EDGE_A;
        }
        if (half) {
            s |= STAT_HALF_B;
        }
        if (asc.count[0] == 0 || asc.count[1] == 0) {
            s |= STAT_EDGE_B;
        }
        asc.regs[R_FIFOSTAT] = s;
        set_irq((half && !(asc.irq_mask[1] & 1)) || (playback && !(asc.irq_mask[0] & 1)));
        return;
    }
    if (asc.count[0] < HALF) {
        s |= STAT_HALF_A;
    }
    if (asc.count[0] == 0 || asc.count[0] >= FIFO_SIZE - 1) {
        s |= STAT_EDGE_A;
    }
    asc.regs[R_FIFOSTAT] = s;
    set_irq(asc.count[0] < HALF);
}

static int8_t fifo_sample(int ch)
{
    if (asc.count[ch]) {
        int8_t s = (int8_t)(asc.fifo[ch][asc.rd[ch]] ^ 0x80);
        asc.rd[ch] = (asc.rd[ch] + 1) & (FIFO_SIZE - 1);
        asc.count[ch]--;
        return s;
    }
    return (int8_t)(asc.last[ch] / 256);
}

/* Play the samples due by now */
static void advance(void)
{
    uint64_t t = mac_now() - mac.frame_start;
    uint32_t upto = t / LINE_CYCLES;
    if (upto > FRAME_LINES) {
        upto = FRAME_LINES;
    }
    if (asc.pos >= upto) {
        return;
    }
    while (asc.pos < upto) {
        int8_t l = fifo_sample(0);
        int8_t r = asc.sonora ? fifo_sample(1) : l;
        asc.last[0] = l * 256;
        asc.last[1] = r * 256;
        asc.frame[asc.pos][0] = asc.last[0];
        asc.frame[asc.pos][1] = asc.last[1];
        asc.pos++;
    }
    update_status();
}

/* The next time the status changes on its own: a FIFO falls to half, or
 * runs empty
 */
static void schedule(void)
{
    uint32_t n = 0;
    for (int ch = 0; ch < channels(); ch++) {
        uint32_t c = asc.count[ch];
        uint32_t m = c > HALF ? c - HALF : c;
        if (c && (n == 0 || m < n)) {
            n = m;
        }
    }
    mac_schedule(EV_ASC, n ? mac.frame_start + (uint64_t)(asc.pos + n) * LINE_CYCLES : EV_NEVER);
}

void asc_event(void)
{
    advance();
    schedule();
}

static void clear_fifos(void)
{
    memset(asc.rd, 0, sizeof(asc.rd));
    memset(asc.wr, 0, sizeof(asc.wr));
    memset(asc.count, 0, sizeof(asc.count));
}

void asc_reset(bool sonora)
{
    memset(&asc, 0, sizeof(asc));
    asc.sonora = sonora;
    asc.regs[R_MODE] = 1;
    asc_irq_changed(false);
    update_status();
    mac_schedule(EV_ASC, EV_NEVER);
}

uint8_t asc_read(uint32_t address)
{
    uint32_t offset = address & 0xFFF;
    if (offset < 0x800) {
        return asc.fifo[offset >> 10][offset & (FIFO_SIZE - 1)];
    }
    uint32_t r = offset - 0x800;
    switch (r) {
    case R_VERSION:
        return asc.sonora ? 0xBC : 0xE8;
    case R_MODE:
        return 1;
    case R_CONTROL:
    case R_FIFOMODE:
        return asc.sonora ? 0 : 1;
    case R_FIFOSTAT:
        advance();
        return asc.regs[R_FIFOSTAT];
    case R_WTCONTROL:
    case R_CLOCK:
        return 0;
    case R_FIFOA_IRQCTRL:
    case R_FIFOB_IRQCTRL:
        return asc.sonora ? asc.irq_mask[r == R_FIFOB_IRQCTRL] : 1;
    default:
        return r < sizeof(asc.regs) ? asc.regs[r] : 0;
    }
}

void asc_write(uint32_t address, uint8_t value)
{
    uint32_t offset = address & 0xFFF;
    if (offset < 0x800) {
        int ch = offset >> 10;
        advance();
        if (ch < channels() && asc.count[ch] < FIFO_SIZE) {
            asc.fifo[ch][asc.wr[ch]] = value;
            asc.wr[ch] = (asc.wr[ch] + 1) & (FIFO_SIZE - 1);
            asc.count[ch]++;
        }
        update_status();
        schedule();
        return;
    }
    uint32_t r = offset - 0x800;
    advance();
    switch (r) {
    case R_MODE:
    case R_CONTROL:
    case R_WTCONTROL:
    case R_CLOCK:
        return;                     /* fixed on both */
    case R_FIFOMODE:
        if (value & 0x80) {
            clear_fifos();
        }
        break;
    case R_FIFOA_IRQCTRL:
    case R_FIFOB_IRQCTRL:
        if (asc.sonora) {
            asc.irq_mask[r == R_FIFOB_IRQCTRL] = value & 1;
        }
        break;
    default:
        break;
    }
    if (r < sizeof(asc.regs)) {
        asc.regs[r] = value;
    }
    update_status();
    schedule();
}

/* ---- Output ---- */

#define RING        4096            /* stereo frames, power of two */

static bool s_enabled;
static int16_t *s_ring;             /* RING stereo frames, made when sound is on */
static volatile uint32_t s_head, s_tail;

void mac_audio_enable(bool enable)
{
    if (enable && s_ring == NULL) {
        size_t bytes = RING * 2 * sizeof(int16_t);
#ifdef ESP_PLATFORM
        s_ring = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#else
        s_ring = malloc(bytes);
#endif
    }
    s_enabled = enable && s_ring != NULL;
}

void asc_frame_end(void)
{
    advance();
    asc.pos = 0;
    if (!s_enabled) {
        return;
    }
    uint32_t tail = __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);
    if (s_head - tail > RING - FRAME_LINES) {
        return;                     /* nobody is listening: drop the frame */
    }
    /* The Macs' audio output is AC coupled: a DC blocker (a high pass at
     * ~20 Hz) takes out the offset a FIFO left at a level adds
     */
    for (int i = 0; i < FRAME_LINES; i++) {
        uint32_t j = (s_head + i) % RING;
        for (int ch = 0; ch < 2; ch++) {
            int32_t x = asc.frame[i][ch] * 5 / 8;
            int32_t y = x - asc.dc_in[ch] + (asc.dc_out[ch] * 4037) / 4096;
            asc.dc_in[ch] = x;
            asc.dc_out[ch] = y;
            s_ring[j * 2 + ch] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        }
    }
    __atomic_store_n(&s_head, s_head + FRAME_LINES, __ATOMIC_RELEASE);
}

int mac_audio_render(int16_t *dst, int frames)
{
    if (s_ring == NULL) {
        return 0;
    }
    uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    uint32_t avail = head - s_tail;
    int n = frames < (int)avail ? frames : (int)avail;
    for (int i = 0; i < n; i++) {
        uint32_t j = (s_tail + i) % RING;
        dst[i * 2] = s_ring[j * 2];
        dst[i * 2 + 1] = s_ring[j * 2 + 1];
    }
    __atomic_store_n(&s_tail, s_tail + n, __ATOMIC_RELEASE);
    return n;
}

int mac_audio_pending(void)
{
    return (int)(__atomic_load_n(&s_head, __ATOMIC_ACQUIRE) - __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE));
}
