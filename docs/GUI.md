<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Keyboard launcher

`scarletnx-gui` is a small SDL3 launcher for the command-line drivers. It uses an
embedded public-domain 8x16 VGA BIOS font, nearest-neighbor integer scaling, and
English text only. No font files or localization packages are needed.

```sh
./build/scarletnx-gui
```

- Up/Down and Left/Right move the yellow selection, wrapping at the ends.
- Space or Enter activates the selected item.
- Selecting **Load Switch NSP/XCI/NRO** or **Load Linux ARM ELF** keeps that item
  visible and opens two selectable options underneath it: **Please drag and drop**
  and **Select file from games/ instead**.
- Activate **Please drag and drop** to wait for a file. SDL file drops and text drops
  are supported. Ctrl+V also accepts a clipboard path while waiting. ESC returns to
  the two options, and another ESC returns to the main menu.
- **Fullscreen** toggles the Switch game's startup setting, not the launcher's
  window. ON passes `--fullscreen`, OFF passes `--fullscreen=0`, overriding the
  game's INI setting. The CPU-only Linux driver has no fullscreen option.
- **Exit** closes the launcher. Closing its window during a run terminates the
  entire owned process tree, so let the game exit normally to preserve its saves.

The launcher looks beside its executable, then on PATH, for `scarletnx-mariko`
(the existing Switch driver alias) or `scarletnx-gnuabi`. Files are passed directly
as argv entries, never through a shell. Spaces, Unicode paths and shell punctuation
are not evaluated. Local `file://` text drops are decoded, but lists and remote URI
authorities are rejected. The working directory is not changed and is shown at the
bottom of the window. See [GNUABI](GNUABI.md) for supported Linux ELF applications.

## Selecting an executable from games/

**Select file from games/ instead** replaces the menu and old output with a file
list. It reads `games/` relative to the working directory shown in the footer,
not relative to the GUI executable. Each entry is displayed by basename, sorted
alphabetically. Only immediate executable files are listed (execute permission
on UNIX, `.exe` on Windows). Hidden files, shared libraries and directories are
excluded. The list is refreshed each time it is opened.

- **Back [ESC]** is the first selectable item on every page. Enter/Space on it, or
  ESC anywhere in the browser, returns to the two load options.
- Up/Down select Back or a file on the current page. Enter/Space opens a file.
- Left/Right and PgUp/PgDown go to the previous/next page. The page number and
  total file count are shown. Paging stops at the first and last pages.
- Resizing adjusts the page size while keeping the selected file visible.
- Empty or missing directories show a diagnostic and still provide Back.

For the Switch selection these are already-prepared native game launchers, so
selecting one runs it directly with the fullscreen setting rather than trying to
extract it as a dump. For the Linux selection, the selected file is passed to
`scarletnx-gnuabi` as an ARM64 ELF. Output capture, resource accounting and the
return to the main menu work the same way for both input methods.

## Output and resource use

While a driver runs, the window displays its merged stdout/stderr pipe. Descendants
inherit the same pipe, including extraction, recompilation and the final game.
The console consumes bytes as they arrive, even without a newline. Long lines wrap
at the right edge. At the bottom, rows move up and the oldest row is discarded.
Only the visible screen is retained, with no scrollback or accumulating log file.
ANSI color/title escapes are ignored. UTF-8 characters available in CP437 use the
corresponding VGA glyph, and other characters are displayed as `?`.

After the driver and its descendants finish, the initial menu returns automatically
with the exit status and the last visible output. Nothing is restarted automatically.
A missing driver or invalid dropped path leaves the prompt open with a diagnostic.
Drivers use line-buffered stdout so progress remains visible when piped. A third-party
program that buffers or privately redirects its output still controls when it flushes.

The footer samples combined GUI and descendant resource use every half second:

- RAM is the sum of resident sets (Windows working sets), in MiB. Shared pages may
  be counted more than once. It is not the machine's total installed memory.
- CPU is aggregate utilization. 100% means one fully occupied host CPU, so parallel
  compilation can exceed 100%.

Linux and FreeBSD track a private child session, including the separate process
groups created by the driver's stage supervisor and descendants that outlive their
immediate parent. Linux uses `/proc`, FreeBSD uses `sysctl`. Windows assigns the
suspended driver to a Job Object before letting it run, containing its descendants
and providing job-wide CPU accounting. No elevated privileges are needed.

## Building and testing

The NX build enables `SNX_BUILD_GUI` by default. Disable it with
`-DSNX_BUILD_GUI=OFF`. Tools-only builds do not enable the GUI.
The GUI itself needs only SDL3, not Vulkan or FFmpeg. Both driver binaries can be
built separately and installed beside it or made available on PATH.

To build it alongside the NX runtime (which supports `-H` for no-window runs):

```sh
cmake -S . -B build/gui -DSNX_DEPLOY=OFF -DSNX_BUILD_GUI=ON
cmake --build build/gui
ctest --test-dir build/gui -R '^gui_' --output-on-failure
```

For just the GUI and Linux runtime, use `-DSNX_BUILD_NX=OFF -DSNX_BUILD_GUI=ON`.
Tools-only and hactool-only builds cannot enable the GUI.
Supported host platforms are Linux, FreeBSD and Windows (llvm-mingw).
SDL3 handles window creation, keyboard input, rendering, clipboard and drag-and-drop
on every host. The platform-specific code only handles child processes and resource
accounting. See [BUILD](BUILD.md) for SDL3's desktop dependencies.

The tests cover menu transitions, bounded console scrolling, split UTF-8/ANSI
sequences, path parsing, argument quoting, inherited grandchild output, resource
accounting and process-tree cleanup. Browser tests cover filtering, missing/empty
directories, pagination, resizing, Back/ESC and launching selected files.
SDL tests use the dummy video driver and the
software renderer, exercising the real font and event handlers without a display.
The font's provenance and public-domain notice are in
[`src/gui/FONT-NOTICE.txt`](../src/gui/FONT-NOTICE.txt).
