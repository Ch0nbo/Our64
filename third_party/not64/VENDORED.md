# Not64, vendored for Our64

From https://github.com/extremscorner/not64, based on commit
d35a7a1c454eca973995939dbca3f06725cf4a6e (2026-04-10). Licence: GPL-2.0 (`LICENCE.TXT`).
Our64 copy taken at commit 076e9f509535db8aa1d913d7601fae66a6a762b2.

Taken: the CPU core and its PowerPC recompiler (`r4300`), memory, DMA and PIF (`gc_memory`), the
HLE RSP (`rsp_hle`), and the few other files those include (`config.h`, `main/`, `fileBrowser/`,
`gui/DEBUG.h`). Left out: the
GameCube and Wii front end, menus, graphics and audio plugins.

Changes made for Our64, built only with `__WIIU__` where they touch the Wii and GameCube paths:
- Wii U support: devkitPPC/wut build, save states, Controller, Rumble and Transfer Pak hooks, and
  graphics and audio tasks handed to Our64.
- Recompiler: jr/jalr target table, loads and stores at TLB addresses looked up in generated code,
  code at TLB addresses kept correct after DMA and remaps, DIV/DIVU with r0, idle-loop skips.
- Interrupts and timing: VI events no longer duplicated, Count writes keep the queue in step, the
  RDP clock counts.
- HLE: the mupen64plus JPEG decoder (Pokemon Stadium 1 and 2, Ocarina of Time), a second audio state,
  Resident Evil 2 on a big-endian host, 4 MB at boot for games that need it.

These are changes for Our64, not fixes claimed for Not64 itself.
