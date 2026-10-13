<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Build

Run from checkout root. [Extract/AOT/run](USAGE.md) · [Tests/diagnostics](DEVELOPMENT.md).

## Prerequisites

- **Linux / FreeBSD:** Clang (CI: 23; FreeBSD's system Clang also works), CMake ≥3.25, Ninja,
  POSIX shell, GNU make (`gmake` on FreeBSD), NASM on x86-64, Git and pkg-config.
  Use a system SDL3 development package, or X11/Wayland development libraries for the bundled SDL3 build.
- **Windows:** Win10/11 x64 + Git. Bootstrap stages SHA-256-checked llvm-mingw/Clang, CMake, Ninja,
  NASM and w64devkit in `deps/tools`.
- **Display:** a Vulkan 1.3 driver. Headless builds omit SDL3 and use Vulkan offscreen.
- **FFmpeg:** avoid spaces in checkout/cache paths.

## Linux / FreeBSD

Install dependencies before configuring. The default preset uses **Ninja** and **Clang/Clang++**.
FFmpeg also uses **GNU make** and, on x86-64, **NASM**. These tools are not downloaded by CMake.

### Debian / Ubuntu

```sh
sudo apt update
sudo apt install build-essential git cmake ninja-build clang nasm pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev
```

This includes the X11 development libraries for the SDL3 source fallback. If your release provides
`libsdl3-dev`, install it to use the distribution's SDL3 instead:

```sh
sudo apt install libsdl3-dev
```

If apt cannot find that package, skip it. ScarletNX builds the pinned SDL3 automatically.
Installing `libsdl2-dev` does not satisfy the SDL3 dependency.

For native Wayland support in the source fallback, also install:

```sh
sudo apt install libwayland-dev libxkbcommon-dev wayland-protocols libdecor-0-dev
```

CMake needs version 3.25 or newer, and Clang must support C23/C++23. Upgrade these tools if your
release ships older versions. Install your GPU's Vulkan driver separately. For Intel/AMD or lavapipe
on Debian/Ubuntu, the usual packages are `libvulkan1 mesa-vulkan-drivers`. NVIDIA users need a
Vulkan-capable NVIDIA driver.

### Arch Linux

```sh
sudo pacman -Syu --needed base-devel git cmake ninja clang nasm pkgconf sdl3
```

### FreeBSD

FreeBSD provides Clang in the base system. Install the remaining tools and the SDL3 package:

```sh
sudo pkg install git cmake ninja gmake nasm pkgconf sdl3
```

### Configure and build

```sh
git clone https://github.com/scarletnx/ScarletNX.git && cd ScarletNX &&
cmake --preset default -B build && cmake --build build
# -> build/{scarletnx,hactool,snx-shaderc}
```

### SDL3 troubleshooting

On Linux and FreeBSD, CMake first looks for `SDL3Config.cmake`. If it is missing, CMake downloads and
builds the same SHA-256-pinned SDL3 used on Windows. The fallback is static, so no separate SDL3 runtime
installation is needed. It still needs your platform's window-system development libraries.

- An old checkout showing `Could not find a package configuration file provided by SDL3` needs either
  the SDL3 **development** package or this updated fallback. Pull the latest checkout and configure again.
- For SDL3 installed in a custom prefix, pass `-DCMAKE_PREFIX_PATH=/path/to/prefix`, or
  `-DSDL3_DIR=/path/to/prefix/lib/cmake/SDL3` (the directory containing `SDL3Config.cmake`).
- To force the source build, pass `-DSNX_BUNDLED_SDL3=ON`.
- If SDL3's configure reports missing X11/Wayland development libraries, install the packages above.
  Do not work around that by disabling both display backends for a windowed build.
- A no-window Vulkan build needs no SDL3. Use `-DSNX_BACKEND=headless` as shown below.
  The runtime flag `-H` alone does not remove the SDL3 build dependency from a window-capable binary.

After installing dependencies, rerun:

```sh
cmake --preset default -B build && cmake --build build
```

If that directory was configured with a different compiler or generator, use a new build directory
(for example `-B build/clang`) rather than mixing the old cache with the new toolchain.

## Windows / PowerShell

```powershell
& {
    $ErrorActionPreference = 'Stop'
    git clone https://github.com/scarletnx/ScarletNX.git
    if ($LASTEXITCODE) { throw 'Clone failed' }
    Set-Location ScarletNX
    powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
    if ($LASTEXITCODE) { throw 'Bootstrap failed' }
    Set-ExecutionPolicy -Scope Process Bypass
    . .\scripts\env.ps1
    cmake --preset default -B build
    if ($LASTEXITCODE) { throw 'Configure failed' }
    cmake --build build
    if ($LASTEXITCODE) { throw 'Build failed' }
}
# -> build\{scarletnx.exe,hactool.exe,snx-shaderc.exe}
```

Each new shell, from the checkout:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
. .\scripts\env.ps1
```

`env.ps1 -Isolated` restricts PATH to bundled tools, Git and Windows.

## Rebuild, deploy, cache

```sh
cmake --build build
```

Default: Ninja + Clang, `RelWithDebInfo` (`-O2 -g`). Runtime relinks deploy `games/Game[.exe]` beside
matching `Game_code.so/.dll`; no-op builds skip deployment. Recompiler/ABI edits require [module rebuilds](DEVELOPMENT.md).

CPM: [SHA-256-pinned sources](../cmake/dependencies.cmake), shared `.cache/cpm` cache, offline configure when populated.
Override via CMake/env `CPM_SOURCE_CACHE`. SDL3: prefer the system development package on Unix,
otherwise build the pinned static version. Windows always uses the pinned static build.

## Build variants

Persistent per-build-dir options (POSIX examples):

```sh
cmake --preset default -B build -DSNX_FFMPEG_ASM=OFF && cmake --build build
```

- `-DSNX_FFMPEG_ASM=OFF`: omit x86-64 NASM assembly; slower FFmpeg decoding.
- `-DSNX_H264=OFF`: disable FFmpeg, including NVDEC video and hwopus Opus decoding.
- `-DSNX_BUNDLED_SDL3=ON`: build the pinned SDL3 instead of using a system SDL3 development package.
- `-DSNX_DEPLOY=OFF`: disable runtime deployment.
- `-DSNX_GAMES_DIR=/path`: override deployment dir (default: `games/`).
- `-DCPM_SOURCE_CACHE=/path`: override source-download cache.

```sh
# Vulkan without SDL3, a display server, a surface or a swapchain:
cmake --preset default -B build/headless-vk -DSNX_BACKEND=headless -DSNX_DEPLOY=OFF &&
cmake --build build/headless-vk
# -> build/headless-vk/scarletnx, Vulkan renderer, always headless

cmake --preset windows-cross && cmake --build --preset windows-cross
# -> build/windows-cross/scarletnx.exe; requires llvm-mingw on PATH
```

The normal SDL3 build also supports `-H` at runtime. Omit the flag for windowed startup.
The no-SDL3 builds cannot switch to a window. [Software Vulkan / lavapipe setup](USAGE.md#headless-vulkan--lavapipe).

Offscreen integration test, with an installed Vulkan 1.3 ICD (lavapipe works):

```sh
cmake --preset default -B build/headless-vk -DSNX_BACKEND=headless \
  -DSNX_DEPLOY=OFF -DSNX_TEST_VULKAN=ON && cmake --build build/headless-vk
# Set VK_DRIVER_FILES to your lavapipe manifest to force CPU rendering.
SNX_VK_VALIDATION=1 ctest --test-dir build/headless-vk -R vulkan_headless --output-on-failure
```

The test uses synthetic pixels, not a game. It checks offscreen submissions, frame recycling, cropping,
readback hashes and PPM contents without WSI. Validation requires the Khronos validation layer to be installed.
`SNX_TEST_VULKAN` is off by default so ordinary unit tests do not require an ICD.

Cross-builds omit host hactool. Use a native driver for `-x`; `-r --target win64`
uses llvm-mingw and builds the cross runtime in `build/win64-recomp`.

Host tools only, without SDL/Vulkan/FFmpeg or Python:

```sh
cmake --preset default -B build/tools -DSNX_TOOLS_ONLY=ON && cmake --build build/tools
build/tools/scarletnx -r -h
ctest --test-dir build/tools --output-on-failure
```

Extraction builds missing hactool separately. Recompilation deploys the running full runtime;
the tools-only build creates one in `build/native-recomp`, or accepts `--runtime FILE`.
`--repo DIR` and `--build-dir DIR` before the command override compiled-in checkout/tool paths.
Python ≥3.12 is optional for `tools/*.py` diagnostics.

## Install / AppImage

```sh
cmake --install build --prefix stage
# -> stage/bin/, stage/share/licenses/scarletnx/
```

AppImage: run inside pkgforge's Arch container with `quick-sharun` ([CI setup](../.github/workflows/build.yml)):

```sh
cmake --preset default -B build/appimage -DSNX_DEPLOY=OFF -DCMAKE_INSTALL_PREFIX=/usr &&
cmake --build build/appimage &&
cmake --install build/appimage &&
GITHUB_REPOSITORY=scarletnx/ScarletNX scripts/make-appimage.sh
# -> dist/ScarletNX-<arch>.AppImage
```

[Nightly AppImage](https://github.com/scarletnx/ScarletNX/releases/tag/nightly) · [Launch](USAGE.md).

[Licenses/credits](CREDITS.md) · [Architecture](ARCHITECTURE.md) · [Roadmap](ROADMAP.md).
