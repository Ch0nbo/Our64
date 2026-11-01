# libgui, vendored for Our64

From https://github.com/dborth/libgui at commit 03cb4283f30f1ff2557b4f34ee16aa076f5c0a8e
(2026-10-15). Licence: GPL (README; no version stated).

Taken: `source/libgui`, the platform-neutral driver code in `source/drivers`
(interfaces, Cond, Mutex, Thread, Logger, Trace, InputController) and from
`source/drivers/wut` the input, thread and video drivers with their shaders.

Left out: GuiOptionBrowser, GuiFileBrowser and GuiKeyboard (they need the demo's art; the keyboard is the system one), the demo app, the GameCube and Wii drivers, the Wii U audio,
file system, SMB, USB probe and logger drivers (no libsmb2, libmocha or
libdvm in Our64), `data/` (images, sounds, language blob) and its bundled
`font.ttf`: Our64 uses the console's system font. `FileSystemDriver.h` and
`SmbDriver.h` stay only because `Platform.h` includes them.

Our64's own changes are marked `OUR64` in the code.

OurSNES (2026-10-22): `WutImageRenderer` gets a second sampler with point
filtering (`setNearest`) and `drawTextureRect`, a texture draw with a float
position and size. The game's picture is scaled with sharp pixels onto whole
TV pixels, which libgui's filtered sampler and whole-canvas-pixel sizes
(854x480 canvas on a 1280x720 or 1920x1080 buffer) cannot do. Both are
marked `OUR64`, as Our64's changes are.
