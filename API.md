# MusicPlayer Core — API publique (client/serveur)

Depuis la version 2026.08.039, le moteur (`musicplayer-core.exe`) est
séparé de l'interface (`MusicPlayer.exe`). Le moteur expose une API
REST standard sur le port **8080** (configurable via `svc_rest_port`
dans `config.yml`, ou Settings ▸ Network… côté client).

Toute commande POST doit porter l'en-tête `Content-Type:
application/json` (anti-CSRF : une requête « simple » d'un site tiers
est refusée 403).

## Endpoints

| Méthode | Chemin | Description |
|---|---|---|
| GET | `/health` | `ok` si le moteur tourne |
| GET | `/api/state` | État complet (JSON) |
| GET | `/api/plist` | Playlist (JSON) |
| GET | `/api/cover` | Jaquette du morceau courant (JPEG/PNG) |
| GET | `/stream` | Flux audio PCM WAV 44,1 kHz stéréo 16 bits |
| GET | `/api/levels` | Niveaux audio `{l, r}` (visuels du client) |
| POST | `/api/cmd` | Commande (JSON) |

## GET /api/state

```json
{
  "state": 1,          /* 0 stopped, 1 playing, 2 paused, 3 finished */
  "pos": 12.345,       /* position en secondes (temps du morceau) */
  "dur": 210.0,        /* durée en secondes */
  "idx": 3,            /* index du morceau dans la playlist */
  "count": 12,         /* nombre de morceaux */
  "speed": 1.00,       /* 0.5 .. 2.0 */
  "shuffle": 0,        /* 0/1 */
  "name": "01_a.mp3",  /* nom de fichier */
  "title": "…", "artist": "…", "album": "…", "year": "…",
  "items": 12
}
```

## POST /api/cmd

Corps JSON, exemples :

```sh
curl -X POST -H "Content-Type: application/json" \
     -d '{"cmd":"play"}'      http://127.0.0.1:8080/api/cmd

curl -X POST -H "Content-Type: application/json" \
     -d '{"cmd":"open","path":"C:\\Music"}'  http://127.0.0.1:8080/api/cmd
```

| Commande | Paramètres | Effet |
|---|---|---|
| `play` / `pause` / `playpause` | — | Lecture / pause |
| `stop` | — | Arrêt, position 0 |
| `next` / `prev` | — | Morceau suivant / précédent |
| `seek` | `value` (secondes) | Déplacement |
| `speed` | `value` (0.5–2.0) | Vitesse |
| `volume` | `value` (0.0–1.0) | Volume (état ; le client applique) |
| `shuffle` | — | Bascule aléatoire |
| `playidx` | `value` (index) | Joue l'index de la playlist |
| `open` | `path` (dossier ou fichier) | Ouvre un dossier en playlist ou un fichier |
| `shutdown` | — | Arrête le moteur proprement |

## Exemple de client minimal (Python)

```python
import requests

r = requests.get("http://127.0.0.1:8080/api/state").json()
print(r["title"], f"{r['pos']:.0f}/{r['dur']:.0f}s")

requests.post("http://127.0.0.1:8080/api/cmd",
              json={"cmd": "play"})
```

## Flux /stream

Flux brut (pas d'en-tête HTTP) : en-tête WAV 44 octets puis PCM
16 bits petit-boutiste, 44 100 Hz, stéréo, volume non appliqué
(le client applique son propre volume). La position du moteur avance
au rythme des clients qui consomment le flux.

## Niveaux /api/levels

`{"l": 0.0532, "r": 0.0478}` — RMS par canal (0.0–1.0) du dernier
bloc diffusé. Les clients visuels peuvent s'en servir (polling ~10 Hz).
