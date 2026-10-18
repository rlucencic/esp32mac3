# Macintosh ROM images

Put your own ROM dumps here (they are copyrighted and not part of this
repository). The emulator recognises them by their checksum, so only these
files work:

| File | Machine | Size | Checksum |
|---|---|---|---|
| `lc2.rom` | Macintosh LC II | 512 KB | 35C28F5F |
| `lc3.rom` | Macintosh LC III (also the LC III+'s) | 1 MB | ECBBC41C |

`idf.py flash` writes them to the `rom_lc2` and `rom_lc3` flash partitions.

Without flashing, the emulator also finds them in a `roms` folder on the SD
card, under any file name.
