# MusicPlayer Core — Public API (client/server)

Since version 2026.08.039 the engine (`musicplayer-core.exe`) is
separate from the interface (`MusicPlayer.exe`). The engine exposes a
standard REST API on port **8080** (configurable via `svc_rest_port` in
`config.yml`, or Settings ▸ Network… on the client).

Every POST command must carry the `Content-Type: application/json`
header (anti-CSRF: a “simple” request from a third-party site is
refused with 403).

## Endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/health` | `ok` when the engine is running |
| GET | `/api/state` | Full state (JSON) |
| GET | `/api/plist` | Playlist (JSON) |
| GET | `/api/cover` | Cover art of the current track (JPEG/PNG) |
| GET | `/stream` | PCM WAV audio stream, 44.1 kHz stereo 16-bit |
| GET | `/api/levels` | Audio levels `{l, r}` (client visuals) |
| POST | `/api/cmd` | Command (JSON) |
| POST | `/api/config` | Live network configuration (JSON body) |

## GET /api/state

```json
{
  "state": 1,          /* 0 stopped, 1 playing, 2 paused, 3 finished */
  "pos": 12.345,       /* position in seconds (track time) */
  "dur": 210.0,        /* duration in seconds */
  "idx": 3,            /* index of the current track in the playlist */
  "count": 12,         /* number of tracks */
  "speed": 1.00,       /* 0.5 .. 2.0 */
  "shuffle": 0,        /* 0/1 */
  "name": "01_a.mp3",  /* file name */
  "title": "…", "artist": "…", "album": "…", "year": "…",
  "items": 12
}
```

## POST /api/cmd

JSON body, examples:

```sh
curl -X POST -H "Content-Type: application/json" \
     -d '{"cmd":"play"}'      http://127.0.0.1:8080/api/cmd

curl -X POST -H "Content-Type: application/json" \
     -d '{"cmd":"open","path":"C:\\Music"}'  http://127.0.0.1:8080/api/cmd
```

| Command | Parameters | Effect |
|---|---|---|
| `play` / `pause` / `playpause` | — | Play / pause |
| `stop` | — | Stop, position 0 |
| `next` / `prev` | — | Next / previous track |
| `seek` | `value` (seconds) | Seek |
| `speed` | `value` (0.5–2.0) | Speed |
| `volume` | `value` (0.0–1.0) | Volume (state; the client applies it) |
| `shuffle` | — | Toggle shuffle |
| `playidx` | `value` (index) | Play the playlist index |
| `open` | `path` (folder or file) | Open a folder as playlist or a file |
| `shutdown` | — | Stop the engine cleanly |
| `dj_open_b` | `path` (file) | Load deck B of the DJ mixer |
| `dj_play_b` | — | Play/pause deck B |
| `dj_stop_b` | — | Unload deck B |
| `dj_xf` | `value` (0.0–1.0) | Crossfader A/B |
| `dj_vol_a` | `value` (0.0–1.5) | Volume of deck A |
| `dj_vol_b` | `value` (0.0–1.5) | Volume of deck B |

## POST /api/config

Live network configuration (no restart needed):

```sh
curl -X POST -H "Content-Type: application/json" \
     -d '{"web_enabled":1,"web_port":8000,"web_ips":"192.168.1.10"}'
```

| Field | Meaning |
|---|---|
| `web_enabled` | 0/1 — web server on/off |
| `web_port` | port of the web server |
| `web_ips` | `"ip1;ip2"` or empty = all interfaces |

## Minimal client example (Python)

```python
import requests

r = requests.get("http://127.0.0.1:8080/api/state").json()
print(r["title"], f"{r['pos']:.0f}/{r['dur']:.0f}s")

requests.post("http://127.0.0.1:8080/api/cmd",
              json={"cmd": "play"})
```

## /stream

Raw stream (no HTTP header): 44-byte WAV header then little-endian
16-bit PCM, 44 100 Hz, stereo, volume not applied (the client applies
its own volume). The engine position advances at the pace of the
clients consuming the stream.

## /api/levels

`{"l": 0.0532, "r": 0.0478}` — per-channel RMS (0.0–1.0) of the last
broadcast block. Visual clients can poll it (~10 Hz).
