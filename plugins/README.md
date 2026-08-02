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

- **plugin_gaindemo.c** — effet audio minimal (atténuation de moitié)
- **plugin_spectrum.c** — visualiseur de spectre complet : FFT radix-2
  (1024 points), fenêtre de Hann, 48 barres logarithmiques, palette
  vert→rouge, pics lumineux, lissage. Compiler avec `-lgdi32 -lm`.

## Compiler un plugin

Avec MinGW (Linux) :

```bash
x86_64-w64-mingw32-gcc -O2 -shared -o mon_plugin.dll mon_plugin.c \
    -Isrc -static-libgcc          # + -lgdi32 -lm pour un visuel
```

## Journal

Les chargements/erreurs de plugins sont écrits dans `musicplayer.log`
(à côté de l'exe) — utile pour déboguer une DLL qui ne se charge pas.
