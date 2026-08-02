# Changelog

All notable changes to MusicPlayer are documented in this file.

## [2026.08.006] — 2026-08-02

### Changed
- **3D Spectrum relooké style Spectrum3D** (spectrum3d.sourceforge.net) : fond noir, grille wireframe au sol, 72 barres fines (blanc→jaune→rouge), arrière assombri
- **Menus simplifiés** : File · Settings · Plugins · Help — menus Playback et Volume supprimés
- **Settings** regroupe : Speed, Fullscreen, Language
- **Raccourcis clavier retirés des libellés de menus** (plus de « Ctrl+O » affichés)
- **Volume 0–200 %** : curseur avec zone bleue (0–100 %) et orange (booster 100–200 %), marque à 100 %

### Fixed
- **Redimensionnement de la fenêtre** : parts de la status bar recalculées, zone entière redessinée (visuel, progression, contrôles)

## [2026.08.005] — 2026-08-02

### Added
- **Boutons de contrôle** dans la fenêtre : lecture/pause (bleu) et stop (rouge) — plus seulement dans le menu
- **Curseur de volume** : piste + poignée cliquable/glissable, à côté des boutons
- **Plein écran** pour les effets visuels : bouton ⛶, touche F11, Échap pour sortir
- **Menu Plugins organisé par type** : sous-menus Visual / Audio effects / Skins — un seul visuel actif à la fois (radio)
- **Icône de l'application** (play violet sur fond arrondi, 5 tailles) — plus l'icône Windows par défaut
- Plugins visuels : **3D Spectrum** (cylindre de barres rotatif, projection 3D), **Fractal** (plasma per-pixel, 256 couleurs, réagit à la musique), **Hypnotic** (tunnel d'anneaux rotatifs pulsés par la musique)
- Fenêtre agrandie (640×240) pour accueillir la barre de contrôles

### Changed
- Boutons et slider dessinés en GDI (aucune dépendance), le rendu visuel occupe la zone au-dessus de la progression

## [2026.08.004] — 2026-08-02

### Added
- **Multilingue** : fichiers texte `lang/<code>.lang` (UTF-8, `cle=valeur`, commentaires `#`) — n'importe qui peut ajouter une langue sans recompiler
- Menu **Language / Langue** : bascule instantanée, préférence mémorisée dans `%APPDATA%\MusicPlayer\lang.txt`
- **Anglais par défaut** (embarqué dans le binaire), détection automatique de la langue de Windows, français fourni (`fr.lang`)
- Fichiers fournis : `lang/en.lang`, `lang/fr.lang`, guide `lang/README.md`

### Changed
- Tous les textes de l'interface passent par le moteur de traduction (menus, status bar, dialogues, messages, états)
- Messages du selftest et des journaux en anglais

## [2026.08.003] — 2026-08-02

### Added
- **Barre de progression** : dégradé bleu→jaune, toujours visible (mode texte et plugins)
- Plugin visuel **VUMeter** : VU mètre stéréo à LED (24 LED/canal, -45..0 dB, pics à décroissance lente, indicateur de clip) — style Winamp/XMMS
- Plugin visuel **Fireworks** : feu d'artifice synchronisé sur la musique (détection de beats par énergie adaptative, gravité, traînées, 360 couleurs)
- Le loader n'affiche plus qu'**un seul plugin visuel** à la fois (choix via le menu Plugins)

### Changed
- Plugin **Spectrum** relooké : palette arc-en-ciel 256 teintes (bleu→rouge), fond en dégradé bleu nuit, grille discrète, effet de halo (glow) autour des barres

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
