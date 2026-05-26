# Hide When Live

An OBS Studio source filter plugin that automatically hides sources during live scene transitions. Apply the filter to any image or video source and it will be invisible when the scene it belongs to is transitioning into the program output — no manual hiding of sources required.

---

## Features

- **Automatic hide on transition-in**: When a scene containing a filtered source is about to go live, the source is hidden at the moment the transition begins. The viewer never sees it appear mid-transition.
- **Automatic restore on transition-out**: Once the scene has fully left the program output and the transition video has completed, the source is made visible again — ready for the next time that scene goes live.
- **Enabled/disabled toggle**: Use OBS's built-in filter enable/disable checkbox to turn the behavior on or off per source without removing the filter.

---

## Installation

1. Copy `hide-when-live.dll` to:
   ```
   C:\Program Files\obs-studio\obs-plugins\64bit\
   ```
2. Copy `en-US.ini` to:
   ```
   C:\Program Files\obs-studio\data\obs-plugins\hide-when-live\locale\
   ```
3. Restart OBS Studio.
4. Right-click any source > **Filters** > **Add** > **Hide When Live**.

---

## Plugin Statistics

| Property | Value |
|---|---|
| Plugin version | 1.0.0 |
| OBS Studio version tested | 31.1.1 |
| Target platform | Windows x64 |
| DLL size (RelWithDebInfo) | ~28.5 KB (29,184 bytes) |
| Filter type | Source filter (`OBS_SOURCE_TYPE_FILTER`) |
| Output flags | `OBS_SOURCE_VIDEO` |
| Filter ID | `hwl_filter` |
| Dependencies | `obs-frontend-api` (included with OBS Studio) |
| Build configuration | RelWithDebInfo |
| License | GPL-2.0-or-later |

---

## Building from Source

Requires CMake and Visual Studio 2022 on Windows.

**Configure (first time only):**
```
cmake --preset windows-x64
```

**Build:**
```
cmake --build build_x64 --config RelWithDebInfo
```

Output: `build_x64\RelWithDebInfo\hide-when-live.dll`

Dependencies (OBS headers, obs-deps, Qt6) are downloaded automatically by the CMake configure step via `buildspec.json`.

---

## AI Coding Disclaimer

This plugin was written by [Claude](https://claude.ai) (Anthropic's AI assistant). During development, Claude was explicitly instructed to:

- Strictly adhere to the OBS Studio coding style guidelines:
  [https://github.com/obsproject/obs-studio/blob/master/CODESTYLE.md](https://github.com/obsproject/obs-studio/blob/master/CODESTYLE.md)
- Build on top of the official OBS Plugin Template as the project foundation:
  [https://github.com/obsproject/obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
- Follow OBS's existing plugin conventions, using the [freeze-filter](https://github.com/exeldro/obs-freeze-filter) plugin by Exeldro soley as a style reference.

The resulting code follows OBS-standard patterns: two-pass scene enumeration to avoid deadlocks, correct reference counting on OBS sources, mutex ordering that matches the OBS signal dispatcher's lock hierarchy, and frontend API event handling consistent with other first-party plugins.

Human reviewing and testing was performed throughout development.
