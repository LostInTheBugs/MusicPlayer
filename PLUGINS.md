# Writing MusicPlayer plugins

MusicPlayer is extensible through plugins: small Windows DLLs that are
dropped into a folder — **no compilation of the main program needed**.
This guide explains the plugin API (version 4), how to build a plugin,
how to install it, and how to write a skin.

## 1. Where plugins live

| Folder | Loaded by | Purpose |
|---|---|---|
| `plugins/` (next to `MusicPlayer.exe`) | client | visuals, audio effects, skins, client services (TeamSpeak…) |
| `skins/` (next to `MusicPlayer.exe`) | client | skins (kept separate since v026) |
| `core_plugins/` (next to `musicplayer-core.exe`) | engine | network services: web server, REST API, UPnP, RTP, Multiroom, cover, metadata |

**Installing a third-party plugin is a simple file copy** into the right
folder. The player loads it at startup (or hot-reloads it if you click
*Reload plugins* in the Plugins menu when present).

## 2. The minimal plugin

A plugin is a DLL exporting one function:

```c
const mp_plugin_api* mp_plugin_entry(void);
```

```c
#include "plugin.h"

static const char* name(void)        { return "My plugin"; }
static const char* version(void)     { return "1.0"; }
static const char* description(void) { return "What it does"; }
static unsigned type(void)           { return MP_PLUGIN_SERVICE; }

static const mp_plugin_api api = {
    .api_version = MP_PLUGIN_API_VERSION,
    .name = name,
    .version = version,
    .description = description,
    .type = type,
};

const mp_plugin_api* mp_plugin_entry(void) { return &api; }
```

Only `name()` is mandatory. Every other hook is optional and is only
called when the matching type is declared in `type()` **and** the plugin
is enabled (Settings ▸ Plugins).

### Plugin types (combinable with `|`)

| Flag | Hook | Purpose |
|---|---|---|
| `MP_PLUGIN_SKIN` | `apply_skin` + host `skin_*` | customizes the window look |
| `MP_PLUGIN_AUDIO_EFFECT` | `process` | processes the PCM stream (EQ…) |
| `MP_PLUGIN_VISUAL` | `audio_frames` + `render` | visualizer (spectrum…) |
| `MP_PLUGIN_SERVICE` | `service`, `get_title`, `get_metadata` | background feature (web server, tags…) |

## 3. Building a plugin

With MinGW-w64 (cross-compiling from Linux or natively on Windows):

```sh
x86_64-w64-mingw32-gcc -O2 -shared -o myplugin.dll myplugin.c \
    -Isrc -Ivendor -static-libgcc
```

Copy `myplugin.dll` into the right folder (see table above), restart the
player (or reload plugins), then enable it in **Settings ▸ Plugins…**.

The repository ships working examples in `examples/`:
`plugin_gaindemo.c` (effect), `plugin_spectrum.c` (visual),
`plugin_ts.c` (client service), `plugin_webserver.c`,
`plugin_restapi.c`, `plugin_upnp.c`, `plugin_rtp.c`,
`plugin_multiroom.c` (engine services), `plugin_equalizer.c`.

## 4. The host API (what the player offers you)

Your `init()` hook receives `const mp_host_api* host` — keep it and call
it from any thread. The important entry points:

```c
host->log(msg);                    /* write to musicplayer.log */
host->get_state();                 /* 0 stopped, 1 playing, 2 paused… */
host->get_position();              /* seconds */
host->get_duration();
host->get_volume();  host->set_volume(v);
host->get_speed();   host->set_speed(s);
host->play_pause();  host->stop();  host->next();
host->plist_count(); host->plist_name(i); host->plist_play(i);
host->get_metadata(path, "title");  host->get_cover(path, &len);
host->main_window();               /* HWND of the main window */
```

### Network services (engine plugins only)

The core's service plugins use:

```c
host->svc_port("rtp");             /* configured port or default */
host->svc_ips("rtp");              /* "ip1;ip2" or "" = all interfaces */
host->web_reader_open();           /* reserve YOUR reader on the stream */
host->web_read_n(rid, buf, frames);/* read PCM stereo float 44.1 kHz */
host->web_reader_close(rid);
```

> **Important**: any plugin that broadcasts the audio stream **must
> reserve its own reader** (`web_reader_open`). Sharing the plain
> `web_read` cursor steals samples from the other consumers (client,
> phone, other services) and produces choppy sound everywhere.

A service plugin receives the `MP_SERVICE_WEB_APPLY` event when the
network configuration changes — re-read `svc_port`/`svc_ips` there.

## 5. Writing a skin

A skin is a plugin of type `MP_PLUGIN_SKIN`. At load time the player
calls `apply_skin(self, hwnd)`, and from there you shape the interface
through the host API:

```c
/* full color palette */
static const mp_skin_colors colors = {
    .bg = 0x001018, .text = 0xE8F0F8, .accent = 0x2F6FE4, …
};
host->skin_set_colors(&colors);

/* optional background image (PNG/JPEG/BMP, UTF-8 path) */
host->skin_set_bg("C:\\MusicPlayer\\skins\\mybg.png");

/* visualizer zone (window-relative) */
host->skin_set_visual_rect(10, 40, 620, 180);

/* layout: hide menu (right-click to reopen), controls on top… */
host->skin_set_layout(0, 1, 1);   /* menu hidden, ctrl top, status shown */

/* full-window skin: impose the exact artwork size, non-resizable */
host->skin_set_window_size(640, 300, 1);
```

The palette (`mp_skin_colors` in `plugin.h`) is applied instantly; the
background image is stretched or displayed 1:1 depending on
`skin_set_window_size`. The player resets the skin state between skins,
so you can restore a clean state by simply not calling these functions.

Skins also receive the `render` hook **if** they declare the VISUAL type
too — that is how a skin hosts its own visualizer inside the artwork
(e.g. the vintage radio dial).

## 6. Lifecycle and threads

- `init(self, host)`: called once at load (return non-zero on failure).
- `destroy(self)`: called at shutdown — free everything, stop threads.
- `process` / `audio_frames`: called from the **audio thread** — do fast
  work, never block, never allocate. Copy the data if you need it later.
- `render`: called from the UI thread ~30 FPS — draw with GDI.
- `service`: called from the plugin thread (network services spawn their
  own listening threads — remember to close sockets in `destroy`).

## 7. Versioning and the plugin repository

Plugins are versioned as `core.NNN` (see the `plugins.json` manifest);
the update checker can fetch a newer DLL without a program update.

Third-party plugins can be distributed through any **plugin repository**
(see Settings ▸ Plugin repository…): a plain HTTP(S) URL serving a
`plugins.json` index plus the DLL files. The default repository is the
project's own (https://github.com/LostInTheBugs/MusicPlayer) and lists
the project's plugins and skins — download them from inside the app, no
manual copying needed.
