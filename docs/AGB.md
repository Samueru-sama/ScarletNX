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
| HLE BIOS interface | not yet |
| PPU and display | not yet |
| APU (audio) | not yet |
| Keypad (HID) | not yet |
| CPU, memory map, DMA, timers, interrupts | not yet |

This build recognizes a ROM and reports its header. Nothing is emulated yet.

## The ROM header

Every GBA ROM image starts with a header the cartridge presents to the console:
an ARM branch at offset 0, the Nintendo logo, the title at 0xA0, the game code at
0xAC, the maker code at 0xB0, the fixed byte 0x96 at 0xB2 and the complement
check at 0xBD over the title block. `cartridge/cartridge.c` validates the branch,
the fixed byte and the complement check, and keeps the parsed fields beside the
image. A ROM the console would reject is refused with the failing byte named.
