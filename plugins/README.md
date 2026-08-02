# MusicPlayer Plugins

Drop plugin DLLs here: they are loaded at startup
(menu **Plugins ▸ Reload** to reload without restarting).

## Plugin types (API v2 — see src/plugin.h)

| Type | Bit | Description |
|---|---|---|
| Skin | `MP_PLUGIN_SKIN` | customizes the window appearance (`apply_skin`) |
| Audio effect | `MP_PLUGIN_AUDIO_EFFECT` | real-time PCM processing (`process`) |
| Visual | `MP_PLUGIN_VISUAL` | rendering in the display area (`render`, ~30 FPS) |

**Visual** plugins also receive the audio stream through `audio_frames()`
(read-only, after effects) for analysis (spectrum, oscilloscope…).

## Plugin DLL contract

1. Export `const mp_plugin_api* mp_plugin_entry(void)` (name `mp_plugin_entry`).
2. `api_version` must equal `MP_PLUGIN_API_VERSION` (2).
3. Provide at least `name()` and `type()`.
4. Fill `init()` to receive the host API (`mp_host_api`): logging,
   player state, position, duration, volume, speed.
5. Optional hooks: `process()` (effects), `audio_frames()` + `render()`
   (visual), `apply_skin()` (skin). They are called only if `type()`
   declares them and the plugin is checked in the Plugins menu.

## Provided examples (examples/)

| Plugin | Type | Description |
|---|---|---|
| `plugin_gaindemo.c` | audio effect | halves the volume (minimal example) |
| `plugin_spectrum.c` | visual | spectrum: FFT 1024, 48 log bars, blue→red palette, halo, grid |
| `plugin_3dspectrum.c` | visual | rotating 3D spectrum, Spectrum3D style: 96 thin bars, rainbow by position, night-blue background |
| `plugin_3diso.c` | visual | 3D bar landscape in perspective, GLBars style: 24×14, rainbow, glowing gradients, scrolling time |
| `plugin_vumeter.c` | visual | stereo LED VU meter (-45..0 dB, peaks, clip) — Winamp/XMMS style |
| `plugin_fireworks.c` | visual | fireworks triggered by music beats |
| `plugin_fractal.c` | visual | per-pixel fractal plasma (256 colors, reacts to music) |
| `plugin_hypnotic.c` | visual | hypnotic tunnel: rotating rings pulsed by the music |

Compile with `-lgdi32 -lm` for visual plugins.

> **Multiple visual plugins**: only one is displayed at a time — the
> **Plugins ▸ Visual** menu is a radio group (check the one you want).

## Building a plugin

With MinGW (Linux):

```bash
x86_64-w64-mingw32-gcc -O2 -shared -o my_plugin.dll my_plugin.c \
    -Isrc -static-libgcc          # + -lgdi32 -lm for a visual
```

## Log

Plugin load/errors are written to `musicplayer.log`
(next to the exe) — handy to debug a DLL that fails to load.
