# Building from source

This page is for developers. To just play, use the setup: see [How to install](../README.md#how-to-install).

The recompiled game code is **generated on your machine from your own disc**. It isn't stored in this repository.

## You need

- **Windows 10 or 11**, 64-bit.
- **LLVM** (`clang`, `clang++` and `llvm-rc`) in `C:\Program Files\LLVM\bin`.
- **CMake 3.25** or newer, and **Ninja**.
- **Git**, **Python 3** and **PowerShell**.
- Your own *X-Men: Destiny* Xbox 360 disc image (`.iso`).

## Steps

Run them from the repository root.

1. Get the **ReXGlue SDK** source into `recomp\rexglue-sdk`, at the commit this port was built against (its v0.10.0 release commit), and apply this port's changes:
   ```bash
   git clone https://github.com/rexglue/rexglue-sdk recomp/rexglue-sdk
   ```
   ```bash
   git -C recomp/rexglue-sdk checkout c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
   ```
   ```bash
   git -C recomp/rexglue-sdk submodule update --init --recursive
   ```
   ```bash
   git -C recomp/rexglue-sdk apply ../../port/patches/rexglue-sdk-xmd.patch
   ```
2. Get **plume** (the graphics layer the native renderer draws through) into `recomp\plume`, with this port's additions:
   ```bash
   git clone --recursive https://github.com/renderbag/plume.git recomp/plume
   ```
   ```bash
   git -C recomp/plume checkout d72379344dacd3dbf9f810f92ddc87e6de1845b1
   ```
   ```bash
   git -C recomp/plume submodule update --init --recursive
   ```
   ```bash
   git -C recomp/plume apply ../../port/patches/plume-xmd.patch
   ```
3. Get the **ReXGlue codegen tool**: download `rexglue-sdk-0.10.0-win-amd64.zip` from the [ReXGlue releases](https://github.com/rexglue/rexglue-sdk/releases/tag/v0.10.0) and unzip it into `recomp\sdk\win-amd64`.
4. Put **`ninja.exe`** in `recomp\bin`, or anywhere on your `PATH`.
5. **Extract your disc image** into a folder named `Game Files` at the repository root:
   ```bash
   python port/tools/extract_iso.py "path/to/your X-Men Destiny.iso" "Game Files"
   ```
6. Put a **program icon** at `port\res\xmd.ico` (any `.ico`; see `port/res/README.md`).
7. **Generate** the C++ code from your `Default.xex`:
   ```bash
   cd port && ../recomp/sdk/win-amd64/bin/rexglue.exe codegen xmd_manifest.toml
   ```
8. **Build**, and copy the program beside your game files (the first build takes a while):
   ```bash
   powershell -ExecutionPolicy Bypass -File port/build.ps1 -Deploy
   ```
9. Optionally, the **native renderer's shaders**. They're converted from the game's own shaders on your disc with [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (commit `990d03b`, with `port/patches/xenosrecomp-xmd.patch`, built in `recomp\XenosRecomp`): run `port/tools/find_shaders.py`, then `port/tools/convert_shaders.py`. Without them the game uses the emulated renderer.

Then start `Game Files\xmd.exe`. Saves, settings, captures and logs go to `Game Files\UserData\`.

## Repository layout

| Path | What's there |
|---|---|
| `port/src/` | The PC side: the app, sound output, the menu pages, the mod menu, captures, crash reports and the native renderer (`port/src/native/`). |
| `port/setup/` | The setup program players use (`XMDSetup.exe`): it builds the game on their PC from their own copy. |
| `port/patches/` | This port's changes to the ReXGlue SDK, plume and XenosRecomp. |
| `port/tools/` | Build, analysis and background-test scripts. |
| `port/xmd_manifest.toml`, `port/xmd_config.toml` | The codegen project and its per-game settings. |
| `docs/` | These pages and the pictures. |

## Licences

The port's own code and docs are under the [MIT licence](../LICENSE). The patches in `port/patches` are under the licences of the projects they patch: the ReXGlue SDK (BSD-3-Clause, partly derived from Xenia), plume (MIT) and XenosRecomp (MIT). No game material is covered or included.
