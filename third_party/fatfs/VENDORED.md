# FatFs, vendored for Our64

FatFs R0.16 by ChaN, from http://elm-chan.org/fsw/ff/ (archive
https://elm-chan.org/fsw/ff/arc/ff16.zip, SHA-256
99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e), taken 2026-11-04.
License: FatFs's one-clause BSD-style license (`LICENSE.txt`, also at the top of `ff.c`), GPL-compatible.

Why R0.16 and not R0.15: R0.15 has the bugs published on 2026-07-10 as CVE-2026-6682 (broken FAT
size field), CVE-2026-6683 (exFAT cluster count divides by zero), CVE-2026-6684 (GPT table size
freezes the system) and CVE-2026-6687 (exFAT volume label overflows its buffer); the fixes are for
R0.16 only, and Our64 reads drives it did not format.

Patches from https://elm-chan.org/fsw/ff/patches.html, applied to `ff.c` in order:
- patch-1, https://elm-chan.org/fsw/ff/patch/ff16p1.diff (minimum volume size 64 sectors)
- patch-2, https://elm-chan.org/fsw/ff/patch/ff16p2.diff (CVE-2026-6682, -6683, -6687)

The archive's files use CRLF line endings; they were changed to LF before patching.

Taken: `ff.c`, `ff.h`, `ffconf.h`, `diskio.h` and `ffunicode.c` from `source/`, and `LICENSE.txt`
from the archive's root.
Left out: `diskio.c` and `ffsystem.c` (Our64 has its own disk functions; no locks or heap
buffers are configured), and the documents.

`ffconf.h`, changed from the defaults:
- `FF_FS_READONLY 1`: read-only on purpose: Our64 never writes to a USB drive
- `FF_USE_LABEL 1`: the volume label and serial (`f_getlabel`)
- `FF_CODE_PAGE 437`
- `FF_USE_LFN 2` (long file names, buffer on the stack: about 1.1 KB with exFAT)
- `FF_LFN_UNICODE 2` (UTF-8), `FF_STRF_ENCODE 3`
- `FF_VOLUMES 8`, `FF_MULTI_PARTITION 1`: two drives of four partitions each
- `FF_MAX_SS 4096` (`FF_MIN_SS` stays 512)
- `FF_LBA64 1` (GPT and drives over 2 TB), `FF_FS_EXFAT 1`
- `FF_FS_NORTC 1`

Unchanged and as Our64 wants them: `FF_FS_MINIMIZE 0`, `FF_USE_FIND 0`, `FF_MAX_LFN 255`,
`FF_STR_VOLUME_ID 0`, `FF_MIN_SS 512`, `FF_FS_REENTRANT 0`. No option Our64 sets was renamed or
changed meaning between R0.15 and R0.16; R0.16's new `FF_PATH_DEPTH` (only with relative paths)
and `FF_FS_CRTIME` (writes only) stay at their defaults. `tests/fatfs_rw/ffconf.h` is the same with
`FF_FS_READONLY 0`, `FF_USE_MKFS 1` and `FF_MIN_GPT 0x30000` (GPT from 96 MB): the PC tests use it
to make their disk images.
