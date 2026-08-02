# Changelog

All notable changes to MusicPlayer are documented in this file.

## [2026.08.002] — 2026-08-02

### Added
- Plugin visuel **Spectrum** : visualiseur de spectre (FFT radix-2 1024 points, 48 barres logarithmiques, palette vert→rouge, pics lumineux, lissage)
- API plugins **v2** : hook `audio_frames()` — flux PCM lecture seule (après les effets) pour les plugins visuels ; les plugins v1 sont rejetés proprement
- Rendu visuel branché : zone centrale remplacée par le plugin (~30 FPS) quand un visuel est actif
- Compilation des plugins d'exemple : `make plugins-examples` (gaindemo + spectrum dans bin/plugins/)
- Archive portable incluant les plugins

### Changed
- Le moteur diffuse chaque bloc audio aux plugins visuels après les effets

## [2026.08.001] — 2026-08-02

### Added
- Lecteur MP3/MP4 pour Windows (Win32 + FFmpeg + miniaudio), compilation croisée MinGW sous Linux
- Menu Ouvrir… (Ctrl+O) + glisser-déposer de fichiers
- Lecture / pause (Espace), stop avec retour à 0 seconde (S)
- Volume 0–100 % (↑/↓), affiché dans la status bar
- Vitesse 0,5× / 1× / 1,5× / 2× (resampling dynamique)
- Status bar : fichier, position/durée, vitesse, volume
- API plugins v1 (skin, effet audio, visuel) + chargeur de DLL avec rechargement à chaud
- Mode `--selftest` : validation du pipeline (lecture, vitesse, pause, stop, fin) sous Wine
- Makefile : `make`, `make test`, `make zip`
