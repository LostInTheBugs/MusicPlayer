# SPEC — Plugin de transcription offline via whisper.cpp

**Branche** : `pre-release`
**Version cible** : `2026.08.046` (nouveau cycle)
**Plugin** : `transcribe_whisper` (SERVICE, port 8083)
**Fichier source** : `examples/plugin_transcribe_whisper.c`

---

## 0. Résumé

Plugin SERVICE qui lance `whisper.cpp` (whisper-cli.exe) en sous-processus pour
transcrire un fichier audio. Le plugin expose un serveur HTTP (port 8083, pattern
identique à podcasts/8082 et restapi/8080) et communique avec whisper-cli.exe via
un pipe stdout. Les transcripts sont stockés en JSON dans
`%APPDATA%\MusicPlayer\transcripts\`.

Le client MusicPlayer enverra les requêtes de transcription (fichier local ou URL
de podcast déjà téléchargé). Seul le plugin moteur est livré ici — le client
(menus/dialogs) sera fait dans une spec séparée.

---

## 1. Architecture

```
plugin_transcribe_whisper.dll  (core_plugins/, SERVICE)
│
├── HTTP server port 8083
│   ├── GET  /models                    → liste les modèles .ggml disponibles
│   ├── POST /transcribe                → lance la transcription
│   │       body: {"path":"C:\\...mp3","lang":"fr","model":"medium"}
│   ├── GET  /transcripts               → liste des transcripts sauvegardés
│   ├── GET  /transcript?file=<urlenc>  → contenu d'un transcript sauvegardé
│   └── GET  /progress                  → progression de la tâche en cours
│
├── Sous-processus : whisper-cli.exe
│   Arguments : -m <model> -f <wav> -l <lang> -oj -of <outdir>/<name>
│   Capture stdout + stderr
│
└── Stockage : %APPDATA%\MusicPlayer\transcripts\<hash>.json
    + %APPDATA%\MusicPlayer\whisper-models\  (modèles .ggml)
```

---

## 2. Étapes (dans l'ordre)

### Étape 1 — Squelette du plugin

Créer `examples/plugin_transcribe_whisper.c` sur le modèle de `plugin_restapi.c` :

- Inclure `<winsock2.h>`, `<windows.h>`, `<stdio.h>`, `<stdlib.h>`, `<string.h>`
- Inclure `../src/plugin.h` et `http_util.h` (depuis `examples/`)
- Définir `#define TRANSCRIBE_PORT 8083`
- Implémenter les 5 fonctions d'identification obligatoires :
  - `name()` → `"Transcribe (Whisper)"`
  - `version()` → `MP_BUILD_VERSION` (fallback `"1.0"`)
  - `description()` → `"Offline speech-to-text transcription via whisper.cpp"`
  - `type()` → `MP_PLUGIN_SERVICE`
  - `init()` → démarre le serveur HTTP (WSAStartup, socket, bind, listen, thread)
  - `destroy()` → arrête le serveur
- Globaux : `g_h`, `g_running`, `g_listen`, `g_thread` (mêmes noms/types que restapi)

**Vérification** : le plugin compile et se charge sans erreur. Le serveur répond
sur `http://127.0.0.1:8083/health` avec `{"ok":1}`.

### Étape 2 — Démarrer/arrêter whisper-cli.exe

Fonction `whisper_run(model_path, wav_path, lang, out_dir, out_name)` :

```c
// Lance whisper-cli.exe et capture sa sortie stdout.
// Retourne un HANDLE vers le processus, et un HANDLE vers le pipe stdout
// (le thread appelant lira le pipe jusqu'à EOF).
```

1. **Localiser whisper-cli.exe** : chercher dans l'ordre :
   - `%APPDATA%\MusicPlayer\whisper\whisper-cli.exe`
   - À côté du plugin : `core_plugins\whisper-cli.exe`
   - À côté de l'exe : `..\whisper-cli.exe`

2. **Créer le pipe stdout** : `CreatePipe` avec `SECURITY_ATTRIBUTES.bInheritHandle = TRUE`

3. **Construire la ligne de commande** (attention aux chemins avec espaces) :
   ```
   "<whisper_exe>" -m "<model>" -f "<wav>" -l <lang> -oj -of "<out_dir>/<out_name>"
   ```
   Format : `-oj` = sortie JSON. `-l auto` pour auto-détection.

4. **CreateProcessW** avec `CREATE_NO_WINDOW`, pipe stdout redirigé, pipe stderr partagé.

5. **Lire le pipe stdout** dans un thread dédié, accumuler dans un buffer `g_result_json`.

**Vérification** : créer un fichier WAV test (16 kHz mono 16-bit), lancer whisper-cli.exe
manuellement, vérifier que le JSON de sortie est capturé.

### Étape 3 — Extraction audio via FFmpeg

Fonction `extract_audio(input_path, wav_path)` :

1. **Localiser ffmpeg.exe** : même logique que whisper — `%APPDATA%\MusicPlayer\ffmpeg\ffmpeg.exe`,
   à côté de l'exe, etc.

2. **Ligne de commande** — convertir en WAV 16 kHz mono PCM 16-bit :
   ```
   "<ffmpeg>" -y -i "<input>" -ar 16000 -ac 1 -c:a pcm_s16le "<output.wav>"
   ```

3. **CreateProcessW** avec `CREATE_NO_WINDOW`, attendre la fin (`WaitForSingleObject`, timeout 300s).

4. Retourner 0 si OK, -1 si échec.

**Vérification** : extraire l'audio d'un MP3, vérifier que le WAV est valide
(header RIFF + taille > 44 octets).

### Étape 4 — Stockage des transcripts

Fonctions de persistance (fichiers dans `%APPDATA%\MusicPlayer\transcripts\`) :

1. **`store_dir()`** — obtenir le dossier, le créer si absent :
   ```c
   wchar_t dir[MAX_PATH];
   // SHGetFolderPathW(CSIDL_APPDATA) + L"\\MusicPlayer\\transcripts"
   // CreateDirectoryW récursif si besoin
   ```

2. **`transcript_save(file_path, json_result)`** :
   - Hash MD5 ou SHA1 du `file_path` pour le nom de fichier : `<hash>.json`
   - Écrire le JSON complet (le résultat de whisper-cli + métadonnées : date, langue, modèle)

3. **`transcript_load(file_path)`** : lire `<hash>.json`, retourner le contenu.

4. **`transcripts_list()`** : lister les `.json` du dossier, retourner un tableau
   avec `file`, `date`, `lang`, `model`, `duration` extraits de chaque JSON.

5. **Index** : `transcripts_index.json` — mappe `file_path` → `hash`, pour retrouver
   un transcript par chemin de fichier source.

**Format d'un transcript sauvegardé** :
```json
{
  "source": "C:\\Users\\...\\podcast.mp3",
  "date": "2026-08-07T14:30:00Z",
  "lang": "fr",
  "model": "ggml-medium.bin",
  "duration_s": 1250.5,
  "segments": [
    {"start": 0.0, "end": 5.2, "text": "Bonjour à tous"},
    {"start": 5.2, "end": 12.1, "text": "Aujourd'hui on va parler de..."}
  ],
  "full_text": "Bonjour à tous. Aujourd'hui on va parler de..."
}
```

**Vérification** : sauvegarder un transcript factice, le recharger, lister les transcripts.

### Étape 5 — Endpoint POST /transcribe

Route la plus complexe. Logique :

1. **Parser le body JSON** entrant : `path` (obligatoire), `lang` (défaut `"auto"`),
   `model` (défaut `"medium"`).

2. **Vérifier** que le fichier existe (`GetFileAttributesW`).

3. **Générer un nom de sortie** unique basé sur le hash du chemin + timestamp :
   `wchar_t out_name[64]; swprintf(out_name, 64, L"%08x_%lld", hash, timestamp);`

4. **Dossier temporaire** : `%TEMP%\musicplayer_transcribe\<out_name>\`
   → `CreateDirectoryW` récursif.

5. **Extraire l'audio** :
   ```
   ffmpeg -y -i "<path>" -ar 16000 -ac 1 -c:a pcm_s16le "<tmp>\audio.wav"
   ```
   → timeout 300s. Si échec → répondre `{"ok":0,"error":"Audio extraction failed"}`.

6. **Trouver le modèle** :
   - `%APPDATA%\MusicPlayer\whisper-models\ggml-<model>.bin`
   - Si absent → répondre `{"ok":0,"error":"Model not found","model":"ggml-medium.bin"}`
     (le téléchargement de modèle sera fait dans une étape ultérieure).

7. **Lancer whisper-cli.exe** :
   ```
   whisper-cli.exe -m "<model_path>" -f "<tmp>\audio.wav" -l <lang> -oj -of "<tmp>/out"
   ```
   → timeout 600s. Capture stdout (JSON) et stderr (logs).

8. **Parser le JSON de sortie** de whisper : extraire `text` (texte complet) et
   `segments` (tableau `{t0, t1, text}`).

9. **Sauvegarder** le transcript via `transcript_save()`.

10. **Nettoyer** le dossier temporaire (`DeleteFileW` + `RemoveDirectoryW`).

11. **Répondre** au client :
    ```json
    {
      "ok": 1,
      "hash": "a1b2c3d4...",
      "lang": "fr",
      "model": "ggml-medium.bin",
      "duration_s": 1250.5,
      "segments": [...],
      "full_text": "..."
    }
    ```

**Gestion des erreurs** :
- Si une transcription est déjà en cours → `{"ok":0,"error":"Transcription in progress"}`
- Timeout → `{"ok":0,"error":"Transcription timed out"}`
- whisper crash → `{"ok":0,"error":"Whisper process failed","stderr":"..."}`

**Vérification** : transcrire un MP3 de test, vérifier la réponse JSON et le
fichier sauvegardé.

### Étape 6 — Endpoints de listing et consultation

**GET /models** :
- Lister les fichiers `ggml-*.bin` dans `%APPDATA%\MusicPlayer\whisper-models\`
- Réponse :
  ```json
  {
    "models": [
      {"name": "ggml-tiny.bin", "size_mb": 75},
      {"name": "ggml-medium.bin", "size_mb": 1500}
    ],
    "dir": "C:\\Users\\...\\whisper-models"
  }
  ```

**GET /transcripts** :
- Lister tous les transcripts sauvegardés (`transcripts_list()`)
- Réponse : tableau d'objets `{hash, source, date, lang, model, duration_s}`

**GET /transcript?file=<chemin_url_encodé>** :
- Retrouver le transcript par le chemin source
- Réponse : le JSON complet sauvegardé (avec `segments`)

**GET /progress** :
- Si une transcription est en cours, retourner l'état :
  ```json
  {"busy": true, "stage": "extracting", "source": "C:\\...mp3"}
  ```
- Sinon : `{"busy": false}`

### Étape 7 — Ajout au Makefile

Dans la cible `plugins-examples`, ajouter après la ligne `podcasts.dll` :

```makefile
$(CC) -O2 -shared -o bin/plugins/transcribe_whisper.dll examples/plugin_transcribe_whisper.c -Isrc -lws2_32 -static-libgcc -DMP_BUILD_VERSION=\"$(VERSION)\"
```

Et dans la copie vers `core_plugins/` (ligne 179), ajouter `transcribe_whisper.dll` :

```makefile
cp -f bin/plugins/webserver.dll bin/plugins/metadata.dll bin/plugins/cover.dll \
      bin/plugins/upnp.dll bin/plugins/rtp.dll bin/plugins/multiroom.dll bin/plugins/podcasts.dll \
      bin/plugins/transcribe_whisper.dll bin/core_plugins/
```

### Étape 8 — Compilation et test sous Wine

1. **Compiler** : `make && make core && make plugins-examples`
2. **Lancer le core** sous Wine : `cd bin && wine64 ./musicplayer-core.exe`
3. **Vérifier les logs** : `Transcribe: listening on port 8083`
4. **Test santé** : `curl -s http://127.0.0.1:8083/health` → `{"ok":1}`
5. **Test models** : `curl -s http://127.0.0.1:8083/models` → `{"models":[...]}`
6. **Test transcripts vides** : `curl -s http://127.0.0.1:8083/transcripts` → `[]`
7. **Test /progress** : `curl -s http://127.0.0.1:8083/progress` → `{"busy":false}`

---

## 3. Règles et conventions

1. **Pas de modification des fichiers existants** sauf le Makefile (étape 7).
2. **Zéro warning** à la compilation (`-Wall -Wextra`).
3. **Pas de variable globale non initialisée**.
4. **Tous les chemins en `wchar_t`** (Wide API Windows : `CreateProcessW`, `DeleteFileW`…).
5. **Thread-safety** : `g_running` en `volatile LONG`, accès via `InterlockedExchange`.
6. **JSON sans lib externe** : forger les chaînes JSON avec `snprintf` + `json_escape`
   (copier les helpers de `plugin_restapi.c`).
7. **Logs** : chaque étape logue via `g_h->log()` — `Transcribe: extracting audio...`,
   `Transcribe: running whisper...`, `Transcribe: done (45 segments, 3.2s)`.
8. **Langue du code** : commentaires en anglais, noms de fonctions en anglais.
9. **Le plugin est un SERVICE côté moteur** (`core_plugins/`), pas un plugin visuel.
   Le client communique avec lui uniquement via HTTP sur `127.0.0.1:8083`.

---

## 4. Modèle de données whisper-cli (référence)

La sortie JSON de `whisper-cli -oj` a cette forme :

```json
{
  "transcription": [
    {
      "timestamps": {
        "from": "00:00:00,000",
        "to": "00:00:05,200"
      },
      "offsets": {
        "from": 0,
        "to": 5200
      },
      "text": "Bonjour à tous"
    }
  ]
}
```

Conversion vers le format interne :
- `start` = `offsets.from / 1000.0`
- `end` = `offsets.to / 1000.0`
- `text` = `text` (trim)
- `full_text` = concaténation de tous les `text`

---

## 5. Limites et garde-fous

- **Un seul fichier à la fois** — file d'attente implicite : si `/progress` dit
  `busy:true`, le client doit attendre.
- **Taille max du WAV temporaire** : pas de limite explicite, mais un timeout
  d'extraction de 300s borne implicitement la durée du fichier.
- **Taille max du JSON de sortie** : allouer dynamiquement (`malloc`/`realloc`)
  jusqu'à 16 Mo max.
- **Nettoyage** : toujours libérer les dossiers temporaires, même en cas d'erreur
  (bloc `goto cleanup`).

---

## 6. Hors périmètre (NE PAS FAIRE)

- ❌ Interface utilisateur (menus, dialogs) — spec client séparée
- ❌ Téléchargement automatique des modèles — spec séparée
- ❌ Transcription en temps réel (streaming) — v1 = fichier complet uniquement
- ❌ Traduction — plugins séparés à venir
- ❌ Support d'autres moteurs que whisper.cpp (Cloud APIs) — plugin séparé
- ❌ Intégration avec le plugin podcasts (POST automatique depuis podcasts) — plus tard

---

## 7. Vérification finale

```bash
# Tout compiler
make && make core && make plugins-examples

# Vérifier que la DLL existe
ls -la bin/plugins/transcribe_whisper.dll bin/core_plugins/transcribe_whisper.dll

# Lancer le core sous Wine (fond)
wine64 bin/musicplayer-core.exe &
sleep 3

# Tester tous les endpoints
curl -s http://127.0.0.1:8083/health
curl -s http://127.0.0.1:8083/models
curl -s http://127.0.0.1:8083/transcripts
curl -s http://127.0.0.1:8083/progress

# Test de transcription (nécessite whisper-cli.exe + modèle dans le bon dossier)
# curl -s -X POST http://127.0.0.1:8083/transcribe -d '{"path":"C:\\test\\audio.mp3","lang":"fr","model":"medium"}'

# Nettoyer
kill %1
```
