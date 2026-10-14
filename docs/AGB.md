# The AGB board

`scarletnx-agb` is the Game Boy Advance system type. It loads a GBA ROM image
(`.gba`) onto an emulated AGB board, the codename of the console's hardware, and
runs it with the same static recompiler the other system types use. The board
library under `src/agb/` is CPU-only: it links `scarletnx-common` and nothing of
`src/nx/` or `src/gnuabi/`, so the board builds without the Switch runtime's
graphics and media packages.

```sh
cmake -S . -B build/agb -DSNX_BUILD_NX=OFF -DSNX_BUILD_AGB=ON
cmake --build build/agb
build/agb/scarletnx-agb Game.gba
```

`scarletnx-agb` is also a menu entry of `scarletnx-gui` ("Load GBA ROM"), next
to the Switch and Linux loaders.

## Status

The board is under construction, piece by piece:

| Part | State |
| --- | --- |
| Cartridge and ROM header | done: header parsing and validation |
| Memory map | done: every region and mirror, the I/O window as plain storage |
| HLE BIOS interface | mostly done: the software interrupts run as board routines, the waits and sound driver stubs |
| PPU and display | done: the text, affine and bitmap layers, sprites, windows and blending, drawn a
| scanline at a time |
| APU (audio) | not yet |
| Keypad (HID) | not yet |
| CPU, DMA, timers, interrupts | not yet |

This build recognizes a ROM, reports its header and runs the BIOS as HLE. No
code runs on the board yet.

## The display device

`ppu/ppu.c` owns the LCD controller's registers at 0x04000000 and its timing,
308 dots of four cycles a scanline and 228 scanlines a frame, with the blanking
flags in DISPSTAT and the vblank, hblank and vcount interrupts. Each visible
line is drawn into the 240 by 160 RGBA frame the host presents: the text,
affine and bitmap background layers, the sprites with their affine forms, the
windows, and the colour special effects (alpha blending and brightness). Mosaic
is the one display feature still missing.

## The memory map

`board/memory.c` owns the address space the CPU will run on: work RAM (256 KiB
at 0x02000000 and 32 KiB at 0x03000000), the palette, VRAM and OAM, the save
memory at 0x0E000000 and the cartridge window at 0x08000000, each with the
mirrors the console decodes. The I/O window at 0x04000000 is plain register
storage so far. The devices replace their registers with real behaviour as they
land.

## The HLE BIOS

The console's BIOS is not shipped with the emulator. The software interrupts run
as board routines (`bios/bios.c`), the way the Switch runtime turns SVC calls
into HLE and the GNU ABI runtime turns `SVC #0` into Linux system calls. Div,
Sqrt, ArcTan and ArcTan2 compute in fixed point, CpuSet and CpuFastSet move
memory, the LZ77, run length and difference calls decompress, BitUnPack widens
packed fields, and BgAffineSet and ObjAffineSet build the rotation matrices. The
calls that need the interrupt controller or the sound hardware (Halt, Stop,
IntrWait, VBlankIntrWait, SoundBias and the sound driver) are known stubs that
land with those devices.

Angles are turns in 16 bits, the way the BIOS counts them, and the affine matrix
drops the low byte of the rotation angle, giving 256 steps around the circle.
The trigonometry rounds to 1.14 fixed point like the BIOS's own tables.

## The ROM header

Every GBA ROM image starts with a header the cartridge presents to the console:
an ARM branch at offset 0, the Nintendo logo, the title at 0xA0, the game code at
0xAC, the maker code at 0xB0, the fixed byte 0x96 at 0xB2 and the complement
check at 0xBD over the title block. `cartridge/cartridge.c` validates the branch,
the fixed byte and the complement check, and keeps the parsed fields beside the
image. A ROM the console would reject is refused with the failing byte named.
