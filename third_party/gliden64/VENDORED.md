# GLideN64, vendored for Our64

From https://github.com/gonetz/GLideN64, based on commit
a04081ad6a966a9413bea63a55f502e0dc98bb18 (2026-10-11). Licence: GPL-2.0 (`LICENSE`,
third-party licences in `licenses/`).
Our64 copy taken at commit 006f2a9132b7a037e1d395b9a22064d9accdff70.

Taken: `src/`, the per-game settings `ini/GLideN64.custom.ini`, and the licences. Left out: the
project files, translations and CI.

Changes made for Our64:
- A big-endian port and a GX2 backend for the Wii U GPU (`src/Graphics/GX2Context`, built with
  `OUR64_GX2_GFX`): shaders compiled on a worker thread and cached on the SD card, CPU and GPU
  overlapped across frames, color and depth copies written by the GPU straight into N64 memory.
- Microcode and per-game fixes found on the console, each noted in its commit: ZSortBOSS, Indiana
  Jones, Resident Evil 2, Donkey Kong 64, Turok, Blast Corps, Pokemon Snap, Army Men, and others,
  with their settings in `ini/GLideN64.custom.ini`.
- A null backend (`Graphics/NullContext`) for the PC and console replay tools.

These are changes for Our64, not fixes claimed for GLideN64 itself.
