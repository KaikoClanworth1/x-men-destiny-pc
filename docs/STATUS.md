# State of the port

The port has four goals. This page is kept up to date as the work goes on.

| Goal | State |
|---|---|
| **1. The game runs** | ✅ Done. Intro movies, title, menus, character selection, cut-scenes and gameplay. |
| **2. Fully native**: no emulated graphics or sound, no stutter, above 60 fps | 🔧 In progress. |
| **3. PC settings inside the game's own menus**: graphics, controller, key remapping | 🔧 Started. *Exit* and *Options → Graphics* are in. Controller and remapping pages aren't. |
| **4. An easy installer** | 🔧 The setup works on the developer's PC and is being tested. No release yet. |

## What works

- The game from start-up into the missions. Not every mission has been played through.
- **Native renderer**: the game's own Direct3D calls drawn through [plume](https://github.com/renderbag/plume) on Direct3D 12, at higher resolutions than the console's 720p, with anti-aliasing and anisotropic filtering.
- **Sound output through XAudio2**, in stereo or 5.1.
- **Controllers** through SDL (Xbox, PlayStation, Switch Pro and others). Rumble is passed through but untested.
- **Exit** on the main menu, and **Options → Graphics**: fullscreen, resolution, anti-aliasing, VSync, frame-rate limit, shadows, texture quality and texture filtering. Settings are saved.
- **A mod menu** (F1): cheats, warps to the game's checkpoints, cut-scenes, game speed.
- **F4 captures** and a problem log, for reporting faults.

## What doesn't yet

- **An installed game uses the emulated renderer for now.** The setup doesn't make the native renderer's shaders on the player's PC yet.
- **Direct3D 11 and Vulkan** renderers exist but aren't finished.
- **Picture faults** in the native renderer: some shadow artefacts, flicker on some particle effects, objects appearing late.
- **Sound decoding** still goes through the SDK's emulated XMA decoder (FFmpeg). There is no PC volume setting yet.
- **The game ran at 30 fps on the console.** A frame-rate limit setting exists; running above 30 is experimental.
- **Keyboard and mouse** are switched off by default, and the default key layout isn't made for this game yet.
- **Button prompts** always show Xbox 360 buttons.
- **Mod menu**: the Mission 7 warps don't work, and most of the cut-scene list is untested.
- The setup recognises one release of the game; other regions' discs are refused as "Wrong version".
- Languages other than English, 5.1 output and changing the sound device while playing are untested.
