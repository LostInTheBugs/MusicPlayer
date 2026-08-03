# MusicPlayer

A **MP3 / MP4 audio player for Windows 11**, written in C (Win32 API),
cross-compiled from Linux with MinGW-w64.

- Decoding: **FFmpeg** (libavformat / libavcodec / libswresample)
- Audio output: **miniaudio** (WASAPI / DirectSound / WinMM)
- Speed from 0.5× to 2× in 0.5 steps · volume 0–200 % (boost included) · stop resets to 0 s
- Drag & drop files, keyboard shortcuts
- **Play/Pause/Stop/Next buttons + volume slider** (0–100 % blue, 100–200 % orange boost) built into the window
- **Folder playback**: File ▸ Open folder… (or drag & drop a folder, or pass it on the command line) — all MP3/MP4 are played in order, subfolders included, next track starts automatically
- **Web server (remote control)**: Settings ▸ Web server… — open the page from your phone/tablet on the same network: control playback (play/pause, stop, next, volume, speed), see the playlist with the current track highlighted, and pick the audio output: this computer, phone, or both simultaneously (WAV stream)
- **Fullscreen** for visual plugins (F11 or the ⛶ button)
- **Progress bar** with color gradient
- **Update checker**: manual (Settings ▸ Check for updates…) and automatic at startup (compares with the latest GitHub release)
- **Multilingual**: plain-text `lang/*.lang` files (English built-in, French provided, Settings ▸ Language menu — see [lang/README.md](lang/README.md))
- **Plugin architecture** (skins, audio effects, visuals) — API version 2:
  color spectrum, rotating 3D spectrum (rainbow), 3D isometric landscape,
  LED VU meter, fireworks, fractal plasma, hypnotic tunnel — all synced to the music

## Version

`2026.08.018-c4` — see [CHANGELOG.md](CHANGELOG.md)

## Run on Windows 11

1. Copy the `bin/` folder (or unzip `dist/MusicPlayer-2026.08.014-win64.zip`)
   to the Windows machine.
2. Run `MusicPlayer.exe`. No installation required
   (the FFmpeg DLLs are in the same folder).

> The `plugins/` folder must sit next to `MusicPlayer.exe`:
> it is created automatically on first launch if missing.

## Development on Linux (this repo)

Ubuntu prerequisites:

```bash
sudo apt install gcc-mingw-w64-x86-64 wine64 ffmpeg zip
```

Vendor dependencies (not stored in the repo — see `.gitignore`):

```bash
mkdir -p vendor/ffmpeg
# miniaudio (single header)
curl -L -o vendor/miniaudio.h \
  https://raw.githubusercontent.com/mackron/miniaudio/v0.11.25/miniaudio.h
# FFmpeg win64 shared build (BtbN) — extract into vendor/ffmpeg/
# https://github.com/BtbN/FFmpeg-Builds/releases (win64-gpl-shared)
```

Build and test:

```bash
make          # → bin/MusicPlayer.exe (+ FFmpeg DLLs)
make test     # builds, generates test files and plays them under Wine
make zip      # portable Windows archive
```

### macOS (cross-compile to Windows)

The same Makefile works on macOS with the MinGW-w64 toolchain:

```bash
brew install mingw-w64
make          # → bin/MusicPlayer.exe — identical output, no code change
make zip      # portable Windows archive
```

`make test` needs Wine (optional on macOS: `brew install --cask wine-stable`).

### Native Linux / macOS build

The UI is pure **Win32**, so the player itself only targets Windows.
The audio engine (FFmpeg + miniaudio) is portable, but no native
frontend is provided — building the engine natively on Linux/macOS
would require pairing it with a native UI (not included).

## Usage

| Action | Menu | Shortcut |
|---|---|---|
| Open MP3/MP4 | File ▸ Open… | Ctrl+O (or drag & drop) |
| Open folder (playlist) | File ▸ Open folder… | drag & drop a folder |
| Play / Pause | window button | Space |
| Stop (reset to 0 s) | window button | S |
| Next track | window button ⏭ | N |
| Speed 0.5× / 1× / 1.5× / 2× | Settings ▸ Speed | — |
| Volume | window slider | ↑ / ↓ |
| Fullscreen | Settings ▸ Fullscreen | F11 (or ⛶ / Esc) |
| Check for updates | Settings ▸ Check for updates… | — |

## Plugins

Plugins are DLLs loaded from `plugins/` (next to the exe).
Three types are defined by the API v2:

- **Skin** (`MP_PLUGIN_SKIN`) — customizes the window
- **Audio effect** (`MP_PLUGIN_AUDIO_EFFECT`) — real-time PCM processing
- **Visual** (`MP_PLUGIN_VISUAL`) — rendering in the display area

See [plugins/README.md](plugins/README.md) and `src/plugin.h` for the API.
Menu **Plugins ▸ Reload** to load/unload without restarting.

## Project layout

```
MusicPlayer/
├── Makefile               # cross-build Linux → Windows
├── src/
│   ├── main.c             # Win32 UI (menus, buttons, slider, D&D, i18n)
│   ├── player.c/.h        # engine: FFmpeg + miniaudio + ring buffer
│   ├── plugin.h           # plugin API (v2)
│   ├── plugin_loader.c/.h # plugin scan/loading
│   ├── lang.c/.h          # i18n engine (lang/*.lang)
│   └── update.c/.h        # update checker (GitHub releases, WinINet)
├── lang/                  # language files (en, fr)
├── plugins/               # plugins to drop in (see README)
├── examples/              # plugin sources (built by make plugins-examples)
├── vendor/                # miniaudio.h + FFmpeg win64 (BtbN)
├── bin/                   # exe + DLLs (what goes to Windows)
├── test/                  # generated test files
└── dist/                  # portable archives
```

## Development cost (LLM)

This project was built entirely through AI-assisted sessions (Hermes Agent, deepseek-v4-flash). Usage so far:

| Metric | Value |
|---|---|
| Input tokens | 673 422 |
| Output tokens | 668 229 |
| **Total (input + output)** | **1 341 651** |
| Cache read (reused at reduced price) | 123 713 408 |
| API calls | 479 |
| **Estimated cost** | **≈ 0.63 USD** |

Full breakdown: [TOKENS.md](TOKENS.md).

## License

MIT — see [LICENSE](LICENSE).
