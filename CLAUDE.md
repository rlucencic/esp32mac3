# esp32mac3: Macintosh LC II and LC III on the M5Stack Tab5

Colour Macs with a 68030 and its MMU, chosen on the setup screen:

- **Macintosh LC II**: a 68030 at 15.67 MHz on the V8 system ASIC's 16-bit
  bus, built-in video at 640x480 in up to 256 colours or 512x384 in
  thousands, Egret (ADB, clock, PRAM), the V8's ASC, 4 MB on the board and
  a SIMM pair: 4, 6, 8 or 10 MB.
- **Macintosh LC III**: a 68030 at 25 MHz on the Sonora ASIC (a full 32-bit
  map, 1 MB of VRAM: thousands of colours at 640x480 too), Egret, Sonora's
  stereo ASC; 4, 8, 16 or 20 MB.

Both run Mac OS 7.6.1 (it needs 32-bit addressing, which they have).
Bluetooth keyboard and mouse, the touch screen as a mouse, disk images of
any size in three drives, an LVGL setup screen and a web page. ESP-IDF
v5.5.4, C.

The sibling of `../esp32mac2` (LC and II: a 68020, System 7.5.5), whose
core this one started as a copy of, and of `../esp32mac` (Plus, SE,
Classic: 68000, both boards). The board side (tasks, LCD, touch, SD, WiFi,
BLE, setup screen, web page, console) and the CPU core are the shared
library in `../esp32emu`; its CLAUDE.md has the runtime and the board-level
lessons. Changes there need all its projects built, for both boards (this
one for the Tab5). The 68030's MMU is Musashi's, rewritten for this project
(`../esp32emu/components/musashi/musashi/m68kmmu.h`, see its README).

## Build, flash, debug

The Tab5 only (`/dev/cu.usbmodem3101`): `CMakeLists.txt` stops a build for
any other target (the MaTouch's S3 can't run a 68020, let alone this).
`sdkconfig.defaults` is still the siblings' base (written for the S3); the
Tab5's settings from `../esp32emu` go on top.

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig build && \
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig -p /dev/cu.usbmodem3101 flash'
```

- `flash` also writes `roms/lc2.rom` and `roms/lc3.rom` (not in git; see
  `roms/README.md`) to partitions `rom_lc2` (512K) and `rom_lc3` (1M);
  `app-flash` only the app. The Tab5 is shared with the other projects:
  after another one, a full `flash` (partition table and ROMs). NVS (WiFi,
  BLE pairings) is shared; this project's settings are namespace `mac3`
  (`model`, `ram_mb`, `monitor`, `pram_lc2`, `pram_lc3`), its drives
  `mac3disks`.
- `CONFIG_M68K_68030=y` (`sdkconfig.defaults`) makes `../esp32emu`'s
  Musashi a 68030 with its MMU (the `M68K_CPU` choice; `-DM68K_68030` in the
  host build). Its FPU is always there (both Macs take a 68882 in a slot).
- `../esp32emu/tools/emu_console.py PORT log N | screenshot F.png | lcd F.png | send "cmd" ... | info | reset | profile ELF`
  with the IDF venv's Python (`-I`). `curl http://macintosh3.local/api/status`.
- Flashing restarts the board and ends the user's Mac session: say so first.
- Uploading a 256 MB disk image through the web page takes ~10 minutes
  (~430 KB/s).

Console commands: the shared ones (`emu/console.h`) and the Mac's
(`main/main.c`): `k <hex>` tap a Mac key code, `K <hex> 0|1`, `m dx dy btn`
relative mouse, `P x y btn` pointer at a screen position (`p` is the
runtime's profiler), `R <model 0|1> <ram MB> [monitor 0|1]` restart as the
LC II (4/6/8/10 MB) or the LC III (4/8/16/20 MB) with the 13" or 12"
monitor, `n` interrupt switch. Touches on the Tab5: the picture is 960x720
at x=160; setup screen Start `t 1065 637`, Model `t 440|795 251`, the
corner long-press `t 1270 10 2500`. The Memory and Monitor rows follow the
model (`choice_names()`); the Monitor buttons' second line is the colours
each monitor gets: LC II up to 256 at 640x480, 32,768 at 512x384; LC III
32,768 at both. A menu: `P 20 10 1`, `P 60 28 1`,
`P 62 29 0` (a second apart) chooses About This Computer.

## Layout

| Path | What |
|---|---|
| `main/main.c` | The Mac as an `emu_machine_t`: memory, start, the CPU speed controller, key map, touch, drives, status, console |
| `main/rom_store.c` | ROMs from their flash partitions, else matching files in the SD card's `roms/` |
| `main/settings.c` | NVS `mac3` |
| `main/web/macfiles.js` | From `../esp32mac` (LGPL): Mac archives -> HFS disk images in the browser |
| `components/mac3/` | The Macs, platform independent (`include/mac.h`) |
| `components/mac3/host/` | Host build for testing on the Mac (see Testing) |
| `roms/` | `lc2.rom`, `lc3.rom` (copyrighted, not distributed) |
| `partitions.csv` | nvs, phy, factory 8 MB, rom_lc2 512K, rom_lc3 1M (type 0x40), storage |

## The core (`components/mac3`)

Hardware behaviour from MAME's `maclc.cpp`, `v8.cpp`, `maclc3.cpp`,
`sonora.cpp`, `pseudovia.cpp`, `mv_sonora.cpp`, `asc.cpp` (BSD-3, R.
Belmont, O. Galibert; read, not copied), and the ROMs' own code.

Shared:
- `mac.c`: time in machine cycles (15.6672 MHz), "lines" of 704 cycles (one
  sound sample, 22.255 kHz), 370 a frame (60.15 Hz). Each of Musashi's
  cycles counts as `cpu_scale` / 256 of them: the model's own speed
  (`cpu_scale_native`: LC II 512 for its 16-bit bus, LC III 160 for 25 MHz
  on 32 bits) divided by `mac_set_cpu_speed()` (1/1024 of the model's).
  Vertical blanking from line 338. Interrupts: VIA1 1, the pseudo-VIA 2,
  SCC 4, NMI 7, the highest wins. The machine behind `model_ops_t`
  (`mac_internal.h`): slow paths, frame hooks, the picture.
- `mem.c`: the Musashi callbacks (physical addresses, after the MMU), fast
  paths the same for both: RAM below `ram_window` is `ram[a]`, ROM at
  `rom_base` + `rom_window`, VRAM at `vram_base` + `vram_window`; each
  16-bit word in host order (byte n at index n^1). The emulator's own
  accesses (`mac_peek*`, `mac_poke*`) take the Mac's logical addresses:
  RAM maps straight on both machines. `mac_ptr()` is a pointer as the Mac
  means it (24 bits in 24-bit mode, `MMU32Bit` at $CB2).
- `via.c` (VIA1), `pvia.c` (the pseudo-VIA: the V8's decodes A4/A1/A0 and
  has the RAM and video config; Sonora's decodes A4-A0, nothing on 0, 1 and
  the sense bits), `egret.c` (Egret, and VIA1's wiring: PA $D5 on the V8,
  $80 on Sonora, as MAME), `video.c` (the V8's 1024-byte lines through
  Ariel; Sonora's packed lines, its mode registers at $50F28000: mode $06
  13" / $02 12", bit 7 blank; depth; sense; beam position), `asc.c` (the
  V8's mono FIFO A, version $E8; Sonora's stereo FIFOs A and B, version
  $BC, status from both, IRQ masks at $F09/$F29), `adb.c`, `input.c`,
  `chips.c` (SCC, SWIM, SCSI stubs), `sony.c` (`../esp32mac`'s paravirtual
  disk driver over the ROM's `.Sony` at $6C3E0 in both ROMs), `rom.c` (the
  known ROMs: LC II 35C28F5F, LC III ECBBC41C; the checksum patches at
  $46AFC and $47A7A).

The LC II (`lc.c`): the V8 decodes A23-A0 (A31 reaches nothing); the ROM's
MMU tables put the ROM at $40A00000 and $A00000, the I/O at $50F00000 and
$F00000. RAM as MAME's V8 with 4 MB on the board: SIMMs at 0, the board's
RAM after the configured SIMM size, its first 2 MB always at $800000; 10 MB
is an 8 MB pair and 2 MB of the board's.

The LC III (`sonora.c`): RAM at 0 in one piece, ROM $40000000 (repeated),
I/O $50000000 (VIA1 every 256 KB, the rest every 1 MB), colour table
$50F24000, video registers $50F28000, ID $A55A0001 at $5FFFFFFC, VRAM
$60000000 (1 MB, repeated). In 32-bit mode the Mac sees the ROM at
$40800000 (ROMBase), in 24-bit mode at $800000.

## Lessons (each one cost a debugging session)

- **The fork's PMMU couldn't run these ROMs** ("Unhandled Table B mode
  2", no ATC, no TT registers, the 030's PMOVE TT0/TT1 read as SRP/CRP):
  rewritten (`m68kmmu.h`).
- **Apple turns the MMU on with `pmove (a3),tc; jmp (a5)`**: the `jmp` was
  fetched before the PMOVE, and its address means something else under the
  new tables (logical $A416B6 is RAM in 32-bit mode). Without that, the CPU
  slid through zeros from $A416B6 to $B00000. Fetches up to the end of the
  16-byte cache line keep the old mapping; a run of PMOVEs (SwapMMUMode:
  TT0, TT1, CRP, TC) keeps the first one's.
- **`PMOVEFD` and PLOAD matter**: SwapMMUMode moves CRP with FD so that the
  ATC still has the page holding the TC it loads next (PLOADed before);
  flushing it there made the TC operand read go through the half-switched
  tables. TT0/TT1 writes don't flush.
- **The disk driver's addresses** (dCtlDriver, the VBL task, the icon) were
  24-bit ROM addresses ($00A6C47E): RAM beyond the end in the LC II's
  32-bit mode, so the VBL task ran zeros. Now from ROMBase ($2AE) at Open.
- **Egret: a command crossing a packet.** The ROM checks PB3 just before
  raising system session; Egret may raise transceiver session in between
  (a tick packet). The ROM then sends its first byte, sees PB3 asserted,
  drops system session and reads the packet, taking its own byte's SR
  interrupt as the attention byte. egret.c took the Mac's system session
  for "reading the packet" and gave bytes into a shift-out SR: a hang on
  the second boot at 8 and 10 MB (timing).
- **The LC III's RAM sizing** finds the top of RAM in each 16 MB window and
  needs a power of two there (the real Sonora mirrors): RAM in one piece
  (as MAME has it) at 12 MB spun forever in the ROM's doubling loop at
  $408A022A. Hence 4, 8, 16, 20 MB (MAME offers 4/8/16/32/... too).
- **The MMU's cost**: every access, instruction words included, through
  `pmmu_translate_addr()` took 41% of core 1. An inline cache of 16 recent
  pages per function code and direction (`mmu_last`, m68kcpu.h) brought it
  to ~2%. Forcing the access helpers inline gained nothing.
- **The Tab5 runs about an LC II's worth of 68030**: the LC II at 92% of
  its speed on the idle Finder, the LC III at 27% of its own (6.9 MHz, the
  interpreter at ~85% of core 1). `main.c`'s controller (as esp32ste's
  Fastest) keeps the Mac's time real (sound, clock, mouse) and runs the
  CPU as fast as the frames still come in time, at most the model's speed.
- **The LC II ROM calls the LC II an LC** (BoxFlag $0D): its product table
  has one V8 record (VIA ID bits as on the LC, which MAME gives every V8).
  Mac OS 7.6.1's About This Computer says "Macintosh" for all three (LC,
  LC II, LC III), so it doesn't show.
- **The 12" monitor: black and white on the LC II, a hang on the LC III.**
  The LC II's ROM keeps its video PRAM ($56-$5A: board $0029, mode $58,
  $80 = 1 bit ... $84 = 16 bits, sense $5A) only while it matches the
  monitor, else resets the mode to $80: `mac_pram_monitor()` (`egret.c`,
  from every `mac_init()`) writes the 12" values ($A2, sense 2) when they
  don't. The LC III's ROM starts Sonora in the 13" mode ($86) whatever the
  monitor and waits for a VBL before it senses it: the VBL only ran in the
  monitor's own mode, so with the 12" it waited at $4080448C forever.
  `sonora_video_on()` is now true in any of Sonora's modes ($01 $02 $06
  $09 $0B); the picture is still drawn only in the monitor's own.
- **The setup screen's colour counts are what the ROMs accept**: PRAM $58
  set to $84 (16 bits) is kept by the LC II at 512x384 only (reset to $80
  at 640x480: 512 KB of VRAM), by the LC III at both (1 MB, as MAME's
  Sonora; a real LC III needs its VRAM SIMM for that at 640x480). The
  Monitor buttons say so per model (`choice_names()`, two lines).

## Testing

- **Host build**: `make -C components/mac3/host`, then
  `build/mac_host roms/lc2.rom|roms/lc3.rom [frames] [out.ppm] [ram_mb]`
  (native speed: no controller). Environment: `MAC_DISK1..3`,
  `MAC_PRAM=<file>`, `MAC_SCRIPT=<file>` (`<frame> key|type|mouse|pos|shot|
  insert|eject|peek|dis|free|units ...`), `MAC_STATUS=<n>`,
  `MAC_TRACE=<pc>[:<n>]` (+ `MAC_TRACE_ALL`: every register),
  `MAC_RING=<pc>[-<end>]|0:<n>` (+ `MAC_RING_JUMPS`: the jumps before a PC,
  or before the first slide through zeros), `MAC_DIS=<addr>[:<n>]`,
  `MAC_PCHIST=[<from>:]<to>`, `MAC_REGS`, `MAC_IO=1|<hex block mask>`,
  `MAC_EGRET`, `MAC_MONITOR=12`, `MAC_WAV=<file>`, `MAC_DEBUG=<port>`. MMU
  faults are logged ("68030 MMU: ..."). Runs ~20x (LC III) to ~60x (LC II)
  real time.
- Checked on the host (2026-10-18): Mac OS 7.6.1 from an uncompressed copy
  of the user's image boots to the Finder in 256 colours on the LC II at 4,
  6, 8, 10 MB and the LC III at 4, 8, 16, 20 MB (the free RAM matches); a
  scripted click opens About This Computer; both startup chimes play
  (~2 s). The first start with fresh PRAM restarts once (7.6 turns on
  32-bit addressing).
- Measured on the Tab5 (2026-10-18), 7.6.1 from the SD card, idle Finder:
  LC II 8 MB 100% real time, CPU 14.5 MHz (92%), core 1 ~85%; LC III 8 and
  20 MB 100% real time, CPU 6.9 MHz (27%), core 1 ~84%; 20 MB leaves 3.3 MB
  of PSRAM (3.1 MB with the setup screen open). The setup screen's rows,
  the pointer (`P`), About This Computer (8,192K on the LC II) checked.
  Not checked by ear or hand: sound, Bluetooth, the Tab5 Keyboard; not
  tried on the board: the 12" monitor, other depths.
- Checked on the host (2026-10-18): both models start in 256 colours on
  either monitor (13" -> 12" -> 13"); thousands draw right on the LC III at
  both sizes and on the LC II at 512x384.
- Checked on the Tab5 (2026-10-18): the setup screen's two-line Monitor
  buttons for the LC II and the LC III (`docs/images/tab5-setup.png`).
- **Free disk space** (web page): each drive's HFS/MFS/HFS+ volume
  (`mac_disk_space()`, esp32emu's `diskspace`), counted in `sony_frame()`
  at insert and a second after the last write. Tab5 (2026-10-21):
  a 256 MB Mac OS 7.6.1 disk 114118656 of 255987712.
- **`.dmg` chunks** (2026-10-21): `hdiutil` images of an HFS+ volume with
  files, bare and behind an Apple partition map, read through
  `MAC_DUMP=1:<file>` (the disk as the Mac sees it): UDRO and UDRW equal
  to the raw (UDTO) image byte for byte, the free space the same; UDZO
  refused ("a compressed .dmg: hdiutil convert -format UDRO"). A UDRO
  used to be taken as its data fork, the zeros left out: a wrong disk.

## Open items

- The CPU's speed on the board: a faster interpreter (a direct opcode fetch
  from RAM/ROM pages, say) would help the LC III most.
- The LC II's BoxFlag (an LC's, see the lessons).
- Sound volume (DFAC via Egret), the ASC's volume registers: ignored.
- No SCSI devices, serial ports, floppy hardware (disk images instead), no
  PDS cards; MMU faults don't raise exceptions (no virtual memory).
- `cpu_scale_native` is a guess for both; a Speedometer run against real
  figures would settle it.
## Working with the user

- Expects changes built, flashed and checked on the board, then a short
  report of what was verified and what wasn't.
- Keep their private data (WiFi name, IP, SD card contents) out of
  anything meant for publishing. Test disks are copies in the scratchpad,
  never their originals.
