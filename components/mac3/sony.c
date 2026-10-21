/*
 * The disk driver: a replacement for the ROM's .Sony driver, written over
 * it in the ROM copy. Small 68000 stubs call native routines here through
 * ILLEGAL ($4AFC) + function word (honoured only inside this driver), so
 * disk images of any size are read and written block by block, with no
 * IWM, GCR or SCSI emulation.
 *
 *   Open      allocates the drive queue elements and a VBL task, adds
 *             drives 1 .. MAC_DRIVES with _AddDrive, installs the task
 *   Prime     reads / writes 512-byte blocks at the DCE's position
 *   Control   eject, format, verify, ...; Status: drive status, formats
 *   VBL task  every 1/4 s: posts a disk-inserted event for a new disk
 *
 * Each drive's record in the Open block: writeProt, diskInPlace (1-2 a
 * floppy, 8 a fixed disk: anything bigger than 1.4 MB, so the Installer
 * and Finder treat hard disk and CD images as such), installed, sides (the bytes the system expects just before a Sony drive
 * queue element), then the drive queue element (qType 1: the size in
 * blocks in dQDrvSz/dQDrvSz2).
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "m68k.h"
#include "diskspace.h"
#include "mac_internal.h"

enum {
    FN_OPEN = 1, FN_NEXT_DRIVE, FN_VBL_TASK, FN_PRIME, FN_CONTROL, FN_STATUS, FN_POLL,
};

/* Mac OS error codes */
#define noErr           0
#define controlErr      (-17)
#define statusErr       (-18)
#define openErr         (-23)
#define ioErr           (-36)
#define wPrErr          (-44)
#define paramErr        (-50)
#define nsDrvErr        (-56)
#define offLinErr       (-65)

#define SONY_REFNUM     (-5)
#define RECORD_SIZE     24
#define DQE_OFFSET      4
#define VBL_OFFSET      (MAC_DRIVES * RECORD_SIZE)
#define DCE_OFFSET      (VBL_OFFSET + 24)   /* the .Disk driver's DCE */
#define DCE_SIZE        40
#define HANDLE_OFFSET   (DCE_OFFSET + DCE_SIZE)     /* its master pointer */
#define SONYVARS_OFFSET (HANDLE_OFFSET + 4)
#define SONYVARS_SIZE   0x400       /* scratch for whoever writes "Sony variables" */
#define BLOCK_SIZE      (SONYVARS_OFFSET + SONYVARS_SIZE)
#define LM_UTABLEBASE   0x011C
#define LM_UNITNTRYCNT  0x01D2
#define VBL_TICKS       15
#define REPOST_FRAMES   120         /* announce an unread disk again after 2 s */
#define MAX_POSTS       60
#define DISK_EVT        7
#define LM_EVENTQUEUE   0x014A

/* Parameter block, DCE and low memory offsets */
#define IO_TRAP         6
#define IO_RESULT       16
#define IO_VREFNUM      22
#define CS_CODE         26
#define CS_PARAM        28
#define IO_BUFFER       32
#define IO_REQCOUNT     36
#define IO_ACTCOUNT     40
#define DCTL_QFLAGS     6
#define DCTL_POSITION   16
#define LM_SONYVARS     0x0134
#define LM_DSKERR       0x0142
#define LM_TAGDATA      0x02FC
#define LM_ROMBASE      0x02AE

/*
 * The driver, hand-assembled (byte offsets on the left). Native calls are
 * $4AFC, function. Two headers share the routines: ".Sony" (floppy-sized
 * images) and ".Disk" (bigger ones, a hard disk to the system: the
 * Installer, for one, treats any volume on .Sony as a floppy and ejects
 * it when it looks for its next "disk").
 */
#define DRV_DISK        0x18        /* the .Disk header */
#define DRV_VBL         0x9E
static const uint16_t driver[] = {
    /* 00 .Sony header */
    0x4F00,                 /* flags: needLock, status, control, write, read */
    0x0000, 0x0000, 0x0000, /* delay, event mask, menu */
    0x0030, 0x005E, 0x0064, 0x0072, 0x009A,     /* open prime control status close */
    0x052E, 0x536F, 0x6E79, /* ".Sony" */
    /* 18 .Disk header: opened by the native Open (no open routine of its own) */
    0x4F00,
    0x0000, 0x0000, 0x0000,
    0x009C, 0x0046, 0x004C, 0x005A, 0x0082,     /* (relative to $18) */
    0x052E, 0x4469, 0x736B, /* ".Disk" */
    /* 30 open: A0 = parameter block, A1 = DCE */
    0x48E7, 0x60E0,         /* movem.l d1-d2/a0-a2,-(sp) */
    0x203C, 0x0000, BLOCK_SIZE,     /* move.l #BLOCK_SIZE,d0 */
    0xA71E,                 /* _NewPtr,sys,clear */
    0x4AFC, FN_OPEN,        /* native: set up the block (A0), .Disk; d0 = error */
    0x4A80,                 /* tst.l d0 */
    0x6614,                 /* bne.s done (58) */
    /* 44 next: */
    0x4AFC, FN_NEXT_DRIVE,  /* native: d0 = drive.w:refnum.w, a0 = element; 0 = done */
    0x4A80,                 /* tst.l d0 */
    0x6704,                 /* beq.s vbl (50) */
    0xA04E,                 /* _AddDrive */
    0x60F4,                 /* bra.s next (44) */
    /* 50 vbl: */
    0x4AFC, FN_VBL_TASK,    /* native: a0 = VBL task */
    0xA033,                 /* _VInstall */
    0x7000,                 /* moveq #0,d0 */
    /* 58 done: */
    0x4CDF, 0x0706,         /* movem.l (sp)+,d1-d2/a0-a2 */
    0x4E75,                 /* rts */
    /* 5E prime: */
    0x4AFC, FN_PRIME,
    0x6012,                 /* bra.s ioreturn (76) */
    /* 64 control: */
    0x4AFC, FN_CONTROL,
    0x0C68, 0x0001, 0x001A, /* cmpi.w #1,csCode(a0): KillIO returns directly */
    0x6606,                 /* bne.s ioreturn (76) */
    0x4E75,                 /* rts */
    /* 72 status: */
    0x4AFC, FN_STATUS,
    /* 76 ioreturn: immediate calls set ioResult and return, queued ones go
     * through IODone (unless still in progress, d0 > 0) */
    0x3228, 0x0006,         /* move.w ioTrap(a0),d1 */
    0x0801, 0x0009,         /* btst #9,d1 (noQueue) */
    0x670C,                 /* beq.s queued (8C) */
    0x4A40,                 /* tst.w d0 */
    0x6F02,                 /* ble.s imm (86) */
    0x4240,                 /* clr.w d0 */
    /* 86 imm: */
    0x3140, 0x0010,         /* move.w d0,ioResult(a0) */
    0x4E75,                 /* rts */
    /* 8C queued: */
    0x4A40,                 /* tst.w d0 */
    0x6F04,                 /* ble.s iodone (94) */
    0x4240,                 /* clr.w d0 */
    0x4E75,                 /* rts */
    /* 94 iodone: */
    0x2F38, 0x08FC,         /* move.l JIODone,-(sp) */
    0x4E75,                 /* rts */
    /* 9A close: */
    0x70E8,                 /* moveq #closErr,d0 */
    0x4E75,                 /* rts */
    /* 9E VBL task: A0 = the task */
    0x317C, VBL_TICKS, 0x000A,      /* move.w #VBL_TICKS,vblCount(a0) */
    0x4AFC, FN_POLL,        /* native: d0 = drive with a new disk, 0 = none */
    0x4A80,                 /* tst.l d0 */
    0x6706,                 /* beq.s B2 */
    0x307C, 0x0007,         /* movea.w #diskEvt,a0 */
    0xA02F,                 /* _PostEvent */
    /* B2 */
    0x4E75,                 /* rts */
    /* B4 .Disk open (never called: it's opened already) */
    0x7000,                 /* moveq #0,d0 */
    0x4E75,                 /* rts */
};

/* The hard disk icon the driver gives the Finder (Control 21/22), as Apple's
 * SCSI hard disk driver did: a drive seen from above at an angle, with its
 * light. Icon, then mask (ICN# layout), then a "where" string for Get Info.
 */
static const uint32_t hd_icon[64] = {
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x0FFFFFFF, 0x10000003, 0x20000005, 0x40000009,
    0xFFFFFFF1, 0x80000011, 0x80000011, 0x80000011,
    0x80000011, 0x80000011, 0x9F000012, 0x9F000014,
    0x80000018, 0xFFFFFFF0, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x0FFFFFFF, 0x1FFFFFFF, 0x3FFFFFFF, 0x7FFFFFFF,
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFC,
    0xFFFFFFF8, 0xFFFFFFF0, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};
static const char hd_where[] = "\x0A" "Disk image";

typedef struct {
    FILE *file;
    char path[192];
    uint64_t data_offset;           /* Disk Copy 4.2 header, partition */
    uint64_t size;
    bool read_only;
    bool posted;                    /* the Mac was told about it */
    uint32_t reads, writes;
    uint32_t last_access;           /* frame */
    bool accessed;
    uint32_t posted_frame;          /* when the disk-inserted event went out */
    uint32_t reads_at_post;
    int posts;
    uint32_t last_write;            /* frame */
    bool space_due;                 /* free space: to be worked out */
    uint32_t space_writes;          /* the write count it was worked out at */
    volatile bool space_known;
    volatile uint64_t space_free, space_size;
} drive_t;

static drive_t s_drives[MAC_DRIVES];
/* Bigger than a 1.4 MB floppy: presented as a fixed (non-ejectable) disk.
 * An ejectable volume of the wrong name gets ejected by the Installer when
 * it looks for its next "disk", where a fixed one is searched for folders.
 */
static bool fixed_disk(const drive_t *d)
{
    return d->size > 1440 * 1024;
}

static uint32_t s_block;            /* the Open block in Mac memory, 0 before Open */
static int16_t s_disk_refnum;       /* the .Disk driver's, 0 if it couldn't be installed */
static uint32_t s_hd_icon;          /* the icon's Mac address (in the ROM, after the driver) */
static int s_next_drive;
static uint32_t s_driver;           /* driver's Mac address */
static uint32_t s_rom_mac_base;     /* the ROM's 24-bit address (until Open) */
static int s_light = -1;
static uint8_t s_io[4096];

bool sony_install(uint8_t *rom, uint32_t rom_size, uint32_t offset, uint32_t rom_mac_base)
{
    uint32_t bytes = sizeof(driver);
    if (offset + bytes + sizeof(hd_icon) + sizeof(hd_where) > rom_size) {
        return false;
    }
    for (size_t i = 0; i < sizeof(driver) / 2; i++) {
        *(uint16_t *)(rom + offset + i * 2) = driver[i];
    }
    /* The icon after it, as host-order words like the rest of the ROM copy */
    uint32_t icon = offset + bytes;
    for (size_t i = 0; i < 64; i++) {
        *(uint16_t *)(rom + icon + i * 4) = hd_icon[i] >> 16;
        *(uint16_t *)(rom + icon + i * 4 + 2) = hd_icon[i] & 0xFFFF;
    }
    for (size_t i = 0; i < sizeof(hd_where); i++) {
        rom[(icon + sizeof(hd_icon) + i) ^ 1] = (uint8_t)hd_where[i];
    }
    s_rom_mac_base = rom_mac_base;
    s_hd_icon = rom_mac_base + icon;
    mac.driver_start = offset;
    mac.driver_end = offset + bytes;
    s_driver = rom_mac_base + offset;
    return true;
}

void sony_reset(void)
{
    s_block = 0;
    s_disk_refnum = 0;
    s_next_drive = 0;
    for (int i = 0; i < MAC_DRIVES; i++) {
        s_drives[i].posted = false;
    }
}

/* ---- Drive records in Mac memory ---- */

static uint32_t record(int i)
{
    return s_block + i * RECORD_SIZE;
}

static void update_record(int i)
{
    if (s_block == 0) {
        return;
    }
    drive_t *d = &s_drives[i];
    uint32_t r = record(i);
    bool in = d->file != NULL && d->posted;
    uint32_t blocks = in ? (uint32_t)(d->size / 512) : 0;
    mac_poke8(r + 0, in && d->read_only ? 0x80 : 0);
    if (!in) {
        mac_poke8(r + 1, 0);
    } else if (fixed_disk(d)) {
        mac_poke8(r + 1, 8);
    } else if (mac_peek8(r + 1) == 0 || mac_peek8(r + 1) == 8) {
        mac_poke8(r + 1, 1);
    }
    mac_poke8(r + 2, 1);                    /* installed */
    mac_poke8(r + 3, 0x80);                 /* two-sided */
    uint32_t q = r + DQE_OFFSET;
    mac_poke16(q + 4, 1);                   /* qType: size in dQDrvSz/dQDrvSz2 */
    mac_poke16(q + 6, i + 1);               /* dQDrive */
    mac_poke16(q + 8, (uint16_t)(in && fixed_disk(d) && s_disk_refnum ? s_disk_refnum : SONY_REFNUM));
    mac_poke16(q + 10, 0);                  /* dQFSID */
    mac_poke16(q + 12, blocks & 0xFFFF);
    mac_poke16(q + 14, blocks >> 16);
}

/* The drive a parameter block names (ioVRefNum), or NULL */
static drive_t *pb_drive(uint32_t pb, int *index)
{
    int n = (int16_t)mac_peek16(pb + IO_VREFNUM);
    if (n < 1 || n > MAC_DRIVES) {
        return NULL;
    }
    *index = n - 1;
    return &s_drives[n - 1];
}

static bool present(const drive_t *d)
{
    return d->file != NULL && d->posted;
}

static void accessed(int i)
{
    s_drives[i].last_access = mac.frames;
    s_drives[i].accessed = true;
    if (s_block && !fixed_disk(&s_drives[i])) {
        mac_poke8(record(i) + 1, 2);        /* disk in place, accessed */
    }
}

/* ---- Native routines ---- */

/* The .Disk driver: a DCE (in the Open block) for the second header, marked
 * open, in a free unit of the unit table
 */
static void install_disk_driver(void)
{
    /* The last unit of the table: not 32-39, which system code takes for
     * Apple's SCSI drivers and looks into
     */
    uint32_t table = mac_ptr(mac_peek32(LM_UTABLEBASE));
    int count = (int16_t)mac_peek16(LM_UNITNTRYCNT);
    int unit = count - 1;
    if (table == 0 || unit < 40 || mac_peek32(table + unit * 4) != 0) {
        MAC_LOGW("no free unit for the .Disk driver: big disks look like floppies");
        return;
    }
    s_disk_refnum = -(unit + 1);
    uint32_t dce = s_block + DCE_OFFSET, handle = s_block + HANDLE_OFFSET;
    for (int i = 0; i < DCE_SIZE; i += 2) {
        mac_poke16(dce + i, 0);
    }
    mac_poke32(dce + 0, s_driver + DRV_DISK);   /* dCtlDriver: a pointer (ROM) */
    mac_poke16(dce + 4, 0x4F00 | 0x0020);       /* dCtlFlags: as the header, opened */
    mac_poke16(dce + DCTL_QFLAGS, 3);           /* driver version */
    mac_poke16(dce + 24, (uint16_t)s_disk_refnum);  /* dCtlRefNum */
    mac_poke32(handle, dce);
    mac_poke32(table + unit * 4, handle);
}

static int fn_open(void)
{
    uint32_t block = mac_ptr(m68k_get_reg(NULL, M68K_REG_A0));
    if (block == 0) {
        return openErr;
    }
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t dce = mac_ptr(mac_peek32(sp + 12));  /* the saved A1 */
    mac_poke32(dce + DCTL_POSITION, 0);
    mac_poke16(dce + DCTL_QFLAGS, (mac_peek16(dce + DCTL_QFLAGS) & 0xFF00) | 3);
    /* SonyVars: system code writes into the real driver's variables (it
     * overwrote the first drive queue element when this pointed at the
     * block): a scratch area of their own
     */
    mac_poke32(LM_SONYVARS, block + SONYVARS_OFFSET);
    mac_poke16(LM_DSKERR, 0);
    /* The addresses the system keeps (dCtlDriver, the VBL task, the icon)
     * from ROMBase, the 32-bit one: it reaches the ROM in both modes. The
     * 24-bit one is RAM in the LC II's 32-bit mode (its MMU maps it that way).
     */
    uint32_t rom_base = mac_peek32(LM_ROMBASE);
    if ((rom_base & 0xF00000) == s_rom_mac_base) {
        s_driver = rom_base + mac.driver_start;
        s_hd_icon = rom_base + mac.driver_end;
    }
    s_block = block;
    install_disk_driver();
    s_next_drive = 0;
    for (int i = 0; i < MAC_DRIVES; i++) {
        s_drives[i].posted = false;
        update_record(i);
    }
    return noErr;
}

static int fn_next_drive(void)
{
    if (s_next_drive >= MAC_DRIVES) {
        return 0;
    }
    int i = s_next_drive++;
    m68k_set_reg(M68K_REG_A0, record(i) + DQE_OFFSET);
    return (int)(((uint32_t)(i + 1) << 16) | (uint16_t)SONY_REFNUM);
}

static int fn_vbl_task(void)
{
    uint32_t v = s_block + VBL_OFFSET;
    mac_poke32(v + 0, 0);                   /* qLink */
    mac_poke16(v + 4, 1);                   /* qType: vType */
    mac_poke32(v + 6, s_driver + DRV_VBL);  /* vblAddr */
    mac_poke16(v + 10, VBL_TICKS);          /* vblCount */
    mac_poke16(v + 12, 0);                  /* vblPhase */
    m68k_set_reg(M68K_REG_A0, v);
    return noErr;
}

/* A disk-inserted event for this drive still waits in the Mac's event queue */
static bool event_waiting(int drive)
{
    uint32_t e = mac_ptr(mac_peek32(LM_EVENTQUEUE + 2));
    for (int n = 0; e != 0 && n < 64; n++) {
        if (mac_peek16(e + 6) == DISK_EVT && (mac_peek32(e + 8) & 0xFFFF) == (uint32_t)drive + 1) {
            return true;
        }
        e = mac_ptr(mac_peek32(e));
    }
    return false;
}

/* From the VBL task: a drive whose disk the Mac should hear about. Events
 * posted while the system starts up can be flushed unseen: a disk nobody
 * read since its event, with no event of its own waiting, is announced
 * again.
 */
static int fn_poll(void)
{
    if (s_block == 0) {
        return 0;
    }
    for (int i = 0; i < MAC_DRIVES; i++) {
        drive_t *d = &s_drives[i];
        if (d->file == NULL) {
            continue;
        }
        bool again = d->posted && d->posts < MAX_POSTS && d->reads == d->reads_at_post &&
                     mac.frames - d->posted_frame >= REPOST_FRAMES && !event_waiting(i);
        if (!d->posted || again) {
            if (!d->posted) {
                d->posts = 0;
            }
            d->posted = true;
            d->posts++;
            d->posted_frame = mac.frames;
            d->reads_at_post = d->reads;
            update_record(i);
            return i + 1;
        }
    }
    return 0;
}

static int set_result(int err)
{
    mac_poke16(LM_DSKERR, (uint16_t)err);
    return err;
}

/* Errors are rare and worth seeing: logged with the call (prime/control/status) */
static int failed(const char *call, uint32_t pb, int code, int err)
{
    MAC_LOGW("disk %s (drive %d, code %d) -> error %d", call, (int16_t)mac_peek16(pb + IO_VREFNUM),
             code, err);
    return set_result(err);
}

static int fn_prime(void)
{
    uint32_t pb = mac_ptr(m68k_get_reg(NULL, M68K_REG_A0));
    uint32_t dce = mac_ptr(m68k_get_reg(NULL, M68K_REG_A1));
    mac_poke32(pb + IO_ACTCOUNT, 0);
    int i;
    drive_t *d = pb_drive(pb, &i);
    if (d == NULL) {
        return failed("prime", pb, 0, nsDrvErr);
    }
    if (!present(d)) {
        return set_result(offLinErr);   /* the system polls empty drives: not worth a log line */
    }
    bool write = (mac_peek16(pb + IO_TRAP) & 0xFF) == 3;
    uint32_t buffer = mac_ptr(mac_peek32(pb + IO_BUFFER));
    uint32_t count = mac_peek32(pb + IO_REQCOUNT);
    uint32_t position = mac_peek32(dce + DCTL_POSITION);
    if ((count | position) & 511) {
        return failed("prime", pb, (int)write, paramErr);
    }
    if ((uint64_t)position + count > d->size || !mac_ram_range(buffer, count)) {
        return failed("prime", pb, (int)write, paramErr);
    }
    if (write && d->read_only) {
        return failed("prime", pb, (int)write, wPrErr);
    }
    accessed(i);
    if (fseeko(d->file, (off_t)(d->data_offset + position), SEEK_SET) != 0) {
        return failed("prime", pb, (int)write, ioErr);
    }
    for (uint32_t done = 0; done < count;) {
        uint32_t n = count - done < sizeof(s_io) ? count - done : sizeof(s_io);
        if (write) {
            mac_ram_read(buffer + done, s_io, n);
            if (fwrite(s_io, 1, n, d->file) != n) {
                return failed("prime", pb, (int)write, ioErr);
            }
        } else {
            if (fread(s_io, 1, n, d->file) != n) {
                return failed("prime", pb, (int)write, ioErr);
            }
            mac_ram_write(buffer + done, s_io, n);
        }
        done += n;
    }
    if (write) {
        fflush(d->file);
        d->writes += count / 512;
        d->last_write = mac.frames;
    } else {
        d->reads += count / 512;
        for (int k = 0; k < 12; k += 2) {
            mac_poke16(LM_TAGDATA + k, 0);  /* no tags */
        }
    }
    mac_poke32(pb + IO_ACTCOUNT, count);
    mac_poke32(dce + DCTL_POSITION, position + count);
    return set_result(noErr);
}

static void close_drive(int i)
{
    drive_t *d = &s_drives[i];
    if (d->file != NULL) {
        fclose(d->file);
    }
    d->file = NULL;
    d->path[0] = 0;
    d->posted = false;
    d->size = 0;
    d->space_known = false;
    update_record(i);
}

/* Control 23: bits 0-3 drive type (1 unknown, 4 1.4 MB SuperDrive),
 * bit 8 external
 */
static uint32_t drive_info(int i, const drive_t *d)
{
    uint32_t external = i == 0 ? 0 : 0x0100;
    return external | (d->file != NULL && fixed_disk(d) ? 0x0001 : 0x0004);
}

static int fn_control(void)
{
    uint32_t pb = mac_ptr(m68k_get_reg(NULL, M68K_REG_A0));
    int code = mac_peek16(pb + CS_CODE);
    switch (code) {
    case 1:     /* KillIO */
    case 8:     /* set tag buffer */
    case 9:     /* track cache */
    case 65:    /* accRun */
        return set_result(noErr);
    }
    int i;
    drive_t *d = pb_drive(pb, &i);
    if (d == NULL) {
        return failed("control", pb, code, nsDrvErr);
    }
    switch (code) {
    case 5:     /* verify */
        return present(d) ? set_result(noErr) : failed("control", pb, code, offLinErr);
    case 6:     /* format: the image is already a blank disk of its size */
        if (!present(d)) {
            return failed("control", pb, code, offLinErr);
        }
        return d->read_only ? failed("control", pb, code, wPrErr) : set_result(noErr);
    case 7:     /* eject */
        if (d->file != NULL) {
            MAC_LOGI("drive %d: ejected %s", i + 1, d->path);
            close_drive(i);
            if (mac.disk_ejected) {
                mac.disk_ejected(i);
            }
        }
        return set_result(noErr);
    case 21:    /* drive icon */
    case 22:    /* media icon: what the Finder shows on the desktop */
        if (!fixed_disk(d)) {
            return set_result(controlErr);  /* the system's floppy icon */
        }
        mac_poke32(pb + CS_PARAM, s_hd_icon);
        return set_result(noErr);
    case 23:    /* drive info */
        mac_poke32(pb + CS_PARAM, drive_info(i, d));
        return set_result(noErr);
    default:
        return failed("control", pb, code, controlErr);
    }
}

static int fn_status(void)
{
    uint32_t pb = mac_ptr(m68k_get_reg(NULL, M68K_REG_A0));
    int code = mac_peek16(pb + CS_CODE);
    int i;
    drive_t *d = pb_drive(pb, &i);
    if (d == NULL) {
        return failed("status", pb, code, nsDrvErr);
    }
    switch (code) {
    case 6: {   /* the formats a disk can have: its own size */
        if (!present(d)) {
            return failed("status", pb, code, offLinErr);
        }
        if ((int16_t)mac_peek16(pb + CS_PARAM) < 1) {
            return failed("status", pb, code, paramErr);
        }
        uint32_t list = mac_ptr(mac_peek32(pb + CS_PARAM + 2));
        uint32_t blocks = (uint32_t)(d->size / 512);
        mac_poke16(pb + CS_PARAM, 1);
        mac_poke32(list, blocks);
        /* current disk's format; floppy sizes also give their geometry */
        uint32_t flags = 0x40000000;
        if (blocks == 800) {
            flags = 0xC1000050;     /* one side, 80 tracks */
        } else if (blocks == 1600) {
            flags = 0xC2000050;     /* two sides, 80 tracks */
        } else if (blocks == 2880) {
            flags = 0xD2120050;     /* high density, two sides, 18 sectors, 80 tracks */
        }
        mac_poke32(list + 4, flags);
        return set_result(noErr);
    }
    case 8: {   /* drive status */
        uint32_t r = record(i), out = pb + CS_PARAM;
        update_record(i);
        mac_poke16(out + 0, 0);                         /* track */
        for (int k = 0; k < 4; k++) {
            mac_poke8(out + 2 + k, mac_peek8(r + k));   /* writeProt .. sides */
        }
        for (int k = 0; k < 12; k += 2) {
            mac_poke16(out + 6 + k, mac_peek16(r + DQE_OFFSET + k));
        }
        mac_poke8(out + 18, 0xFF);                      /* two-sided format */
        mac_poke8(out + 19, 0);                         /* needs flush */
        mac_poke16(out + 20, 0);                        /* errors */
        return set_result(noErr);
    }
    default:
        return failed("status", pb, code, statusErr);
    }
}

int sony_native(uint16_t function)
{
    switch (function) {
    case FN_OPEN:       return fn_open();
    case FN_NEXT_DRIVE: return fn_next_drive();
    case FN_VBL_TASK:   return fn_vbl_task();
    case FN_PRIME:      return fn_prime();
    case FN_CONTROL:    return fn_control();
    case FN_STATUS:     return fn_status();
    case FN_POLL:       return fn_poll();
    default:
        MAC_LOGW("unknown disk driver call %u", function);
        return paramErr;
    }
}

/* ---- Free space, for the web page ----
 *
 * Worked out here on the emulation task, which owns the files: at insert
 * and a second after the last write. The Mac writes its volume's counts
 * back from time to time, so they may lag a little behind.
 */

#define SPACE_FRAMES    60

static bool space_read(void *ctx, uint64_t offset, void *buf, uint32_t len)
{
    drive_t *d = ctx;
    return offset + len <= d->size && fseeko(d->file, (off_t)(d->data_offset + offset), SEEK_SET) == 0 &&
           fread(buf, 1, len, d->file) == len;
}

static void space_update(drive_t *d)
{
    diskspace_disk_t disk = { space_read, d, d->size };
    diskspace_t s = { 0, 0 };
    bool known = diskspace_mac_disk(&disk, &s);
    d->space_known = false;
    d->space_free = s.free;
    d->space_size = s.size;
    d->space_known = known;
}

bool mac_disk_space(int drive, uint64_t *free_bytes, uint64_t *size)
{
    if (drive < 0 || drive >= MAC_DRIVES || !s_drives[drive].space_known) {
        return false;
    }
    *free_bytes = s_drives[drive].space_free;
    *size = s_drives[drive].space_size;
    return true;
}

/* Drive light: on for ~0.3 s after an access; the free space */
void sony_frame(void)
{
    int light = -1;
    for (int i = 0; i < MAC_DRIVES; i++) {
        drive_t *d = &s_drives[i];
        if (d->accessed && mac.frames - d->last_access < 20) {
            light = i;
        }
        if (d->file != NULL &&
            (d->space_due || (d->writes != d->space_writes && mac.frames - d->last_write >= SPACE_FRAMES))) {
            d->space_due = false;
            d->space_writes = d->writes;
            space_update(d);
        }
    }
    s_light = light;
}

/* ---- Images ---- */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

bool mac_disk_insert(int drive, const char *path)
{
    if (drive < 0 || drive >= MAC_DRIVES) {
        return false;
    }
    mac_disk_eject(drive);
    drive_t *d = &s_drives[drive];
    /* CD images are locked, as CDs are; so are .dmg images (writing would
     * leave their checksum wrong, and a Mac wouldn't open them any more)
     */
    static const char *const cd[] = { ".iso", ".toast", ".cdr", ".dmg" };
    bool read_only = false;
    size_t len = strlen(path);
    for (size_t i = 0; i < sizeof(cd) / sizeof(cd[0]); i++) {
        size_t n = strlen(cd[i]);
        read_only |= len > n && strcasecmp(path + len - n, cd[i]) == 0;
    }
    FILE *f = read_only ? NULL : fopen(path, "r+b");
    if (f == NULL) {
        f = fopen(path, "rb");
        read_only = true;
    }
    if (f == NULL) {
        MAC_LOGW("can't open %s", path);
        return false;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size < 512) {
        fclose(f);
        return false;
    }
    uint64_t file_size = st.st_size;
    d->data_offset = 0;
    d->size = file_size & ~511ull;

    /* Disk Copy 4.2: 84-byte header, data size at $40, magic $0100 at $52 */
    uint8_t h[84];
    if (fread(h, 1, sizeof(h), f) == sizeof(h) && h[0] < 64 && h[0x52] == 0x01 && h[0x53] == 0x00) {
        uint32_t data = be32(h + 0x40), tags = be32(h + 0x44);
        if (data > 0 && (data & 511) == 0 && 84ull + data + tags == file_size) {
            d->data_offset = 84;
            d->size = data;
        }
    }
    /* Disk images from a Mac (.dmg, UDIF): read-only or read-write ones
     * that aren't compressed have the disk as their data fork, and a
     * 512-byte "koly" block at the end says where
     */
    uint64_t base = 0;
    bool dmg = false;
    uint8_t k[512];
    if (d->data_offset == 0 && file_size >= 1024 && fseeko(f, (off_t)(file_size - 512), SEEK_SET) == 0 &&
        fread(k, 1, sizeof(k), f) == sizeof(k) && memcmp(k, "koly", 4) == 0) {
        uint64_t offset = (uint64_t)be32(k + 0x18) << 32 | be32(k + 0x1C);
        uint64_t length = (uint64_t)be32(k + 0x20) << 32 | be32(k + 0x24);
        uint8_t v[2] = { 0, 0 };
        bool raw = offset + length <= file_size - 512 && length >= 1024 &&
                   ((fseeko(f, (off_t)(offset + 1024), SEEK_SET) == 0 && fread(v, 1, 2, f) == 2 &&
                     ((v[0] == 'B' && v[1] == 'D') || (v[0] == 'H' && v[1] == '+'))) ||
                    (fseeko(f, (off_t)offset, SEEK_SET) == 0 && fread(v, 1, 2, f) == 2 && v[0] == 'E' && v[1] == 'R'));
        if (!raw) {
            MAC_LOGW("%s: a compressed .dmg (convert it to a read-only or read-write image)", path);
            fclose(f);
            return false;
        }
        base = d->data_offset = offset;
        dmg = true;
        d->size = length & ~511ull;
        if (fseeko(f, (off_t)offset, SEEK_SET) != 0 || fread(h, 1, sizeof(h), f) != sizeof(h)) {
            memset(h, 0, sizeof(h));
        }
    }
    /* Apple partition map (hard disk and CD images): the first Apple_HFS
     * partition is the disk
     */
    if ((d->data_offset == 0 || dmg) && h[0] == 'E' && h[1] == 'R') {
        uint8_t pm[512];
        int entries = 1;
        for (int i = 1; i <= entries && i < 64; i++) {
            if (fseeko(f, (off_t)(base + i * 512), SEEK_SET) != 0 || fread(pm, 1, sizeof(pm), f) != sizeof(pm) ||
                pm[0] != 'P' || pm[1] != 'M') {
                break;
            }
            entries = be32(pm + 4);
            uint64_t start = base + (uint64_t)be32(pm + 8) * 512, blocks = be32(pm + 12);
            if (strncasecmp((const char *)pm + 48, "Apple_HFS", 32) == 0 && start < file_size) {
                d->data_offset = start;
                d->size = blocks * 512 < file_size - start ? blocks * 512 : (file_size - start) & ~511ull;
                MAC_LOGI("drive %d: partition %.32s at block %lu", drive + 1, (const char *)pm + 16,
                         (unsigned long)(start / 512));
                break;
            }
        }
    }
    d->file = f;
    d->read_only = read_only;
    d->posted = false;
    d->space_known = false;
    d->space_due = true;
    snprintf(d->path, sizeof(d->path), "%s", path);
    MAC_LOGI("drive %d: %s, %llu KB%s%s%s", drive + 1, path, (unsigned long long)(d->size / 1024),
             d->data_offset == 84 ? ", Disk Copy 4.2" : "", dmg ? ", .dmg" : "", read_only ? ", locked" : "");
    return true;
}

void mac_disk_eject(int drive)
{
    if (drive >= 0 && drive < MAC_DRIVES && s_drives[drive].file != NULL) {
        close_drive(drive);
    }
}

const char *mac_disk_path(int drive)
{
    if (drive < 0 || drive >= MAC_DRIVES || s_drives[drive].file == NULL) {
        return NULL;
    }
    return s_drives[drive].path;
}

bool mac_disk_write_protected(int drive)
{
    return drive >= 0 && drive < MAC_DRIVES && s_drives[drive].file != NULL &&
           s_drives[drive].read_only;
}

uint64_t mac_disk_size(int drive)
{
    if (drive < 0 || drive >= MAC_DRIVES || s_drives[drive].file == NULL) {
        return 0;
    }
    return s_drives[drive].size;
}

bool mac_disk_create(const char *path, uint32_t bytes)
{
    if (bytes == 0 || (bytes & 511)) {
        return false;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    memset(s_io, 0, sizeof(s_io));
    const uint32_t ends = 64 * 1024;
    bool ok = true;
    for (uint32_t done = 0; done < bytes && ok;) {
        if (done == ends && bytes > 2 * ends) {
            /* Skip the middle: the file grows when written past its end */
            done = bytes - ends;
            ok = fseek(f, done, SEEK_SET) == 0;
            continue;
        }
        uint32_t n = bytes - done < sizeof(s_io) ? bytes - done : sizeof(s_io);
        ok = fwrite(s_io, 1, n, f) == n;
        done += n;
    }
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(path);
    }
    return ok;
}

void mac_disk_status(mac_disk_status_t *status)
{
    status->light = s_light;
    for (int i = 0; i < MAC_DRIVES; i++) {
        status->reads[i] = s_drives[i].reads;
        status->writes[i] = s_drives[i].writes;
    }
}
