<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# GUI launcher

`scarletnx` started with no arguments (or a model's name, or the AppImage), with no game of its
name beside it, opens its launcher: a small SDL3 menu for the command-line drivers. A copy named
after a game, without its code, says so and never opens it. It uses an
embedded public-domain 8x16 VGA BIOS font, nearest-neighbor integer scaling, and
English text only. No font files or localization packages are needed.

```sh
./build/scarletnx
```

The window's title tells which build it is, as `ScarletNX <branch> <commit> (<compiler>, <OS> <arch>)`, for example
`ScarletNX main 43bf71a (clang 22.1.8, Linux x86_64)`; the commit ends in `-dirty` when the tree had changes, a
build of a detached checkout outside CI says `detached`, and both read `unknown` outside a git checkout. A game's window carries the same line after the game's name.

- Up/Down and Left/Right move the yellow focus, wrapping at the ends.
- A list with rows out of sight (the menu with big letters, `games/`'s pages, the Settings, the theme
  editor) shows an arrow in its left margin, up on its first row and down on its last, in the soft colour.
- Moving the mouse over an item focuses it in yellow without activating it.
- The menu, top to bottom: **Recent games**, **Games**, the three loads, **Setup device keys**,
  **Settings**, **Share log** and **Exit**.
- Space, Enter or a left click explicitly activates an item, marking it bright
  pink. Activation is tracked separately from hover/focus, so the activated item
  stays pink while another item is hovered in yellow. Mouse controls also work
  on browser entries and Back.
- A finger taps an item anywhere on its row, as a click does on its label; a swipe across
  turns the pages of `games/`; a tap on a finished run returns to the menu, and one on a
  running run's ESC line aborts it. Android's back button is ESC. On a screen packed with
  pixels (a phone) the text takes the display's own scale, as far as the menu still fits.
- **Games (N games)** shows only when `games/` or a folder of Settings > App > Game folders
  holds a game, and opens their browser: the games ready to run (a recompiled game runs
  directly), the dumps to prepare on Android, and CIA files, which go to the 3DS driver.
- **Load Switch NSP/XCI/NRO**, **Load Linux ARM ELF** and **Load 3DS CIA** open a screen as the
  keys': a click on its blinking **[ Pick or drop a file ]**, or Enter, opens the system's file
  picker (on Linux the desktop's, else zenity's). Dropping a file on the window and Ctrl+V with
  a path work too. ESC returns to the menu. On Android the picker opens at once.
- **Load 3DS CIA** uses `scarletnx-3ds`. The driver extracts supported decrypted application CIAs,
  then returns an explicit error because ARM11 AOT/execution is still pending.
  Existing extraction directories are refused rather than treated as cache hits.
  See [3DS port](3DS.md) for limits and extract-only usage.
- **Setup device keys**, immediately after the three load entries, prompts you to
  pick `prod.keys` (a click on the blinking **[ Pick or drop prod.keys ]** in the middle of
  the screen, the theme's `[prompt]`, or Enter), drop it, or paste its path with Ctrl+V. It installs the file to `~/.switch/prod.keys`, or
  `%USERPROFILE%\.switch\prod.keys` on Windows; an installed copy on Linux puts it in its
  data folder instead (`~/.local/share/scarletnx/prod.keys`), which the driver reads before
  `~/.switch`, and names the keys of `~/.switch` it would use until then. It creates the
  folder if needed. Existing keys trigger the bright-red warning
  **Keys already setup. This will overwrite the previous ones**. A successful
  drop replaces them and returns to the menu; ESC cancels without changing them.
- **Settings** opens on its groups, Enter opening one and ESC coming back: **App**, then
  **Launch** (the launcher's two), then a group per section of the game's `.ini` (Display,
  Graphics, Performance, On-screen controls, Controllers, Network, Debug), as `scarletnx --settings`
  prints them from the runtime's own tables. The game's menu's settings have its names and words
  (*Scale* 100%, *Touch screen* Fingers), and its **Buttons** page, whose **All** steps every row;
  *Edit layout* and *Reset layout* stay in the game's menu, as the layout is each game's own.
  Controllers' **Button map** draws the pad as the game's menu does: the presets on the left (*Each
  game's*, the `.ini`'s, then *By position*, *By label*, *Custom*), each control in a box with the
  console button it sends, and what the focus does at the bottom. Enter (or the pad's A) uses a
  preset and Right goes to the controls, where Left/Right change a control's button and Enter waits
  for a press on the pad, which takes it. A change makes *Custom* the preset in use, kept as
  `gamepad_buttons` and `gamepad_map`; Del puts both back to the `.ini`'s. The first pad plugged in
  is shown, its held buttons lit; the launcher reads pads only on this page.
  **App** holds **Theme** and **Text size**, which Left/Right or Enter go round either way, **Theme editor**, which comes back to App, and **Game folders**: `games/`,
  always listed, then the player's own folders, whose games the `games/` browser lists after
  `games/`'s. **Add a folder** opens the system's folder picker; a folder dropped on the window
  or a path pasted with Ctrl+V is added too. Del, or Enter twice, takes one off the list (the
  folder stays). They are kept in `folders.txt` beside `settings.ini`; one that cannot be read
  is named when the games are listed. A group of settings shows
  a row each, its help above, Left/Right going round its values, Enter typing one. A setting not picked shows the runtime's default and `[default]` (*By label [default]*):
  the run takes the game's `.ini`'s, which starts as that; one picked goes to every
  Switch run as `--key=value`, over the `.ini` ([USAGE](USAGE.md)), and is kept for every game:
  what was changed, by the `.ini`'s sections and keys, in `settings.ini` of the config folder on
  Linux (`~/.config/scarletnx/`), else beside `recent.txt`. A run takes a setting from what that
  run picks, then from the game's own picks (to come), then from these, and last from the game's
  `.ini`. Del puts a setting back to its default, and the groups' last row, **Reset all to the
  defaults**, every one. The launcher's own choices start picked: `fullscreen` 0, `controls` 0 (on Android 1,
  and `fullscreen` puts the launcher itself on the whole screen too).
  **libgccjit** passes `--backend=gcc` when preparing a dump (it stays off in a build without
  libgccjit; games in `games/` are not recompiled), **gdbstub** passes `-S` and `-s`, the
  runtime's stub on port 1234 waiting for the debugger at startup (UNIX only). None of them
  reaches the Linux or the 3DS driver.
- The footer's first line, after the RAM and the CPU, says what ScarletNX's folders take (games/,
  work/ and themes/ of the data folder, and on Linux its config, state and cache folders, else
  the menu's own) and what is left on that disk, counted as the menu opens and after each run.
- Settings > App's **Theme** switches to the next theme, or the one before: the built-in Default
  and YoRHa, then each folder of `themes/` (see [Themes](#themes)). It lasts for the session.
- Settings > App's **Theme editor** edits the current theme: a row per `theme.ini` key, and beside them a
  preview of the menu or a run (Tab switches). Left/Right change a word, a switch or a
  number, Enter types a colour, a text or a number.
  Save (Ctrl+S) writes `themes/<folder>/theme.ini` in the data folder and switches to it;
  a built-in is saved as a new theme, `mytheme` until renamed. ESC leaves, asking first if
  there are unsaved changes.
- Settings > App's **Text size** steps the menu's letters through Auto (fit the screen) and four
  sizes. Bigger letters scroll the menu.
- **Share log** saves the last run's output and game log to `scarletnx.log`. On the
  desktop it goes in the working folder with its path copied. On Android it goes to
  `/sdcard/ScarletNX/` and is shared.
- **Exit** closes the launcher. Closing its window during a run terminates the
  entire owned process tree, so let the game exit normally to preserve its saves.
- On Linux, as it opens, the menu shows in a box what an older version left where this one no
  longer looks: each file where it is (`- `, in the failure colour) and where to move it (`-> `),
  a path too long for a row going on in the next. They are the menu's
  `recent.txt` and `last-run.log`, `work/`, and each game's settings, log, `threads.txt` and
  pipeline cache. Any key, click or tap closes it;
  nothing is moved, and it shows again at the next start for as long as they are there.
  **Share log** lists them all.
- **Recent games**, above the items, lists the last five games run, newest first, by
  the name their run gave (the runner's `ScarletNX - <name>` line), else their file's.
  The focus moves between them and the items in one ring, and the line under the menu
  shows the focused game's path. Choosing one runs it again with the driver it was
  loaded with and the options the menu has now; a game whose file is gone leaves the
  list instead. The list is kept in `recent.txt`, beside `last-run.log`, in the user's state
  folder on Linux (`$XDG_STATE_HOME/scarletnx/gui/`, by default `~/.local/state/scarletnx/gui/`;
  an older version's in `~/.local/share/scarletnx/gui/` stay where they are), or in
  `%APPDATA%\scarletnx\gui\` on Windows, a line per
  game: `switch`, `linux` or `3ds`, `file` or `games` (a program from games/, run as it is),
  the path and the name, separated by tabs.

The launcher runs Switch inputs with the `scarletnx` it is part of, and looks beside it, then on
PATH, for `scarletnx-gnuabi` or `scarletnx-3ds`. Files are passed directly
as argv entries, never through a shell. Spaces, Unicode paths and shell punctuation
are not evaluated. Local `file://` text drops are decoded, but lists and remote URI
authorities are rejected. The working directory is not changed and is shown at the
bottom of the window. See [GNUABI](GNUABI.md) for supported Linux ELF applications.

## Games: the games of games/ and the player's folders

**Games** replaces the menu and old output with a file list. It reads the `games/` of the
folder the driver works in, which the menu passes it as `--repo`: the checkout of a build tree,
else the folder of an AppImage (or of the GUI) that has a `games/` beside it, else the user's
data folder (docs/USAGE.md), then each folder of Settings > App > Game folders. Each entry is
displayed by basename, sorted alphabetically in each folder. Only immediate files are listed:
executables (execute permission on UNIX, `.exe` on Windows) and `.cia` files (case-insensitive,
no execute permission needed), which run with the 3DS driver. Hidden files, shared libraries and
directories are excluded. The folders are looked at once a second while the menu is open (their
names only), so a game put in or taken out shows in Games and in the open list; a folder that
cannot be read is named and the others still listed.

- **Back [ESC]** is the first selectable item on every page. Enter/Space on it, or
  ESC anywhere in the browser, returns to the menu, on Games.
- Up/Down select Back or a file on the current page. Enter/Space opens a file.
- Left/Right and PgUp/PgDown go to the previous/next page. The page number and
  total file count are shown. Paging stops at the first and last pages.
- Resizing adjusts the page size while keeping the selected file visible.
- Empty or missing directories show a diagnostic and still provide Back.

A recompiled game is an already-prepared native launcher, so selecting one runs it directly with
the Settings rather than trying to extract it as a dump. A CIA goes to `scarletnx-3ds`, which is
never executed on it directly. Output capture, resource accounting and the return to the main menu
work as for a file picked by a load.

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

- RAM is the sum of resident sets (Windows working sets), in MB or GB. Shared pages may
  be counted more than once. It is not the machine's total installed memory.
- CPU is a share of the whole machine, 100% being every core busy, then how many cores'
  worth of time that is.

Linux and FreeBSD track a private child session, including the separate process
groups created by the driver's stage supervisor and descendants that outlive their
immediate parent. Linux uses `/proc`, FreeBSD uses `sysctl`. Windows assigns the
suspended driver to a Job Object before letting it run, containing its descendants
and providing job-wide CPU accounting. No elevated privileges are needed.

## Themes

A theme colours and dresses the screen's three parts: the head (a title and the hints
at the top), the body (the menu, the file list or the driver's output) and the footer
(resource use and the working directory). It applies to the menu screens and to the
run, which is **compiling** until the runner prints its `ScarletNX - <name>` banner and
**loading** from then on.

The built-ins are `default`, the look above, and `yorha`: paper and ink with a header
bar, square markers and the focused line inverted on the menus, and a run drawn as a
dark loading screen in its own colours (`[run]`): the title, the game's name with turning
dots and a turning ring; each step the driver and the runner report (unpack, extract,
recompile with its blocks, built, launch, GPU) typed down the page, with turning dots
while it runs and `...OK` or `...FAILED` once it ends, the cursor blinking on the current
one, and the other lines it printed indented under it (`[run] bar` adds a progress bar
with the running step's percentage). A step that counts, the unpack and the extraction by
what hactool has written, the recompile by its blocks, shows how far it is on its line as
`[run] progress` says: `amount` (`1.2 / 4.0 GB`, `3 / 6`), `percent` (`30%`, YoRHa's) or
`time`, what is left at its pace so far (`01:23 left`). With `layout = console` the
driver's own row says it, `unpacked 1229 of 4096 MB (30%)`, written over itself each second. The menu fades out to black as a run starts and the run fades in from it; ESC
fades the run out and the menu back in.

A theme is also a folder holding a `theme.ini`, the folder's name its id, in
`themes/` in that same folder or in `themes/` beside the GUI; the first one found
wins, and a folder named like a built-in replaces it. The built-ins are such files, and
nothing else: each folder of `src/gui/themes/` is one, its `theme.ini` compiled into the GUI
(`cmake/gui_themes.cmake`) and read as any other, setting every key. The runtime's game menu
takes YoRHa's colours from the same file at build time. The files are copied beside the GUI
in the build and installed in `share/scarletnx/themes`: copy one under a new name to make a theme, or edit one
to change the built-in. `[theme] base` picks the built-in a file starts from, and a key it
leaves out, or that cannot be read, keeps the base's value. The menu's **Theme editor**
writes one with every key.

```ini
[theme]
; the theme file's format, 1; a newer file's keys this GUI knows still apply
format = 1
name = Night ink
author = me
; default | yorha
base = yorha
[colors]
; #RRGGBB. soft: hints, secondary text, rules. focus: the focused line, or its band.
; dim: the ring's outer circle, the progress bar's empty segments and its rule
page = #101418
ink = #D8D4C4
soft = #8A8676
focus = #D8D4C4
active = #C8A040
failure = #D05040
dim = #3E3E40
; a diff's added lines, as failure its removed ones; no screen uses it yet
added = #6A9A50
[head]
; plain | bar | none; rule: a line under the head
style = bar
rule = true
; the title per screen: menu, games, keys, compiling, loading
menu = NIGHT  /  MENU
[body]
; before each menu item; quotes keep spaces
marker = "> "
; color | bar
focus = bar
[footer]
style = plain
rule = true
[run]
; console: the driver's output as it comes; steps: the loading screen above, in its own colours
layout = steps
page = #141416
ink = #E6E4DC
soft = #8E8C86
dim = #3E3E40
failure = #D2463A
warning = #D9B24A
; faint lines over the page, the progress bar and percentage, and the ring: none | left | right
texture = true
bar = true
ring = right
; a running step's progress: amount | percent | time
progress = percent
; detailed: each step's other lines under it, down the screen; collapse: a done step's
; folded away, a failed one's kept
detailed = true
collapse = false
[prompt]
; a line that waits on the user, when its key is true: framed, left | center | right on its
; row, blinking at blink_ms. keys: the keys screen's drop line; done, failed: the ESC line
; after a run; aborting: a run being aborted
align = center
open = "[ "
close = " ]"
blink = true
keys = true
failed = true
[transitions]
; milliseconds, 0 to 10000; 0 turns the thing off. fade_out_ms: the menu as a run starts,
; and a run ESC leaves; fade_in_ms: what comes after
fade_in_ms = 250
fade_out_ms = 500
; how a step's line comes in: none | typing | slide | fade
line_in = typing
line_in_ms = 220
typing_ms = 5
; async: every line types from when it comes; sync: one at a time. A running step's newest line
; types a character each typing_ms, any other line whole in typing_ms
typing = async
blink_ms = 500
; a line too long for its row (big letters) slides left a column every scroll_ms and back; 0 cuts it
scroll_ms = 120
[splash]
; the boot's: the program's icon on page for ms milliseconds (0: none). chance: the boots in 100 that
; show this theme's, the default theme's the others
page = #DAD6BE
ms = 1000
chance = 25
```

Comments take a line of their own, starting with `;` or `#`.

The format is rEden's ErET format 1, extended, so one file can serve both: each program
reads the keys it knows and leaves the others. From ErET's, the GUI also reads:

| ErET key | here |
|---|---|
| `[theme] layout` | the base when no `base` names one: `menu` and `loading` are `yorha`, `plain`, `debug` and `skip` are `default` |
| `[colors]` | with `layout = loading` and no `base`, the run's colours, as `[run]`: ErET's loading screen is YoRHa's runs |
| `[colors] texture`, `warning` | as `[run] texture`, and the colour of a run being aborted |
| `[header] ring_side` | `right`, `left` or `right-near-text` (right after the name and its dots), as `[run] ring` |
| `[transitions] typing` | `async` or `sync`, as above |
| `[loading] title` | the loading screen's title |

ErET needs `[theme] layout`, so the built-in files carry one: `debug` and `menu`.

### Theme formats

`[theme] format = N` names the theme format a file was written for; this GUI's is 1
(`GUI_THEME_FORMAT`).

- A file of a format the GUI knows reads the same in it and in every later GUI: each key
  keeps its meaning, and each value reads to the same theme. `tests/gui_themes/<N>/` holds
  files of format N as a theme tool wrote them, with the theme each reads to (`<name>.json`,
  the GUI's fields by key); `gui_theme_formats` checks them, and `gui_theme_write` checks
  that each, written again, reads to the same theme.
  They are never changed: a field added to themes later is simply not in an older one's JSON.
- A new key, or a new word for a key, keeps N: an older GUI leaves what it does not know, and
  the base's value stays. A new key's value in the built-ins must keep an older file looking
  as it did.
- N goes up only when a key that exists has to mean something else. The GUI then reads the
  older format's files with the older meaning, their fixtures stay, and the new format's go
  in `tests/gui_themes/<N+1>/`.
- A file of a newer format than the GUI's: the keys it knows still apply, the others are left.
- The theme editor writes every key and the newest format it knows (`gui_theme_write`), so its
  files never lean on a built-in's values, which may change. It reads an older file as that
  format reads, and writes it in the current one.

## Building and testing

The NX build enables `SNX_BUILD_GUI` by default. Disable it with
`-DSNX_BUILD_GUI=OFF`. Tools-only builds do not enable the GUI.
The GUI itself needs only SDL3, not Vulkan or FFmpeg. The driver binaries can be
built separately and installed beside it or made available on PATH.

To build it alongside the NX runtime (which supports `-H` for no-window runs):

```sh
cmake -S . -B build/gui -DSNX_DEPLOY=OFF -DSNX_BUILD_GUI=ON
cmake --build build/gui
ctest --test-dir build/gui -R '^gui_' --output-on-failure
```

For just the GUI and Linux runtime, use `-DSNX_BUILD_NX=OFF -DSNX_BUILD_GUI=ON`: the launcher is
then `scarletnx-gui`, a program of its own, which looks for `scarletnx` as it does for the other drivers.
Tools-only and hactool-only builds cannot enable the GUI.
Supported host platforms are Linux, FreeBSD, Windows (llvm-mingw) and Android (`dist/android`).
SDL3 handles window creation, keyboard/mouse input, rendering, clipboard and drag-and-drop
on every host. The platform-specific code only handles child processes and resource
accounting. See [BUILD](BUILD.md) for SDL3's desktop dependencies.

The tests cover menu transitions, bounded console scrolling, split UTF-8/ANSI
sequences, path parsing, argument quoting, inherited grandchild output, resource
accounting and process-tree cleanup. Browser tests cover filtering, missing/empty
directories, pagination, resizing, Back/ESC and launching selected files.
Key setup tests cover installation, replacement, invalid drops, failed writes,
self-copy safety, cancellation, and the bright-red overwrite warning.
Settings tests cover the runtime's list read into rows, picks, the options each combination
passes, and the screens of its groups and App; menu tests cover mouse hover versus explicit
activation, Games shown only with games, the recent games' show-more row,
yellow/pink highlighting and footer descriptions. Theme tests cover the built-ins, a `theme.ini` over its base
(unreadable values and unknown keys included), the `themes/` folders listed, the
compiling and loading screens, and the YoRHa bars and band in the real renderer. Step
tests cover each driver and runner line, the other lines kept under their step, lines
split across reads, the progress and how a run ends; the renderer tests check the menu
fading out into a run, the loading screen, its typing, detailed and collapsed steps,
texture and fades at a fixed clock.
SDL tests use the dummy video driver and the
software renderer, exercising the real font and event handlers without a display.
The font's provenance and public-domain notice are in
[`src/gui/FONT-NOTICE.txt`](../src/gui/FONT-NOTICE.txt).
