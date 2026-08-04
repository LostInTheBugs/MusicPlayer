# MusicPlayer

A **MP3 / MP4 audio player for Windows 11**, written in C (Win32 API),
cross-compiled from Linux with MinGW-w64.

- Decoding: **FFmpeg** (libavformat / libavcodec / libswresample)
- Audio output: **miniaudio** (WASAPI / DirectSound / WinMM)
- Speed from 0.5× to 2× in 0.5 steps · volume 0–200 % (boost included) · stop resets to 0 s
- Drag & drop files, keyboard shortcuts
- **Play/Pause/Stop/Next buttons + volume slider** (0–100 % blue, 100–200 % orange boost) built into the window
- **Folder playback**: File ▸ Open folder… (or drag & drop a folder, or pass it on the command line) — all MP3/MP4 are played in order, subfolders included, next track starts automatically
- **CD audio playback**: File ▸ Open CD… (MCI) — tracks listed as “CD Track N”, auto-advance
- **Web server (remote control)**: Settings ▸ Web server… — open the page from your phone/tablet: control playback, see metadata (title/artist/album/year), the cover, the playlist with the current track highlighted, and pick the audio output: this computer, phone, or both (WAV stream)
- **DJ Mixing**: 2-deck mixer on the web page (volume, pitch ±12 %, 3-band EQ, crossfader, via Web Audio API) **and** on the computer itself (console with per-deck play/pause/stop, volumes, crossfader, pitch — deck B is a real second decoder mixed into the output); the mode is synchronized both ways (Settings ▸ DJ Mixing ⇄ web)
- **Session persistence** (`config.yml` in `%APPDATA%\MusicPlayer`): volume, speed, last folder/file and web server config are saved on exit and restored on startup — playlists are rescanned (new files added, removed files dropped) and playback resumes on the current track
- **True fullscreen** (F11 or the ⛶ button): covers the monitor under the cursor; **multi-screen fullscreen** configurable in Settings ▸ Interface… — number of screens used (auto-detected) and per-screen content: visual effect, playlist, lyrics or cover
- **Playlist window** (Ctrl+L): numbered tracks, current track highlighted, double-click to play
- **Progress bar** with color gradient
- **Skins** (Plugins ▸ Skins): 11 skins — color palettes and **full-window skins** (radio, Winamp) with background image, per-skin layout (hidden menu + right-click, controls on top) and visualizer zone; choice in Settings ▸ Interface…
- **Update checker**: Settings ▸ Update… — automatic (at startup), manual, or disabled (compares with the latest GitHub release)
- **Multilingual**: plain-text `lang/*.lang` files (English built-in, French provided, Settings ▸ Interface… — see [lang/README.md](lang/README.md))
- **Plugin architecture** (skins, audio effects, visuals, services) — API version 2:
  - **Visuals**: color spectrum, rotating 3D spectrum (rainbow), 3D isometric landscape, LED VU meter, fireworks, fractal plasma, hypnotic tunnel — all synced to the music
  - **Effects**: Volume booster (+25 %), Sound Quality (sub-bass filter, bass boost, presence, soft limiter)
  - **Services**: Web server (8000), REST API (8080), DLNA/UPnP media server (8081), RTP/AES67 multicast output (5004), Multiroom, TeamSpeak Broadcast, MP3 metadata (ID3 tags), cover art, lyrics (.lrc)
  - **Network configuration**: Settings ▸ Network… — port and listening IPs per service

## Version

`2026.08.038-c1` — see [CHANGELOG.md](CHANGELOG.md)

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
# https://github.com/BtbN/FFmpeg-Builds/releases (win64-lgpl-shared)
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
| Input tokens | 3 841 278 |
| Output tokens | 2 400 025 |
| **Total (input + output)** | **6 241 303** |
| Cache read (reused at reduced price) | 462 257 024 |
| API calls | 1621 |
| **Estimated cost** | **≈ 2.32 USD** |

Full breakdown: [TOKENS.md](TOKENS.md).

## License

MIT — see [LICENSE](LICENSE).

**FFmpeg** (décodage audio) is linked dynamically and distributed as
separate binaries built from the **BtbN `win64-lgpl-shared`** builds
(FFmpeg n8.1, **LGPLv2.1+** — see `LICENSE-FFmpeg.txt` shipped in the
archive). The project's own source code stays under MIT; FFmpeg's
obligations (LGPL notice, source availability) are carried by the
bundled FFmpeg binaries.
