# Plugins MusicPlayer

Déposez ici les DLL de plugins : elles sont chargées au démarrage
(menu **Plugins ▸ Recharger** pour recharger sans redémarrer).

## Types de plugins (API v2 — voir src/plugin.h)

| Type | Bit | Description |
|---|---|---|
| Skin | `MP_PLUGIN_SKIN` | personnalise l'apparence de la fenêtre (`apply_skin`) |
| Effet audio | `MP_PLUGIN_AUDIO_EFFECT` | traitement PCM temps réel (`process`) |
| Visuel | `MP_PLUGIN_VISUAL` | rendu dans la zone d'affichage (`render`, ~30 FPS) |

Les plugins **visuels** reçoivent aussi le flux audio via `audio_frames()`
(lecture seule, après les effets) pour l'analyse (spectre, oscilloscope…).

## Contrat d'une DLL de plugin

1. Exporter `const mp_plugin_api* mp_plugin_entry(void)` (nom `mp_plugin_entry`).
2. `api_version` doit valoir `MP_PLUGIN_API_VERSION` (2).
3. Fournir au minimum `name()` et `type()`.
4. Remplir `init()` pour recevoir l'API hôte (`mp_host_api`) : journalisation,
   état du lecteur, position, durée, volume, vitesse.
5. Hooks optionnels : `process()` (effets), `audio_frames()` + `render()`
   (visuel), `apply_skin()` (skin). Ils ne sont appelés que si `type()` les
   déclare et si le plugin est coché dans le menu Plugins.

## Exemples fournis (examples/)

| Plugin | Type | Description |
|---|---|---|
| `plugin_gaindemo.c` | effet audio | atténuation de moitié (exemple minimal) |
| `plugin_spectrum.c` | visuel | spectre : FFT 1024, 48 barres log, palette bleu→rouge, halo, grille |
| `plugin_3dspectrum.c` | visuel | spectre 3D : cylindre de barres rotatif (projection perspective) |
| `plugin_vumeter.c` | visuel | VU mètre stéréo à LED (-45..0 dB, pics, clip) — style Winamp/XMMS |
| `plugin_fireworks.c` | visuel | feu d'artifice déclenché par les beats de la musique |
| `plugin_fractal.c` | visuel | plasma fractal per-pixel (256 couleurs, réagit à la musique) |
| `plugin_hypnotic.c` | visuel | tunnel hypnotique : anneaux rotatifs pulsés par la musique |

Compiler avec `-lgdi32 -lm` pour les plugins visuels.

> **Plusieurs plugins visuels** : un seul est affiché à la fois — le menu
> **Plugins ▸ Visual** fonctionne en radio (cochez celui que vous voulez).

## Compiler un plugin

Avec MinGW (Linux) :

```bash
x86_64-w64-mingw32-gcc -O2 -shared -o mon_plugin.dll mon_plugin.c \
    -Isrc -static-libgcc          # + -lgdi32 -lm pour un visuel
```

## Journal

Les chargements/erreurs de plugins sont écrits dans `musicplayer.log`
(à côté de l'exe) — utile pour déboguer une DLL qui ne se charge pas.
