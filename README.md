# Citro: Invidious YouTube Client for Nintendo 3DS

**Citro** is an open-source, keyless YouTube client tailored for the Nintendo 3DS family of consoles using public [Invidious](https://invidious.io) instances. It is written in pure C for devkitARM, `libctru`, and `citro2d`.

---

## Features

- Uses public, open Invidious instances (`https://invidious.flokinet.to/api/v1/`) with pure HTTP GET requests.
- **Dual-Screen Layout:**
  - **Top Screen (400x240):** Video playback area with progress indicator, playback state (`PLAYING` / `PAUSED`), and an **active video title OSD banner that displays for 5 seconds** after launching any video.
  - **Bottom Screen (320x240 Resistive Touchscreen):** 
    - Search screen with quick tag chips (`"3ds"`, `"homebrew"`, `"chiptune"`).
    - Touch controls: Play/Pause toggle, touch seek bar with scrub knob, like count display, rewind (-10s), fast-forward (+10s), and a full description view button.
- **Memory-Efficient & 60 FPS Locked:** Zero heap dynamic memory allocations in the main render loop. Pre-allocated text buffers (`C2D_TextBuf`) and page-aligned socket buffer (`memalign(0x1000, 0x100000)`).
- **libctru Hardware Lifecycle:** Native `httpc` HTTPS client, `aptMainLoop()`, `hidScanInput()`, and double-buffered `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)`.

---

## File Structure

```text
├── citro_invidious.h    # Structs (VideoMetadata, SearchResults, AppState) & prototypes
├── citro_invidious.c    # Keyless HTTP GET and cJSON parsing into pre-allocated memory
├── main.c               # 3DS hardware init, socInit, dual-screen rendering, touch loop
├── cJSON.h              # Lightweight ANSI C JSON parser header
├── Makefile             # devkitARM / devkitPro build configuration
└── src/                 # Web-based dual-screen 3DS simulator & C code studio
```

---

## Building with devkitPro

### 1. Install Dependencies

Using `dkp-pacman` on Linux, macOS, or Windows (MSYS2):

```bash
sudo dkp-pacman -S 3ds-dev libctru citro2d citro3d
```

### 2. Set Environment Variables

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
```

### 3. Compile

```bash
make
```

This will produce `Citro.3dsx` in the project root.

### 4. Running on Hardware

Copy `Citro.3dsx` to your Nintendo 3DS SD card at `/3ds/Citro/Citro.3dsx` and launch it from the Homebrew Launcher.

---

## Security & Multi-Licensing

Citro is distributed under GPL3,Other License would be used for the project.
1. **Primary License**: [GNU General Public License v3.0 (GPL-3.0)](LICENSE)
2. **Material 3**: [Apache License 2.0 (Apache-2.0)](LICENSE-2)
3. **Material 3**: [Creative Commons Attribution 4.0 International(CC-BY 4.0)](LICENSE-3)
4. **Ubuntu** [Ubuntu Font License](FONT-LICENSE-1) (There are also multiple files under FONT-LICENSE-1,these are Copyright,Trademarks,FAQ,Contributing and Fontlog.)
5. **Andika** [Sil Open Font License](FONT-LICENSE-2)

SPDX-License-Identifier: `GPL-3.0-or-later OR Apache-2.0 OR CC-BY-SA-4.0`

