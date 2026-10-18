/*
 * The chips the emulation only has to keep the ROM and system happy (from
 * ../esp32mac):
 *
 *   SCC 85C30  registers and status, no serial data and no interrupts
 *   SWIM       in its IWM mode: mode and status registers; the disk driver
 *              is replaced, so no drive is ever read through it
 *   SCSI 5380  an empty bus: arbitration works, selection finds no device
 */
#include <string.h>
#include "mac_internal.h"

/* ---- SCC at $F04000: A1 = channel A, A2 = data ---- */

static struct {
    uint8_t pointer;
    uint8_t wr[2][16];
} scc;

void scc_reset(void)
{
    memset(&scc, 0, sizeof(scc));
}

uint8_t scc_read(uint32_t address)
{
    int ch = (address >> 1) & 1;            /* 1 = A */
    if (address & 4) {
        return 0;                           /* data: nothing received */
    }
    uint8_t reg = scc.pointer;
    scc.pointer = 0;
    switch (reg) {
    case 0:
        /* Tx buffer empty, DCD and CTS high, Tx underrun */
        return 0x04 | 0x08 | 0x20 | 0x40;
    case 1:
        return 0x01 | 0x06;                 /* all sent */
    case 2:
        return scc.wr[0][2];                /* vector (shared register) */
    case 3:
        return 0;                           /* no interrupts pending */
    case 12: case 13:
        return scc.wr[ch][reg];
    case 15:
        return scc.wr[ch][15] & 0xFA;
    default:
        return 0;
    }
}

void scc_write(uint32_t address, uint8_t v)
{
    int ch = (address >> 1) & 1;
    if (address & 4) {
        return;                             /* data: sent nowhere */
    }
    uint8_t reg = scc.pointer;
    scc.pointer = 0;
    if (reg == 0) {
        scc.pointer = v & 7;
        if (((v >> 3) & 7) == 1) {
            scc.pointer |= 8;               /* point high */
        }
        return;
    }
    if (reg == 2 || reg == 9) {
        /* Shared between the channels */
        scc.wr[0][reg] = scc.wr[1][reg] = v;
        if (reg == 9 && (v & 0xC0) == 0xC0) {
            /* Hardware reset */
            uint8_t wr9 = v & ~0xC0;
            memset(scc.wr, 0, sizeof(scc.wr));
            scc.wr[0][9] = scc.wr[1][9] = wr9;
        }
        return;
    }
    scc.wr[ch][reg] = v;
}

/* ---- SWIM at $F16000 + n * $200, IWM mode: even n = line off, odd = on ---- */

#define IWM_MOTOR   0x10

static struct {
    uint8_t lines;
    uint8_t mode;
    uint8_t status;
} iwm;

void swim_reset(void)
{
    memset(&iwm, 0, sizeof(iwm));
}

uint8_t swim_access(uint32_t address, bool write, uint8_t value)
{
    int n = (address >> 9) & 0xF;
    uint8_t result = 0;
    if (n == 0x0E && !write) {
        /* Q7 low access reads the register Q6/Q7 select now */
        switch ((iwm.lines >> 6) & 3) {
        case 1:     /* Q6 high: status */
            result = iwm.status;
            break;
        case 2:     /* Q7 high: handshake */
            result = 0x80;
            break;
        default:    /* data: no disk turning */
            break;
        }
    }
    if (n == 0x0F && write && !(iwm.lines & IWM_MOTOR)) {
        /* Q7 high access with the drive off writes the mode register */
        iwm.mode = value;
        iwm.status = (iwm.status & 0xE0) | (value & 0x1F);
    }
    uint8_t line = 1 << (n >> 1);
    if (n & 1) {
        iwm.lines |= line;
    } else {
        iwm.lines &= ~line;
    }
    if (line == IWM_MOTOR) {
        iwm.status = (n & 1) ? iwm.status | 0x20 : iwm.status & ~0x20;
    }
    return result;
}

/* ---- SCSI at $F10000: register n at + n * $10, the even byte ---- */

enum { CDR, ICR, MR, TCR, CSR, BSR, IDR, RST };
#define ICR_RST     0x80
#define ICR_AIP     0x40
#define ICR_LA      0x20
#define ICR_SEL     0x04
#define MR_ARB      0x01

static struct {
    uint8_t rd[8];
    uint8_t wr[8];
} scsi;

void scsi_reset(void)
{
    memset(&scsi, 0, sizeof(scsi));
}

static void scsi_update(void)
{
    if (scsi.wr[ICR] & ICR_RST) {
        memset(scsi.rd, 0, sizeof(scsi.rd));
        scsi.rd[ICR] = ICR_RST;
        scsi.rd[CSR] = 0x80;
        scsi.rd[BSR] = 0x10;
        return;
    }
    scsi.rd[ICR] = scsi.wr[ICR] & ~(ICR_AIP | ICR_LA);
    scsi.rd[CSR] &= ~0x80;
    if ((scsi.wr[MR] & MR_ARB) && (scsi.wr[CDR] & 0x80)) {
        /* Arbitration in progress, not lost, nobody else on the bus */
        scsi.rd[ICR] |= ICR_AIP;
        scsi.rd[CDR] = 0;
    }
    if (scsi.wr[ICR] & ICR_SEL) {
        scsi.rd[CSR] |= 0x02;
        scsi.rd[BSR] = 0x10;
    } else {
        scsi.rd[CSR] &= ~0x02;
    }
    scsi.rd[MR] = scsi.wr[MR];
    scsi.rd[TCR] = scsi.wr[TCR];
}

uint8_t scsi_read(uint32_t address)
{
    if (address & 1) {
        return 0;
    }
    return scsi.rd[(address >> 4) & 7];
}

void scsi_write(uint32_t address, uint8_t v)
{
    if (address & 1) {
        return;
    }
    scsi.wr[(address >> 4) & 7] = v;
    scsi_update();
}
