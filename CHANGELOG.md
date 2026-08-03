# Changelog

All notable changes to MusicPlayer are documented in this file.

## [2026.08.027-c2] — 2026-08-03

### Fixed
- **Menu qui ne revenait plus** : après le skin Vintage radio (menu caché), revenir à l'interface par défaut laissait la fenêtre sans barre de menus — la barre était créée dans une variable locale au démarrage, le rappel du menu ne pouvait pas la retrouver

## [2026.08.027-c1] — 2026-08-03

### Fixed
- **Crash corrigé** : quand la barre de menus est cachée (skin Vintage radio), un clic dans le menu contextuel (vitesse, plugins…) provoquait une erreur — toutes les manipulations de menu passent désormais par la barre réelle même cachée

## [2026.08.027] — 2026-08-03

### Added
- **Disposition personnalisable par skin** : chaque skin peut **cacher la barre de menus** (menu accessible par **clic droit**) et **déplacer les boutons de contrôle** (en haut, à la place du menu, ou en bas) — l'image de fond couvre **toute la fenêtre**
- **Skin Vintage radio** : la barre de menus est remplacée par les boutons de contrôle en haut ; le menu reste accessible par clic droit

## [2026.08.026-c1] — 2026-08-03

### Fixed
- **Zip corrigé** : le dossier `skins/` (DLL + textures) est maintenant inclus dans l'archive de distribution
- **Skin Vintage radio** : texture photoréaliste générée par IA (ComfyUI) — vraie radio à cadran 1950s, le visualiseur se joue dans la grille du haut-parleur

## [2026.08.026] — 2026-08-03

### Changed
- **Skins dans leur propre dossier `skins/`** (à côté de l'exe) — plus mélangés aux plugins dans `plugins/`
- **Sous-menu Skins retiré du menu Plugins** — le skin se choisit uniquement dans Settings ▸ Interface…
- **Skin Vintage radio** : le visualiseur se joue dans le **haut-parleur** de la radio (zone imposée par le skin), texture redessinée (bandeau doré « VINTAGE RADIO », cadran AM gradué, grille du haut-parleur)

## [2026.08.025] — 2026-08-03

### Added
- **Skins complets** : un skin personnalise maintenant toute l'interface :
  - **Image de fond** de la fenêtre principale (étirée), affichée sous le visualiseur
  - **Barre de menus** dessinée avec la palette du skin (fond, texte, survol) — y compris le fond de la barre
  - **Fenêtres de configuration** (Web server, Plugins, Interface, Update, About) aux couleurs du skin (fond + textes)
- **Skin Vintage radio** : texture complète d'une radio à lampes (bois, bandeau doré « VINTAGE RADIO », cadran AM gradué, haut-parleur) affichée derrière le visualiseur
- **Skin Winamp** : texture façon Winamp classique (barre de titre, zone playlist, égaliseur)

## [2026.08.024] — 2026-08-03

### Added
- **Settings ▸ Interface…** : fenêtre pour choisir le **skin** (liste déroulante, palette par défaut comprise) et la **langue** — le sous-menu Language est retiré du menu Settings
- **Settings ▸ Update…** : fenêtre pour configurer le **mode de mise à jour** (automatique au démarrage / manuel / désactivé) et **vérifier maintenant** — « Check for updates » et « Check for updates at startup » sont retirés du menu Settings

## [2026.08.023-c1] — 2026-08-03

### Fixed
- **Skins : plus de désactivation** — choisir un skin dans Plugins ▸ Skins ne retire plus les autres skins de la liste : les 11 restent toujours affichés, la sélection (radio) ne change que le skin actif
- **Dialog Plugins (Settings ▸ Plugins…)** : il définit désormais les plugins **affichés dans le menu Plugins** (libellé corrigé : « Plugins to show in the Plugins menu ») ; **les skins n'y figurent plus** — ils se choisissent uniquement dans Plugins ▸ Skins
- **Menu Plugins** : sélectionner/désélectionner un plugin (case ou radio) ne l'enlève plus de la liste
- Un seul skin est appliqué au démarrage (le premier actif de la sélection radio)

## [2026.08.023] — 2026-08-03

### Added
- **Télécommande web enrichie** :
  - **Jaquette de la musique en cours** affichée sur la page (endpoint `/cover` — jaquette intégrée au MP3 ou cover.jpg/folder.jpg à côté du morceau)
  - **Métadonnées** dans l'état : titre, artiste, album de la musique en cours d'écoute
  - **Titres des morceaux** (balises ID3) dans la playlist web au lieu des noms de fichiers

### Changed
- **Skins exclusifs** : un seul skin actif à la fois (sélection radio, comme les visuels) — re-clic sur le skin actif = retour à la palette par défaut

## [2026.08.022] — 2026-08-03

### Added
- **Skins** : 11 skins proposés dans Plugins ▸ Skins — rétro 60s, 70s, 80s (néon), 90s, années 2000, radio vintage (bois & or), Winamp, épuré, kitsch, cartoon, noir & blanc. Chaque skin applique une palette de couleurs complète (fond, texte, boutons, volume, progression) instantanément
- **Fenêtre Playlist** : bouton playlist (touche L) — liste des morceaux avec le morceau courant en surbrillance ; double-clic ou Entrée pour jouer la musique choisie
- **Plugin Jaquette** (Cover art) : affiche l'image de la chanson en cours — jaquette intégrée au MP3 (frame APIC) ou cover.jpg / folder.jpg / cover.png / front.jpg placé à côté du morceau

## [2026.08.021] — 2026-08-03

### Added
- **Settings ▸ Plugins…** : fenêtre listant tous les plugins avec cases à cocher pour les activer/désactiver (état persisté dans `plugins.ini`)
- **Plugin Lyrics** : affiche les paroles de la chanson (fichier `.lrc` placé à côté du morceau) — clic dans Plugins ▸ Services
- **Barre de progression cliquable** : cliquer dessus pour aller directement à un moment de la musique
- **Icône de la fenêtre et de la barre des tâches** (WM_SETICON)
- **À propos : lien GitHub cliquable** (bouton « Open GitHub »)

### Changed
- **Mise à jour automatique** : fenêtre d'avertissement à 3 choix avant l'installation — « Mettre à jour maintenant » (télécharge le zip, ferme et relance automatiquement l'application), « Plus tard », « Ignorer cette version » (seules les versions suivantes seront proposées)

## [2026.08.020] — 2026-08-03

### Changed
- **Le serveur web est maintenant un plugin** (Web Server, type Service) — visible dans Plugins ▸ Services, désactivable dans Settings ▸ Plugins
- **Booster de volume** : le plugin d'effet audio s'appelle désormais « Volume booster » (+25 % avec écrêtage)

### Added
- **Plugin Métadonnées MP3** (type Service) : lit les balises ID3 (titre) des fichiers MP3 — l'interface affiche le titre au lieu du nom de fichier quand il est disponible
- **Settings ▸ Plugins** : active/désactive chaque plugin (état persisté dans `plugins.ini`) ; un plugin désactivé n'apparaît plus dans le menu Plugins

## [2026.08.019] — 2026-08-03

### Added
- **Fichier de configuration `config.yml`** (`%APPDATA%\MusicPlayer\config.yml`) : l'état du lecteur est sauvegardé à la fermeture et restauré au démarrage
  - Volume, vitesse, mode aléatoire
  - Dernier chemin ouvert (fichier ou dossier) et fichier en cours de lecture
  - Configuration du serveur web (activé, port, sortie audio, IP écoutées)
- **Rescan automatique de la playlist au démarrage** : les nouveaux fichiers (dossier et sous-dossiers) sont ajoutés, ceux qui n'existent plus sont retirés, et la lecture reprend sur le morceau en cours
- Migration automatique de l'ancien fichier `web.txt`

## [2026.08.018-c5] — 2026-08-03

### Added
- **Fenêtre Web server : liste des interfaces réseau avec cases à cocher** — chaque adresse IP (avec le nom de l'interface) peut être activée ou désactivée pour l'écoute ; le serveur n'écoute que sur les IP cochées (par défaut : toutes)

## [2026.08.018-c4] — 2026-08-03

### Added
- **Télécommande web : télécommande fixe (sticky)** — les boutons et le titre restent visibles en haut de l'écran quand on fait défiler la playlist
- **Clic sur un morceau** de la playlist web pour y passer directement
- **Mode aléatoire (shuffle)** : bouton 🔀 dans l'application (barre de contrôles, orange quand actif) et sur la télécommande web — le morceau suivant est choisi au hasard

## [2026.08.018-c3] — 2026-08-03

### Added
- **Télécommande web : bouton de sortie audio clair** — un bouton (icône + libellé) indique le mode son courant : 🖥️ **PC** / 📱 **Phone** / 🔀 **Both** ; un clic change le mode (cyclique) directement depuis le téléphone, la configuration est sauvegardée
- Le bouton lecture de la page lance/arrête aussi le son sur le téléphone (modes Phone/Both) ; stop coupe le son partout

## [2026.08.018-c2] — 2026-08-03

### Fixed
- **Page web : icônes de la télécommande remplacées par des SVG inline** — le bouton lecture (▶/⏸) et les autres s'affichaient mal sur certains navigateurs/téléphones (caractères Unicode ⏸ et emojis mal rendus)
- **Bouton « Son téléphone » dédié** : la lecture du son sur le téléphone est maintenant indépendante du bouton lecture — en mode « les deux », le bouton lecture contrôle l'application, et un bouton haut-parleur (violet) lance/arrête le son sur le téléphone

## [2026.08.018-c1] — 2026-08-03

### Fixed
- **Settings ▸ Web server… ne faisait rien** : l'identifiant du template de dialogue dans la ressource était symbolique (`IDD_WEB`) au lieu du numéro attendu (104) — le dialogue ne pouvait pas être chargé. Corrigé (ID numérique 104).

## [2026.08.018] — 2026-08-03

### Added
- **Serveur web de contrôle à distance** (Settings ▸ Web server…) — la page web est accessible depuis le téléphone ou la tablette sur le même réseau
  - **Port libre détecté automatiquement à partir de 8000**, modifiable par l'utilisateur
  - **Télécommande** : lecture/pause, stop, suivant, volume +/-, vitesse +/-
  - **Playlist affichée** avec le morceau en cours mis en évidence (raffraîchie chaque seconde)
  - **Sortie audio** : cet ordinateur, téléphone seul, ou les deux en simultané — le téléphone reçoit le son via un flux audio WAV diffusé par le serveur (/stream)

## [2026.08.017-c2] — 2026-08-02

### Fixed
- **File ▸ Open folder… : vrai sélecteur de dossier** — retour au dialogue classique `SHBrowseForFolderW` (avec COM initialisé) : on choisit un **dossier**, plus un fichier
- **Vérification de mises à jour : les corrections `-cX` sont détectées** — le comparateur de versions gère le suffixe de correction (ex. `2026.08.017` → `2026.08.017-c1` est signalé comme mise à jour ; `-c2` > `-c1`)

## [2026.08.017-c1] — 2026-08-02

### Fixed
- **File ▸ Open folder… : l'application ne répondait plus / se fermait** après le dialogue de sélection (dialogues shell instables : `IFileOpenDialog` se fige, `SHBrowseForFolderW` détruit la fenêtre) — remplacé par le **dialogue d'ouverture classique** `GetOpenFileNameW` (le même que File ▸ Open…, le plus fiable) : choisissez n'importe quel MP3/MP4 du dossier → tout le dossier est lu (sous-dossiers inclus)
- Titre du dialogue raccourci

## [2026.08.017] — 2026-08-02

### Fixed
- **Menu File ▸ Open folder… affichait la clé « menu_open_folder »** : les nouvelles clés de l'anglais embarqué (`menu_open_folder`, `err_folder`, mises à jour…) manquaient dans le tableau par défaut
- **Plantage au clic sur Open folder…** : le dialogue de sélection de dossier passe de `SHBrowseForFolderW` (instable sans COM initialisé) au dialogue moderne **IFileOpenDialog** (FOS_PICKFOLDERS, COM initialisé) — plus stable sur Windows 11

## [2026.08.016] — 2026-08-02

### Added
- **Lecture d'un dossier (playlist)** : File ▸ Open folder…, glisser-déposer d'un dossier ou en ligne de commande — scan récursif des MP3/MP4 (sous-dossiers inclus), tri par nom, lecture automatique du morceau suivant, arrêt en fin de playlist ; compteur « [n/total] » dans la barre d'état
- **Bouton ⏭ Suivant** dans la barre de contrôles (raccourci N)

### Fixed
- chemins de la playlist tronqués : `%ls` obligatoire pour `wchar_t*` dans `swprintf` (MinGW) — le scan scannait le mauvais dossier

## [2026.08.015] — 2026-08-02

### Added
- **Vérification de mises à jour** (GitHub Releases) :
  - manuelle : **Settings ▸ Check for updates…** (affiche le résultat même si à jour)
  - automatique : **Settings ▸ Check for updates at startup** (4 s après le lancement, silencieuse si à jour), préférence persistée dans `%APPDATA%\MusicPlayer\upd.txt`
  - comparaison de la version locale avec la dernière release ; si une nouvelle version existe : boîte de dialogue avec ouverture de la page des releases (bouton Yes)
  - requête en arrière-plan (thread WinINet, timeout 10 s) : l'interface ne bloque jamais

## [2026.08.014] — 2026-08-02

### Changed
- **3D Isometric : caméra frontale basse** (conforme aux références) — fini le « carré séparé en deux » des axes diagonaux : les rangées s'élèvent en rétrécissant vers le centre (point de fuite central), la grille au sol est un trapèze, les barres du fond sont plus petites, plus hautes et plus sombres (2 jeux de dégradés), sommet lumineux sur chaque barre
- Gain d'analyse augmenté (18) et lissage adouci (0.95) pour un paysage plus dense avec la musique

## [2026.08.013] — 2026-08-02

### Fixed
- **3D Isometric : bug critique d'affichage** — la hauteur projetée `gh` devenait négative avec l'axe du temps inversé (`by < 0`), ce qui donnait une échelle de grille négative : barres placées hors écran / scène inversée. Corrigé avec la valeur absolue.
- Analyse audio stabilisée : **tampon continu de 0,74 s** (au lieu d'un bloc unique) + **détection de silence** (les zéros ne vident plus le paysage, retombée très lente) — le paysage se remplit désormais régulièrement, y compris sous Wine

## [2026.08.012] — 2026-08-02

### Changed
- **3D Isometric : vraie vue de côté isométrique** — les axes partent en diagonale (fréquences → bas-droite, temps → haut-gauche), chaque barre a 3 faces (avant dégradée, latérale sombre, dessus lumineux), grille centrée par calcul de boîte englobante
- **Plus de scintillement** : rendu en double buffer (dessin hors écran + un seul BitBlt) dans toute la zone centrale

## [2026.08.011] — 2026-08-02

### Changed
- **3D Isometric entièrement refait, style GLBars / WM3DSpectrum** :
  - grille **rectangulaire en perspective** (les rangées du fond sont plus petites et plus hautes) — fini le losange isométrique
  - **dégradé vertical sur chaque barre** : sombre à la base, vive et lumineuse au sommet (6 segments, éclat blanc)
  - **grille centrée** sur l'écran (horizontal et vertical), 24 colonnes × 14 rangées

## [2026.08.010] — 2026-08-02

### Changed
- **3D Isometric** : grille désormais **carrée 24×24** (au lieu de 30×16) — comme les références WM3DSpectrum / spectrogrammes 3D

## [2026.08.009] — 2026-08-02

### Fixed
- **3D Isometric** : grille et bâtons maintenant centrés sur l'écran (le centre logique de la grille était confondu avec l'origine de la projection — décalage à droite et en bas), centrage vertical incluant la hauteur des barres

## [2026.08.008] — 2026-08-02

### Added
- **Plugin visuel « 3D Isometric »** : paysage de bâtons 3D isométrique (style WM3DSpectrum / spectrogramme 3D) — grille rectangulaire fréquences × temps, 30 colonnes arc-en-ciel, 16 rangées d'historique qui défilent, faces 3D (avant clair, côté sombre, sommet en losange), fond bleu nuit
- Fenêtre agrandie à 640×300 (plus de place pour les paysages 3D), taille minimale 420×260

## [2026.08.007] — 2026-08-02

### Changed
- **3D Spectrum aligné sur le rendu Spectrum3D de référence** : fond bleu nuit très sombre, 96 barres fines, dégradé arc-en-ciel par position (bleu → cyan → vert → jaune → orange → rouge), arrière assombri — grille wireframe retirée

### Fixed
- **Menu Paramètres** : le sous-menu des langues n'apparaît plus en double (position de remplacement corrigée)
- **Redimensionnement de la fenêtre** : le fond est repeint en blanc avant chaque rendu (régression du `WM_ERASEBKGND`), taille minimale 420×220 ajoutée

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
