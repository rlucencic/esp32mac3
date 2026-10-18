<h1 align="center">esp32mac3</h1>

<p align="center">
  <b>A Macintosh LC II or LC III on an M5Stack Tab5.</b><br>
  68030 with its MMU, 640×480 in 256 colours, up to 20 MB, Mac OS 7.6.1, disk images of any size, Bluetooth keyboard and mouse.
</p>

<p align="center">
  <img alt="ESP32-P4" src="https://img.shields.io/badge/ESP32--P4-M5Stack%20Tab5-e7352c">
  <img alt="ESP-IDF" src="https://img.shields.io/badge/ESP--IDF-v5.5-blue">
  <img alt="LVGL" src="https://img.shields.io/badge/LVGL-9.4-5e48e8">
  <img alt="68030" src="https://img.shields.io/badge/CPU-Musashi%2068030-555">
  <img alt="MIT licence" src="https://img.shields.io/badge/licence-MIT-green">
</p>

esp32mac3 turns an M5Stack Tab5 into a 68030 Mac, chosen on its setup
screen: the **Macintosh LC II** (the V8 chip's built-in video, Egret, the
Apple Sound Chip) or the **Macintosh LC III** (the Sonora chip: a 32-bit
machine with 1 MB of video RAM and stereo sound). Both show a 13-inch
(640×480) or 12-inch (512×384) monitor and run Mac OS 7.6.1. A Bluetooth
keyboard and mouse (or your finger on the screen) work the Mac, and disk
images on the microSD card go into three drives. A setup screen on the
touch panel, and a web page on your network, handle everything else.

It's the 68030 sibling of
[esp32mac2](https://github.com/rlucencic/esp32mac2) (Macintosh LC and II)
and [esp32mac](https://github.com/rlucencic/esp32mac) (the compact
black-and-white Macs), and lives next to the other machines on the shared
runtime [esp32emu](https://github.com/rlucencic/esp32emu).

## Features

**The machines**
- 68030 with its paged MMU and FPU
  ([Musashi](https://github.com/kstenerud/Musashi) core, its MMU rewritten
  for these ROMs): the ROMs build their own page tables and switch between
  24-bit and 32-bit addressing as on the real thing
- **Macintosh LC II**: 4, 6, 8 or 10 MB; built-in video at 640×480 in black
  and white up to 256 colours, or 512×384 up to thousands; Egret for the
  ADB keyboard and mouse, the clock and parameter RAM
- **Macintosh LC III**: 4, 8, 16 or 20 MB; 1 MB of video RAM (up to
  thousands of colours at 640×480); Sonora's stereo sound FIFOs
- 256 colours by default; the Monitors control panel changes it
- The startup chime, system beeps and sound through the Apple Sound Chip
- The clock is set from the internet; parameter RAM is kept across
  power-offs
- Always in real time (sound, clock, mouse): the 68030 runs as fast as the
  board keeps up, at most the model's own speed (see Limitations)

**Disks**
- **Three drives** for disk images of any size, through a fast replacement
  of the Mac's disk driver (from esp32mac)
- Raw images (`.dsk`, `.img`, `.hfv`), Disk Copy 4.2 images (`.image`),
  partitioned hard disk and CD images (`.hda`, `.iso`, `.toast`), and
  uncompressed `.dmg` images (read-only)
- Mac archives dropped on the web page (StuffIt, Compact Pro, BinHex,
  MacBinary) become disk images in your browser, as in esp32mac
- New blank disks from the setup screen or the web page

**Around it**
- Touch screen as mouse; setup screen (hold the top-right corner for 2 s);
  Bluetooth keyboard and mouse; web page at `http://macintosh3.local`;
  sound on the Tab5's speaker

## Hardware

An [M5Stack Tab5](https://docs.m5stack.com/en/core/Tab5): ESP32-P4 (RISC-V,
360 MHz), 16 MB flash, 32 MB PSRAM, a 5" 1280×720 touch screen (the Mac's
picture at 960×720), microSD, speaker; an ESP32-C6 for WiFi and Bluetooth.

The Makerfabs MaTouch ESP32-S3 7", the other board of the sibling
projects, isn't supported: its ESP32-S3 is far too slow for a 68030.

## Getting started

1. [ESP-IDF](https://github.com/espressif/esp-idf) v5.5 and
   [esp32emu](https://github.com/rlucencic/esp32emu) checked out next to this
   project (`../esp32emu`).
2. Your own ROMs (copyrighted, **not included**; see
   [roms/README.md](roms/README.md)): the LC II's as `roms/lc2.rom`, the LC
   III's as `roms/lc3.rom`.
3. Build and flash:

   ```sh
   idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=build-tab5/sdkconfig build
   idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=build-tab5/sdkconfig -p /dev/cu.usbmodem… flash
   ```

   Instead of flashing the ROMs, you can copy them into a `roms` folder on
   the SD card.
4. On the setup screen: WiFi, then drop a Mac OS 7.6.1 disk image on the
   web page and insert it into Drive 1, choose the model, memory and
   monitor, **Start**.

## How it works

- **`components/mac3`**: the LC II and LC III, platform independent (one
  file for each machine's map, the rest shared), with a desktop build
  (`components/mac3/host`) for testing: scripted keyboard and mouse,
  screenshots, sound recording, CPU, MMU and I/O traces.
- **`main`**: the Mac on the board: memory, ROMs, settings, the CPU speed,
  keyboard map, touch, drives and status.
- **[esp32emu](https://github.com/rlucencic/esp32emu)**: the board runtime and
  the CPU core, built as a 68030 here (`CONFIG_M68K_68030`).

The ROMs are patched in memory, every patch checked against the expected
bytes: no ROM checksum test, and the `.Sony` floppy driver replaced by
esp32mac's paravirtual one. Egret is emulated at the level of its packets,
with the handshake the ROMs themselves use. `CLAUDE.md` has the details.

## Limitations

- New. On the Tab5, both machines start Mac OS 7.6.1 and run it in real
  time in 256 colours, and the touch screen works. Sound, Bluetooth and the
  Tab5 Keyboard haven't been tried by hand yet.
- **Speed**: the Tab5 emulates about an LC II's worth of 68030. The LC II
  runs at ~90% of its real speed, the LC III at ~27% of its own (as fast
  as the LC II, not faster). Its bigger memory and video are there all the
  same.
- RAM sizes: the LC III takes 4, 8, 16 or 20 MB (the sizes its ROM finds
  with the RAM in one piece; 12 MB isn't one of them).
- No SCSI devices, serial ports, AppleTalk, printer or PDS cards; MMU
  faults don't raise exceptions, so no virtual memory.
- Disk images are presented through the floppy driver: copy-protected
  originals that talk to the floppy hardware directly don't work.

## Credits

- [Musashi](https://github.com/kstenerud/Musashi) by Karl Stenerud (MIT),
  via Matt Evans' [fork](https://github.com/evansm7/Musashi) for embedded use
- [MAME](https://www.mamedev.org)'s Mac LC II and LC III drivers by R.
  Belmont (V8, Sonora, pseudo-VIA, ASC; hardware tests by Doug Brown), its
  Sonora video by O. Galibert, and its 68030 MMU, the reference for how
  these machines' chips behave
- Linux's `via-cuda.c` and QEMU's Cuda, for the Egret packets
- [Mini vMac](https://www.gryphel.com/c/minivmac/) and
  [umac](https://github.com/evansm7/umac), via esp32mac
- [LVGL](https://lvgl.io), [ESP-IDF](https://github.com/espressif/esp-idf) and NimBLE

## Licence

[MIT](LICENSE). `main/web/macfiles.js` (the archive converter, from
esp32mac) is under the GNU LGPL 2.1 or later.

Apple, Macintosh and Mac are trademarks of Apple Inc. This project contains
no Apple code; you need your own ROM images and system software.
