<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Usage

Setup: [BUILD](BUILD.md). Checkout CWD; own dumps/keys.
For the keyboard-controlled SDL3 menu and file drops, see [GUI](GUI.md).

## Extract → AOT → run

Pass a single XCI or NSP to extract, recompile and play:

```sh
./build/scarletnx "Game.xci"
```

The driver runs each step as a child process and starts the game with `games/` as its working directory.
The name comes from the dump stem, retaining letters, digits, UTF-8, spaces, hyphens and underscores.
`--repo DIR` and `--build-dir DIR` may precede the dump. Keys use the usual discovery paths below.
`-H` (`--headless`) can precede or follow the dump and is passed to the game only.
Omitting it selects windowed startup, unless the build has no window backend.
`-x` means `--extract`, and `-r` means `--recomp` (the old `--recompile` spelling was removed).
On UNIX, short flags can be combined. Windows accepts separate short flags only.

To prepare a dump without playing, use `./build/scarletnx -r "Game.xci"`.
This extracts when necessary and forces recompilation, even if a cached executable exists.
`-rx` is equivalent on UNIX (`-r -x` on Windows). `-x` alone only extracts.
Pass arguments to the game executable after `--`, for example:

```sh
./build/scarletnx "Game.xci" -- some arg
```

The argument boundary is retained when launching the deployed game, so game arguments
cannot trigger launcher actions or overwrite runtime settings. This does not add a
homebrew-loader guest argv block.
`--fullscreen` (or `--fullscreen=0` for windowed startup) can also precede or follow
the dump and is forwarded to the game. It overrides that game's INI setting.
Use the separate commands for updates, custom keys or compiler options.

Run the same command again to retry after a failure or interruption:

- `work/Game/` skips extraction. Automatic extraction publishes this directory only after success,
  using `work/Game.extracting/` while running. An incomplete staging directory is discarded on retry.
- A complete `games/Game` (`Game.exe` on Windows), code module and matching `Game.abi` stamp
  skip recompilation. An ABI change or missing/invalid stamp triggers recompilation automatically.
  Older unstamped games rebuild once. Failed builds never publish a valid ABI checkpoint.
- Changing directory and running the game happen on every launch. A game failure never invalidates the build.

Failures stop the workflow and print the failed stage and exit status or signal. On POSIX, stopping a
child also aborts that stage so it can be retried, except for intentional debugger stops. Uncatchable termination of the driver itself, such as
`SIGKILL`, cannot print a diagnostic. Do not run concurrent workflows for the same output name.
Existing manually extracted directories are assumed complete. Remove stale checkpoints or use the
explicit commands when changing a dump or rebuilding game code.

### NRO applications

Pass an uncompressed NRO directly to recompile and play, without keys or extraction:

```sh
./build/scarletnx "app.nro" -H
```

The `.nro` extension is case-insensitive. NRO launches always recompile and restage the input,
so a same-named cached executable or extraction directory cannot silently select an older program.
Embedded RomFS assets are staged by the recompiler. Compressed NROs are not supported.
This uses ScarletNX's existing guest runtime, not a complete homebrew-loader environment.
Compatibility still depends on the application's runtime and service requirements.
See [NRO implementation notes](NRO.md) for the reference and validation policy.

To run the steps separately:

```sh
./build/scarletnx -x "Game.xci" --name Game
./build/scarletnx -r work/Game Game
games/Game --fullscreen=0
```

```powershell
.\build\scarletnx.exe -x "Game.xci" --name Game
.\build\scarletnx.exe -r work/Game Game
.\games\Game.exe --fullscreen=0
```

Remaining examples: POSIX.

```sh
# NSP + same-title update
./build/scarletnx -x Game.nsp --update update.nsp --name Game \
  --keys /path/prod.keys --title-keys /path/title.keys

./build/scarletnx -r work/Game Game --jobs 8
./build/scarletnx -r app.nro -o App --outdir games
./build/scarletnx -r -h
```

- **Keys:** flags → checkout → `~/.switch` (`%USERPROFILE%\.switch` on Windows). Common NSP tickets supplement title keys.
- **Extraction:** defaults to `work/<dump stem>`; `--name`/`--out DIR` override. Refuses existing `exefs`;
  builds missing hactool. [Temp cleanup/key filtering](../src/nx/runtime/driver/extract.c).
- **Recompile:** AOT → module compile/link → NVN prebuild → final runtime deployment. Cached objects;
  NVN prebuild failures defer compilation to runtime. `--native` = `-march=native`;
  `--cc CLANG` / `-O 0..3` also accepted.
- **Codegen:** `--no-build` emits C/data/INI; `--interp-only` omits AOT functions.
  `--chunk N` sets instructions per TU. Generated C, objects and linker response files use a private system
  temporary directory (`TMPDIR` on Unix), removed on success or failure. Objects are not cached between runs.
  `--keep-c` or `--no-build` saves C sources in `<outdir>/<NAME>_build/`.
- **Data updates:** ExeFS refreshes; `Game_data/romfs` is reused.
  Replace cached RomFS after asset changes; preserve `Game_data/save/`.

| Output | Contents |
| --- | --- |
| `work/Game/{exefs,romfs}` | Extracted input |
| `games/Game[.exe]`, `Game_code.so/.dll` | Runtime + AOT module |
| `games/Game_data/`, `Game.ini` | Data/saves + settings |
| `games/Game_build/` | C sources only with `--keep-c` or `--no-build` |
| `games/shader_cache/spirv/` | SPIR-V cache |

## GDB (UNIX only)

Install `gdbserver` on PATH. `-S` enables the native-code stub on loopback port **1234**
(the QEMU default port). The game runs until a debugger connects.
`-s` enables the stub and waits before runtime initialization. It implies `-S`.

```sh
./build/scarletnx -s "Game.xci"
# Or: games/Game -Hs
```

In GDB, load the deployed executable's symbols and connect:

```gdb
file games/Game
tar rem :1234
continue
```

The launcher prints a connection hint before waiting. A separate helper keeps accepting
connections while the game is stopped, then delegates the remote protocol to `gdbserver`.
The stub debugs host/native registers and code, not an emulated ARM64 CPU. It supports one
connection per run. Local ptrace permissions must allow attachment. Binding failures and
missing `gdbserver` are reported instead of silently running without a debugger.
No debugger flags or combined short flags are supported on Windows.

## Headless Vulkan / lavapipe

The normal SDL3 build can run without initializing SDL video or opening a window:

```sh
./build/scarletnx --game=games/Game -H --seconds=30 --shot=/tmp/frame
# Or extract, recompile and launch a dump without a window:
./build/scarletnx "Game.xci" -H
```

`-H` enables it. Omit the flag for the normal window. `--headless` is the long spelling
and accepts no value (`--headless=0` and `--headless=1` are errors).
This is a startup option, not a live toggle or an INI setting. A build with only the headless backend
always starts headless because it has no window backend.

Rendering still uses Vulkan 1.3, including uploads, draws, compute and screenshot readback. Headless runs
create no window, `VkSurfaceKHR` or swapchain and request no WSI extensions, including `VK_EXT_headless_surface`.
Frames still submit and recycle their resources. `--seconds`, `--frames`, `--script` and `--shot` work as before.
Keep `SNX_H264=ON` (the build default) for games that play H.264 video. With decoding disabled, those scenes can
present black frames even though Vulkan is drawing and frame counts advance.

To use software rendering, select Mesa's **lavapipe Vulkan ICD**, rather than the hardware driver. Mesa may
report this device as `llvmpipe` in the Vulkan log. Selecting a CPU ICD is independent of `-H`:

```sh
# Linux example. Substitute the actual lvp ICD manifest installed on your machine.
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/scarletnx --game=games/Game -H --seconds=30 --shot=/tmp/frame
```

Manifest names vary (for example `lvp_icd.json`). FreeBSD packages normally install manifests under
`/usr/local/share/vulkan/icd.d/`. Older Vulkan loaders use `VK_ICD_FILENAMES` instead of `VK_DRIVER_FILES`.
`LIBGL_ALWAYS_SOFTWARE` is an OpenGL setting and is not how this Vulkan renderer selects lavapipe.
No display server is required. Check the `vulkan:` device line in the log to confirm the chosen ICD.

For a build without an SDL3 dependency, see [headless build options](BUILD.md#build-variants).
This does not impose a staging-memory cap or fix the allocation/flush issues identified separately.

## AppImage / display

```sh
chmod +x ScarletNX-x86_64.AppImage
./ScarletNX-x86_64.AppImage --game=games/Game
SDL_VIDEO_DRIVER=x11 games/Game
```

`--game` selects `Game_code.so`/`Game_data/`; or name the image `games/Game.AppImage` to load adjacent files.
X11 bypasses Wayland. [Licenses](CREDITS.md).

## Settings / input

`Game.ini`: help rewritten at startup, most settings hot-reload. `--key=value` overrides one run.
Restart for `docked`, `language`, `fullscreen`, `resolution`, `memory_gb`, `heap_max_mb`, `profile`.

| Setting | Effect |
| --- | --- |
| `docked=1`, `fullscreen=0` | TV mode, windowed |
| `resolution=1` | Native scale; range 0.25–4 |
| `fps_limit=1` | 60 FPS; `0` uncapped; `60..240` explicit cap |
| `shadows=0` | Skip shadow maps |
| `language=1` | English (US); other IDs in INI |
| `log=2` | Verbose, adds runtime overhead |

`audout` PCM playback and the audio renderer's device sink (`audren`) use SDL3's system-default output on
Windows, Linux and supported Unix systems. Choose the output in your operating system's sound settings. The
renderer does not render splitters, voice biquad filters, or the delay, reverb and biquad effects yet.

SDL gamepads use Switch positions; pads + keyboard share one guest controller.

| Keyboard | Switch |
| --- | --- |
| Arrows; WASD / IJKL | D-pad; L/R sticks |
| X or Space / Z / C / V | A / B / X / Y |
| Q / E / 1 / 3 | L / R / ZL / ZR |
| Enter / Backspace | Plus / Minus |

F1 help; F3 screenshot; F4 FPS cap; F5 thread dump; F11/Alt+Enter fullscreen; Escape quit.
Beside the executable: `Game.log`, `threads.txt` (15 s refresh), `captures/` (screenshots).
Scripts/disasm/shaders: [DEVELOPMENT](DEVELOPMENT.md).
