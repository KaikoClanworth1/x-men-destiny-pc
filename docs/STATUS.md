# State of the port

The port has four goals. This page is kept up to date as the work goes on.

| Goal | State |
|---|---|
| **1. The game runs** | ✅ Done. Intro movies, title, menus, character selection, cut-scenes and gameplay. |
| **2. Fully native**: no emulated graphics or sound, no stutter, above 60 fps | 🔧 In progress. |
| **3. PC settings inside the game's own menus**: graphics, controller, key remapping | 🔧 Started. *Exit* and *Options → Graphics* are in. Controller and remapping pages aren't. |
| **4. An easy installer** | 🔧 First release out (v0.1.0). The setup has only been tested on the developer's PC so far. |

## What works

- The game from start-up into the missions. Not every mission has been played through.
- **Native renderer**: the game's own Direct3D calls drawn through [plume](https://github.com/renderbag/plume) on Direct3D 12, at higher resolutions than the console's 720p, with anti-aliasing and anisotropic filtering.
- **Sound output through XAudio2**, in stereo or 5.1.
- **Controllers** through SDL (Xbox, PlayStation, Switch Pro and others). Rumble is passed through but untested.
- **Exit** on the main menu, and **Options → Graphics**: fullscreen, resolution, anti-aliasing, VSync, frame-rate limit, shadows, texture quality and texture filtering. Settings are saved.
- **A mod menu** (F1): cheats, warps to all 93 checkpoints (missions and challenge arenas), 42 cut-scenes, game speed.
- **F4 captures** and a problem log, for reporting faults.

## What doesn't yet

- **In v0.1.0 the native renderer is switched off by default**, so that release draws with the emulated renderer unless you turn it on. From the next release it is the default on Direct3D 12, and the emulated renderer takes over by itself (the window title says why) when the native one can't start or its prepared shaders are missing.
- **Direct3D 11 and Vulkan** renderers exist but aren't finished.
- **Picture faults** in the native renderer: some shadow artefacts, flicker on some particle effects, objects appearing late.
- **Sound decoding** still goes through the SDK's emulated XMA decoder (FFmpeg). There is no PC volume setting yet.
- **The game ran at 30 fps on the console.** A frame-rate limit setting exists; running above 30 is experimental.
- **Keyboard and mouse** aren't in v0.1.0. They are in the source for the next release (see below), tested with scripted input only, not yet with real devices.
- **A remap page in the game's menu** is in progress. Remapping itself comes with the next release (see below), by editing a settings file.
- **Button prompts** always show Xbox 360 buttons in v0.1.0. Prompts that follow your device come with the next release (see below).
- **Mod menu**: there is no direct suit equip, and only the 42 cut-scenes that play on their own are listed.
- The setup recognises one release of the game; other regions' discs are refused as "Wrong version".
- Languages other than English, 5.1 output and changing the sound device while playing are untested.

## Coming in the next release

These are in the source already, not in the v0.1.0 download:

- **The native renderer as the default** on Direct3D 12, with the emulated renderer taking over by itself when it can't start.
- **Keyboard and mouse play**, on by default beside the controller: WASD to move, mouse for the camera, left / right mouse button for light / heavy attack, Space to jump, Shift to dodge, Q or the middle button to block, R for X-Mode, E for PowerShift, F to focus the camera, arrow keys for the d-pad, Escape to pause, Tab for Back. In menus, Enter accepts and Backspace goes back. Settings `mouse_sensitivity` and `mouse_invert_y`.
- **Remapping**: 20 game actions, each rebindable for keyboard and mouse and for the controller, kept in `controls.toml` in the game folder (delete it for the defaults). A key or button already in use swaps places. For now it is changed by editing that file, for example `key.jump = "G"` or `pad.jump = "y"`.
- **Button prompts that follow the device in use**, switching while you play: the game's own pictures on an Xbox-style pad, keys and mouse buttons on keyboard and mouse, PlayStation or Nintendo buttons on those pads, and your own button where you changed one. Only with the native renderer. The big controller drawing on *Options → Controls* is still the Xbox 360 pad.

All of the above was tested with scripted input only. Real keyboards, mice, PlayStation and Nintendo pads haven't been tried by hand yet.
