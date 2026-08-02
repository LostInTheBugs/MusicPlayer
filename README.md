# MusicPlayer

Lecteur audio **MP3 / MP4** pour Windows 11, écrit en C (Win32 API),
compilé sous Linux par compilation croisée (MinGW-w64).

- Décodage : **FFmpeg** (libavformat / libavcodec / libswresample)
- Sortie audio : **miniaudio** (WASAPI / DirectSound / WinMM)
- Vitesse de 0,5× à 2× par pas de 0,5 · volume 0–100 % · stop = retour à 0 s
- Glisser-déposer de fichiers, raccourcis clavier
- **Barre de progression** avec dégradé de couleurs
- **Multilingue** : fichiers texte `lang/*.lang` (anglais par défaut, français fourni, menu Language pour basculer — voir [lang/README.md](lang/README.md))
- **Architecture de plugins** (skins, effets audio, visuels) — API version 2 :
  spectre coloré, VU mètre à LED, feu d'artifice synchronisé sur la musique

## Version

`2026.08.004` — voir [CHANGELOG.md](CHANGELOG.md)

## Lancer sous Windows 11

1. Copier le dossier `bin/` (ou dézipper `dist/MusicPlayer-2026.08.001-win64.zip`)
   sur la machine Windows.
2. Lancer `MusicPlayer.exe`. Aucune installation requise
   (les DLL FFmpeg sont dans le même dossier).

> Le dossier `plugins/` doit être créé à côté de `MusicPlayer.exe` :
> il est créé automatiquement au premier lancement s'il manque.

## Développement sous Linux (ce dépôt)

Prérequis Ubuntu :

```bash
sudo apt install gcc-mingw-w64-x86-64 wine64 ffmpeg zip
```

Compiler et tester :

```bash
make          # → bin/MusicPlayer.exe (+ DLLs FFmpeg)
make test     # compile + génère des fichiers de test et les joue sous Wine
make zip      # archive portable pour Windows
```

## Utilisation

| Action | Menu | Raccourci |
|---|---|---|
| Ouvrir un MP3/MP4 | Fichier ▸ Ouvrir… | Ctrl+O (ou glisser-déposer) |
| Lecture / Pause | Lecture ▸ Lecture / Pause | Espace |
| Stop (retour à 0 s) | Lecture ▸ Stop | S |
| Vitesse 0,5× / 1× / 1,5× / 2× | Lecture ▸ Vitesse | — |
| Volume | Volume ▸ Monter / Descendre | ↑ / ↓ |

## Plugins

Les plugins sont des DLL chargées depuis `plugins/` (à côté de l'exe).
Trois types sont prévus par l'API v1 :

- **Skin** (`MP_PLUGIN_SKIN`) — personnalise la fenêtre
- **Effet audio** (`MP_PLUGIN_AUDIO_EFFECT`) — traitement PCM temps réel
- **Visuel** (`MP_PLUGIN_VISUAL`) — rendu dans la zone d'affichage

Voir [plugins/README.md](plugins/README.md) et `src/plugin.h` pour l'API.
Menu **Plugins ▸ Recharger** pour charger/décharger sans redémarrer.

## Structure du projet

```
MusicPlayer/
├── Makefile               # build croisé Linux → Windows
├── src/
│   ├── main.c             # interface Win32 (menus, status bar, D&D)
│   ├── player.c/.h        # moteur : FFmpeg + miniaudio + ring buffer
│   ├── plugin.h           # API plugins (v1)
│   └── plugin_loader.c/.h # scan/chargement des plugins
├── plugins/               # plugins à déposer (voir README)
├── vendor/                # miniaudio.h + FFmpeg win64 (BtbN)
├── bin/                   # exe + DLLs (ce qui part sur Windows)
├── test/                  # fichiers de test générés
└── dist/                  # archives portables
```

## Licence

MIT — voir [LICENSE](LICENSE).
