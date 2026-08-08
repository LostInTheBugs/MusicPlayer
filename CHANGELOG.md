# Changelog

All notable changes to MusicPlayer are documented in this file.

## [2026.08.046-c20] — 2026-08-08

### Changed — transcription layout + window height
- The transcript is now **paginated into paragraphs**: the whisper plugin
  keeps the segment list and returns it in `/progress`
  (`{"busy":false,"full_text":"…","segments":[…]}`); the client builds one
  paragraph per segment (fallback to the raw text with older plugins).
- The main window is created **twice as tall** (640×600 instead of 640×300)
  so several transcript lines are visible by default.

## [2026.08.046-c19] — 2026-08-08

### Fixed — Podcasts ▸ Playlist button
- The Playlist button is narrowed (60 px) so it no longer overlaps
  Read/Unread.
- The Playlist window is rebuilt systematically after loading the podcast
  episodes (also when it was already open).
- The podcasts plugin now emits valid JSON for `GET /podcasts/episodes`
  (a stray trailing quote broke the payload).

## [2026.08.046-c18] — 2026-08-08

### Changed — Podcasts dialog
- The **Play** button is now **Playlist**: it loads the podcast episodes into
  the playlist, closes the Podcasts window and opens the Playlist window —
  playback starts when the user presses play (engine command `playlist` gains
  an optional `play:0`).
- The **Download** button now asks **where to save** the episode
  (`GetSaveFileNameW`); the plugin accepts a `dest` field in
  `POST /download` (falls back to AppData without it).

### Added — transcription progress bar
- The whisper plugin reports a `progress` percentage in `/progress` (total
  duration parsed from the ffmpeg extraction output, current position parsed
  from whisper-cli's `[a --> b]` lines); the Now playing panel draws a
  progress bar with the percentage under the status label.

## [2026.08.046-c17] — 2026-08-08

### Fixed — transcription result never reached the panel
- The client's `/progress` poll discarded the transcription result: when the
  task finished it fetched the feed's embedded transcript instead of the
  whisper full text, so the panel stayed on « Transcribing… » forever after
  the 15 s POST timeout cut the initial request.
- The plugin now keeps the last successful `full_text` and returns it in the
  `/progress` response (`{"busy":false,"full_text":"…"}`); the client reads
  it and displays the text (falls back to the feed transcript or an error
  when it is empty/absent).

### Fixed — launcher left partial files on failed downloads
- `http_download` now deletes the destination file when the download fails
  (a leftover error/partial file would be detected by the size check but
  could not be executed, e.g. a 14-byte `ffmpeg.exe`).

## [2026.08.046-c16] — 2026-08-08

### Fixed — Now playing transcription panel labels
- During extraction the panel now shows « Extracting audio… » (new key
  `now_extracting`), and during the whisper run it shows « Transcribing… »
  alone — the redundant « Transcribing… transcribing » stage text is gone
  (the plugin stage value is only appended for unknown stages).

## [2026.08.046-c15] — 2026-08-08

### Added — default whisper model selection (Plugins ▸ Whisper models…)
- New **Default** button: mark the selected model as the default for
  transcription (stored in `%APPDATA%\MusicPlayer\whisper.cfg`); the
  transcription request now sends the chosen model, the list shows it as
  « default ».
- The Plugins menu label has a hard-coded fallback (no raw key with an
  outdated language file).

### Fixed — FFmpeg runtime download resilience (launcher)
- Runtime files are checked for a **minimum size** (a truncated file from an
  interrupted download is now detected and re-downloaded automatically —
  fixes the « avdevice-63.dll 0xc000012f » corrupt-file error).
- The manual-fix error message points to the **releases page** (FULL zip of
  the current channel) instead of `releases/latest`, which led pre-release
  users to the stable version.
- The fallback individual download tries **all** runtime files before giving
  up (a transient failure on one file no longer aborts the rest).

## [2026.08.046-c14] — 2026-08-08

### Added — Whisper models manager (menu Plugins ▸ Whisper models…)
- Lists the official whisper.cpp models (tiny / base / small / medium /
  large-v3 / large-v3-turbo) with size and install state, plus any model
  already present in `%APPDATA%\MusicPlayer\whisper-models\`.
- **Download** a selected model directly from Hugging Face (with progress,
  downloaded to a `.part` file and renamed on success — no partial model is
  ever used).
- **Add…** a local model file (copied into the models folder).
- All labels have hard-coded fallbacks (no raw keys with an outdated
  language file).

## [2026.08.046-c13] — 2026-08-08

### Changed — whisper model handling (transcription)
- If the requested model (`ggml-medium.bin` by default) is missing, the
  plugin now **falls back to any model present** in
  `%APPDATA%\MusicPlayer\whisper-models\` (e.g. `ggml-small.bin` works
  without any configuration).
- If **no model at all** is installed, the error message now gives the full
  instructions: « Model not found - download a whisper.cpp model (e.g.
  ggml-medium.bin) into %APPDATA%\MusicPlayer\whisper-models\ ».

## [2026.08.046-c12] — 2026-08-08

### Fixed — transcription: ffmpeg.exe was never shipped
- The Transcribe button failed with « ffmpeg.exe not found »: the FFmpeg
  runtime only contained the 4 decoding DLLs, but the transcription plugin
  launches `ffmpeg.exe` to extract the audio. The runtime now ships the
  **full LGPL shared build** (8 files: the 4 previous DLLs +
  avdevice/avfilter/swscale + `ffmpeg.exe`, same BtbN build, verified
  identical hashes).
- The launcher now checks all 8 runtime files and re-downloads the full
  runtime when any is missing — existing installs get `ffmpeg.exe`
  automatically on next start.
- Transcription panel labels now have **hard-coded fallbacks** (Transcribe,
  Transcribing…, Transcription error, hint) when the local language file is
  outdated — no more raw keys like `now_transcribe` shown on screen.

## [2026.08.046-c11] — 2026-08-07

### Fixed — update check now works even when the GitHub API order is off
- The pre-release channel read only `releases?per_page=1` (the first item of
  the list). During CDN cache propagation, GitHub can return a freshly
  created release **out of order** — the check then reported « no update »
  while a newer release existed (e.g. c10 published but c9 still listed
  first).
- The client now reads the **10 most recent releases** and keeps the **best
  version** (proper semantic comparison), so a mis-ordered list can no
  longer hide an update.

## [2026.08.046-c10] — 2026-08-07

### Changed — Now playing panel layout (podcasts)
- The episode description now sits **immediately below the title** (the
  previous layout reserved a fixed-height box under the title, leaving a
  large empty gap before the description).
- Slightly larger fonts: title 17→18, description 12→13.
- The horizontal separator is now drawn **right under the description**,
  above the transcription area (button / status / transcript text).

## [2026.08.046-c9] — 2026-08-07

### Added — automatic plugin repair when the engine still runs outdated plugins
- When the engine was restarted but still runs outdated core plugins (a
  partial update left new executables + old DLLs), the client now **repairs
  itself**: it stops the engine, downloads the zip of its own version,
  extracts `core_plugins/*` and the language files over the install folder,
  restarts the engine and resumes the playlist. One attempt per session.
- The « engine plugins are outdated » message now has a **hard-coded
  fallback** (in English) when the local language file is itself outdated —
  no more raw key shown on screen (e.g. after a partial extraction that
  didn't replace `lang/fr.lang`).

## [2026.08.046-c8] — 2026-08-07

### Fixed — update script aborts cleanly when the engine cannot be stopped
- The updater script now **aborts with a clear FAIL** (and relaunches the app)
  when the engine is still alive after the kill attempts — a partial
  extraction previously left a **mixed install** (new executables + old
  core plugins), which is exactly why the podcast episode description never
  appeared even after updating.
- The client now shows an explicit message in the center panel when the engine
  was restarted but still runs outdated plugins: « Engine plugins are
  outdated — extract the update zip over the install folder… ». No more silent
  failure.

## [2026.08.046-c7] — 2026-08-07

### Fixed — episode description now appears even with an outdated engine
- When the episode lookup fails because the running engine still has
  **outdated plugins in memory** (the `MusicPlayerCore` Windows service keeps
  the old DLLs loaded even after an update), the client now **restarts the
  engine automatically**: it sends the REST shutdown (works for both the child
  process and the service), relaunches its own core from the updated install
  folder, and resends the playlist so playback continues. Happens once per
  session, after ~24 s of failed lookups.
- The podcasts plugin now answers `{"error":"episode not found"}` (distinct
  from the generic `not found`) when the episode is simply not in the
  subscriptions — the client knows not to restart the engine in that case.

## [2026.08.046-c6] — 2026-08-07

### Fixed — podcasts plugin never updated by automatic updates
- The **minimal zip** (used by the auto-updater) now includes
  `core_plugins/podcasts.dll`. Since c2 it was missing, so automatic updates
  never replaced the podcasts plugin: users kept the old version in memory,
  without the episode description endpoint — the episode title was shown but
  the **description never appeared**. A c6 update replaces the plugin, and the
  client's automatic retry (c5) then displays the full panel (title +
  description + Transcribe) without any manual step.

## [2026.08.046-c5] — 2026-08-07

### Fixed — podcast URL no longer displayed
- The center panel and the status bar no longer show the raw podcast URL.
  When the episode info (title + description) cannot be fetched yet, the
  **episode title** from the playlist is displayed instead — and nothing at
  all if no title is known.
- The episode lookup is now **retried every ~2 seconds** until the podcasts
  plugin responds: previously a single failed fetch left the URL on screen
  forever (e.g. when the engine/plugins were not up to date yet); now the
  full panel (title, description, Transcribe) appears automatically as soon
  as the endpoint answers, without changing tracks.

## [2026.08.046-c4] — 2026-08-07

### New — Podcast « Now playing » panel with offline transcription
- The center panel now shows, for the current podcast episode, its **title** and
  **description** (from the feed), replacing the plain « Now playing » text.
- A **Transcribe** button starts an offline transcription of the current episode
  (whisper, via the transcribe plugin): progress is polled (`extracting /
  transcribing`), an existing transcript is loaded automatically when the
  episode starts, the text scrolls with the mouse wheel, and errors are
  displayed inline.
- New plugin endpoint `GET /podcasts/episode?url=...` (title + description of
  one episode); episode descriptions are stored and **refreshed** when the feed
  updates (titles/descriptions/dates now update in place instead of keeping
  stale data).
- Transcription now accepts **http(s) URLs** (streamed podcast episodes), not
  only local files.

### Fixed — transcribe plugin
- **Double free** in the extraction error path corrupted the HTTP response
  (memory garbage instead of the JSON error) whenever audio extraction failed.
- **`SHGetFolderPathW` test inverted** (`!result` instead of `== S_OK`):
  `%APPDATA%\MusicPlayer\ffmpeg`, `\whisper` and `\whisper-models` were never
  found — ffmpeg.exe / whisper-cli.exe / models could never be located, so the
  plugin could not transcribe at all.
- **RIFF check ran on the process output pipe** (ffmpeg's text logs) instead of
  the produced WAV file — extraction always failed even with a working ffmpeg.

### Fixed — client
- `pod_json_unescape` re-encoded escaped bytes as code points, producing
  mojibake (`Ã©`) for accented podcast titles/descriptions.
- Playing an episode from the Podcasts dialog never updated the client playlist
  index, so the track name (and the new panel) stayed empty after a podcast
  play.
- Podcasts plugin: the episodes array (3.4 MB with descriptions) moved from the
  stack to the heap — the plugin crashed with a stack overflow on large feeds.

## [2026.08.046-c3] — 2026-08-07

### Fixed (update blocked by the running engine — « a program has files open »)
- The update now **stops the engine before applying**, including when it runs as a **Windows service** (`MusicPlayerCore`): the client sends the REST shutdown (`POST /api/cmd shutdown`), waits for the process to die (child: wait + forced stop fallback; service: health poll + SCM stop fallback). Previously the service kept running (24/7 by design) and locked `core_plugins/*.dll` — `taskkill` in the script cannot kill a LocalSystem service without elevation, so the extraction always failed
- The script now also stops the service (`sc stop MusicPlayerCore`) and **verifies each process is really gone** (`tasklist` loop, up to 10 tries) before extracting — no more race with a dying process still locking its files
- **`fwprintf` collapsed `%%i` → `%i`**: every generated `updater.bat` since 045-c21 contained `for /l %i in (…)` — invalid batch syntax, so the re-kill/verification loop was **silently skipped** on real Windows (the timeout sleeps were already no-ops without a console). The escape is now correct (`%%%%i` → `%%i` in the file)
- **Launcher runtime download URL broken since v044**: `REPO_DEFAULT_BASE` is a wide string but was passed to `MultiByteToWideChar` as ANSI — the FFmpeg runtime download from a fresh minimal-zip install always failed (fallback DLLs too). Now copied directly (`wcscpy`)
- After an update the engine service stays stopped until the next boot or a manual start (Settings ▸ Interface ▸ Windows service ▸ Start); the client starts the engine as a normal process in the meantime

## [2026.08.046-c2] — 2026-08-07 (pre-release branch)

### Fixed (plugin repository follows the release channel)
- The default plugin repository now follows the **release channel**: pre-release channel → `pre-release` branch repo (test plugins), release channel → `master` repo (production). Read directly from `upd.txt` (`channel=1`) so it works in the launcher, the client and the engine (auto plugin updates manifest too)
- If the pre-release branch repo **does not exist** (deleted on purpose), the client falls back to the master repo with a log line — no crash, no fake success
- The transcription plugin is now **embedded in the minimal zip** too (pre-release testing), not only in the full zip

## [2026.08.046-c1] — 2026-08-07

### Fixed (update cycle: app never relaunched, update never applied)
- The update script relaunched `MusicPlayer.exe --update-plugins`, and the client in that mode exited after updating plugins (`ExitProcess`) — the launcher then exited too, so **the app never came back** after an update. The plugin-update mode now falls through to the normal UI startup: after an update the app always returns to the screen
- The script's sleeps used `timeout.exe`, which fails instantly when the process has no console (the script is launched with `CREATE_NO_WINDOW`) — the extraction raced with the dying processes and `tar` failed on the still-locked exes, leaving the old version in place. The sleeps now use `ping -n N 127.0.0.1` (works with or without a console)
- The « Update plugins with the program » checkbox is now honored: `--update-plugins` is only passed to the relaunched launcher when it is checked (before, it was always passed)
- Verified end-to-end under Wine: script kills the processes, extracts, logs `OK: <version>`, cleans up, self-deletes, and the new client window comes up

## [2026.08.046] — 2026-08-07 (pre-release branch)

### Added (offline transcription — transcribe_whisper plugin)
- New SERVICE plugin `transcribe_whisper` (port 8083): offline speech-to-text via whisper.cpp (`whisper-cli.exe` subprocess)
- Endpoints: `GET /health`, `GET /models`, `POST /transcribe` (`path`, `lang` default auto, `model` default medium), `GET /transcripts`, `GET /transcript?file=`, `GET /progress`
- Pipeline: source → ffmpeg (WAV 16 kHz mono PCM, 300 s timeout) → whisper-cli.exe `-oj` (600 s timeout) → JSON parsed into segments
- Transcripts stored in `%APPDATA%\MusicPlayer\transcripts\<hash>.json` + `transcripts_index.json` (source → hash)
- Models expected in `%APPDATA%\MusicPlayer\whisper-models\ggml-<model>.bin` (download: separate spec)
- Verified under Wine: health/models/transcripts/progress + error paths (file not found, path required, forbidden)
- Not yet: client UI, model download, real transcription test (needs whisper-cli.exe + model)

## [2026.08.045-c24] — 2026-08-06

### Added (podcast episode titles in the playlist)
- The playlist now shows the **episode title** instead of the raw URL for podcast feeds: the client sends `{"url","title"}` objects in the playlist command, the engine stores titles alongside the playlist and exposes them in `/api/plist` (`titles`), and the playlist window displays them
- URL basenames now handle `/` separators too (previously an URL was shown in full)
- Verified: « 551 - La Biblinfini », « 550 - Convention pour les savoirs », … shown with their URLs

## [2026.08.045-c22] — 2026-08-05

### Fixed (podcast episodes not shown in the playlist window)
- The podcast Play filled the engine's playlist but the **client window displays its own local copy** (synchronised via `/api/plist` for folders only) — the episodes never appeared in the Playlist window
- The client now calls `cc_plist_refresh()` right after the podcast playlist command: opening a podcast feed behaves like opening a folder — the episodes appear in the Playlist window
- Also: `make test` now rebuilds the core binary (it was missing after `make clean`)

## [2026.08.045-c21] — 2026-08-05

### Fixed (update leaving the client binary behind — « zip contained X but binary is still Y »)
- The update script killed the 3 processes once and extracted immediately; a process that was still shutting down kept `MusicPlayerApp.exe` locked, so the client stayed on the old version while the launcher was updated (propriétés = new, programme = old)
- The script now **re-kills the 3 processes 3 times** (1 s apart) before extracting — the files are released, the extraction replaces everything

## [2026.08.045-c20] — 2026-08-05

### Fixed (podcast episodes missing from the player playlist)
- **Client**: the episodes-to-playlist JSON used a fixed 10 KB buffer — big feeds (Podcast Science: 562 episodes ≈ 56 KB of URLs) got truncated, the engine received an incomplete command and the playlist stayed empty. Now a **dynamic buffer** sized to the actual payload
- **Engine**: the REST JSON parser required `"cmd":"…"` / `"items":[…]` with no spaces — `json_str`/`json_num` now skip spaces, so both `"cmd": "playlist"` and `"cmd":"playlist"` work
- Verified: 2 episodes → playlist=2, 78 episodes → playlist=78, playback starts on the chosen episode

## [2026.08.045-c19] — 2026-08-05

### Added (podcast episodes as the player playlist)
- **Play (or double-click) on an episode now fills the player playlist with all the podcast's episodes** (new engine command `playlist` with `items` + `start`): playback continues episode after episode (Next, auto-advance) even after closing the Podcasts window
- Verified: playlist of 2 episodes, state=1, Next switches to the following episode

## [2026.08.045-c18] — 2026-08-05

### Fixed (podcast Play silent + infinite loop in the parser)
- **Infinite loop fixed**: when every item of a feed was filtered out (no-audio category feeds), the parser looped forever on the first item — the startup refresh appeared stuck
- **Page links no longer accepted as audio URLs**: the `<link>` fallback now only accepts URLs that look like media (.mp3, .mp4, .m4a, .ogg, .opus, .aac, audio/, media.) — a radiofrance article page is no longer mistaken for an episode
- **Automatic refresh at startup** (background thread, doesn't slow the engine): subscriptions are re-fetched and stale episodes purged — the old image-URL episodes stored before the audio filter are removed, so Play/Download no longer target images silently
- Verified: radiofrance/economie → 0 episodes (feed has no audio), acast → 78 playable episodes

## [2026.08.045-c17] — 2026-08-05

### Fixed (podcast Play doing nothing)
- The Play button re-fetches the episodes to resolve the selected URL; when the re-fetched list was shorter than the displayed one (episodes filtered out — e.g. no-audio entries), the index pointed past the end and the URL came back empty → nothing played. The index is now **clamped to the last available episode**
- Verified: the engine plays podcast episode URLs correctly (state=1, position advancing)

## [2026.08.045-c16] — 2026-08-05

### Fixed (podcasts: image enclosure taken as audio URL + episodes list still clearing)
- **Audio URL selection**: the parser now prefers an `<enclosure type="audio/*">` (iterating past image enclosures), falls back to `media:content` audio, then the first enclosure, then `<link>`. Episodes **without any audio URL are skipped** — category/channel feeds that only carry images (e.g. some radiofrance feeds) now show an empty list instead of broken Play/Download entries
- **Episodes list clearing fixed for real**: the subscription selection is now restored **before** the episodes list is rebuilt (the previous fix restored it after, so the list still went empty)

## [2026.08.045-c15] — 2026-08-05

### Added (software life cycle: release / pre-release channels)
- **Settings ▸ Update… ▸ « Release channel »**: the user chooses between
  - **Release (stable)** — only the latest non-pre-release (current behaviour)
  - **Pre-release (test)** — the latest published release, pre-releases included (for testing new builds before they go stable)
- Channel persisted in `upd.txt` (`channel=0|1`)
- Publishing workflow: test builds are tagged and uploaded as **GitHub pre-releases** (`gh release create --prerelease`), stable builds as normal releases — stable-channel users only ever get production versions

## [2026.08.045-c14] — 2026-08-05

### Fixed (podcasts: episodes list clearing after Play / double-click)
- After playing an episode (Play button or double-click), the refresh rebuilt the subscription list and **lost the selection** — the episodes list then stayed empty. The selection is now preserved across the refresh
- Episode state labels are now English: « Played » / « New » (was « lu » / « nouveau »)

## [2026.08.045-c13] — 2026-08-05

### Added / Fixed
- **Podcast episodes duration fixed**: the client now parses numeric JSON values — durations (e.g. acast: 63:35, 102:47…) display correctly; feeds without a duration tag (radiofrance) show « -- »
- **Column label « Dur » → « Length »** (interface stays English)
- **Settings ▸ Update… ▸ « Update plugins with the program »** checkbox (default on): after the program update applies, the launcher runs in `--update-plugins` mode and downloads every engine/runtime plugin from the repository (no locked-file issues: the engine is stopped at that point)

## [2026.08.045-c12] — 2026-08-05

### Fixed (podcast client↔engine communication — WinINet unreliable)
- The client's podcast HTTP calls (list, subscribe, search, sources) are now sent over **raw sockets** (same approach as the engine): WinINet was corrupting the request path (`GET /` instead of `GET /podcasts`) and dropping the responses (0 bytes) — making subscription and even the availability check fail on some systems
- Verified under Wine with the real client: `GET /podcasts` returns the full subscription list (589 bytes) and the engine logs show the fetch working

## [2026.08.045-c11] — 2026-08-05

### Fixed (podcast subscription never worked — the « POST » was a GET)
- **Root cause found**: the client's podcast HTTP helper used `InternetOpenUrlW`, which only ever sends **GET** — the « POST /podcasts » (subscribe) was actually a GET with a body. The engine replied with the subscription list (`{"podcasts":[]}`), the client misread it as « Invalid feed URL », and no fetch ever appeared in the engine logs
- The client now sends a **real POST** (`HttpOpenRequestW` + `HttpSendRequestW`) for requests with a body
- The engine tolerates legacy clients: a GET with a body is treated as a POST
- Verified under Wine: subscribe to `https://www.radiofrance.fr/rss/economie` → `{"ok":1,"title":"Économie : podcasts…","new":17}` with full fetch logs

## [2026.08.045-c10] — 2026-08-05

### Fixed (Update all leaving plugins locked by the engine)
- **« Update all » now retries the locked files automatically**: after the first pass, if some downloads failed (files locked by the running engine), the client proposes to stop the engine, retry and restart it — the previously locked plugins (e.g. Podcasts) get replaced and the new versions are loaded right away

## [2026.08.045-c9] — 2026-08-05

### Added (unmissable podcasts diagnostics)
- The **client logs every podcast API call** (`logs/musicplayer.log`): `Podcasts: POST /podcasts body={...} -> N o : {raw response}` — the raw server reply is visible even when the engine logs nothing
- The **podcasts plugin logs every fetch attempt** (`logs/musicplayer-core.log`): `Podcasts: fetching <url> (attempt 1/2)` before each attempt, then `fetched N o, M episodes` + `head: <first 120 chars>`
- With these lines, an « Invalid feed URL » is always explainable: network error (no head), HTML page (head shows `<!DOCTYPE`), binary (head shows dots) or valid XML (head shows `<?xml`)

## [2026.08.045-c8] — 2026-08-05

### Fixed (engine robustness against corrupt plugins)
- The engine now **checks the MZ magic before loading any plugin DLL** — a corrupt file (e.g. a downloaded 404) is skipped with a log line (« invalid image (not MZ), skipped ») instead of triggering the Windows system dialog (0xc000012f)

## [2026.08.045-c7] — 2026-08-05

### Fixed (corrupt plugin file from the repository — 0xc000012f)
- **The repository download no longer writes a non-DLL body into a .dll file**: a 404 (or any error page) used to be saved as `restapi.dll` (14 bytes of text) — Windows then refused to load it (0xc000012f « not a valid Windows image »). DLL/EXE downloads are now checked (MZ magic + minimum size) and rejected
- **Removed the stale « REST API (engine) » entry from the repository index** (the REST server on port 8080 is built into the engine — the plugin was a dead duplicate pointing to a non-existent file)
- If you already have a broken `restapi.dll` in `core_plugins/`: delete it (Settings ▸ Plugins… ▸ select the row ▸ Delete selected...)

## [2026.08.045-c6] — 2026-08-05

### Added / Fixed
- **All plugins now expose their build version** (`MP_BUILD_VERSION` baked in — the Web Server plugin no longer shows « 1.0 »)
- **« Update all » button** in Settings ▸ Plugin repository…: downloads every engine/runtime plugin of the repository in one click (locked files are reported)
- **Fixed the misleading post-update message**: the zip version comparison now strips trailing newlines — « Update failed… » no longer appears when the update actually succeeded

## [2026.08.045-c5] — 2026-08-05

### Fixed (downloading a plugin loaded by the engine)
- When a repository download fails because the file is locked by the running engine (loaded plugin — e.g. Podcasts — or FFmpeg DLL in use), the client now **offers to stop the engine, retry the download and restart it automatically** (playback is interrupted briefly). The new plugin version is loaded right away — no more « download failed » requiring manual file deletion
- Applies to every DLL-type category: service/engine, runtime, skin, visual, effect

## [2026.08.045-c4] — 2026-08-05

### Fixed (plugins list selection + repository download of a loaded plugin)
- **Selection of engine plugins in Settings ▸ Plugins…**: the read-only guard only locked the checkbox, not the row selection — **Delete selected...** now works on engine plugins too
- **Repository download when the plugin is loaded** (engine running): the DLL file is locked; the download now deletes the existing file (best effort) and retries before failing — no more « download failed » requiring a manual file removal
- **Diagnostic log**: the podcasts plugin logs the first 120 characters of the fetched content (`Podcasts: head: …`) — HTML/binary/XML is immediately visible in `logs/musicplayer-core.log` (level Debug)

## [2026.08.045-c3] — 2026-08-05

### Added (plugin versions in Settings ▸ Plugins…)
- New **Version** column in the plugins list (client plugins and engine plugins): each plugin now shows its build version
- The podcasts plugin exposes its build version (`MP_BUILD_VERSION`) — check it reads `2026.08.045-c3` after downloading the plugin from the repository

## [2026.08.045-c2] — 2026-08-05

### Fixed (podcast « invalid feed » on a valid feed)
- Feed fetch now retries once (network errors are often transient) — verified with `https://feeds.acast.com/public/shows/podcastscience`: **Podcast Science, 562 episodes** subscribed
- **Distinct error messages**: « Network error while fetching the feed » (connection/DNS/TLS) vs « Invalid feed URL » (genuinely not an RSS podcast feed)

## [2026.08.045] — 2026-08-05

### Added (Podcasts: feed sources & search directories)
- **Podcast sources list** (like the plugin repositories): RSS feeds and search directories, persisted in `%APPDATA%\MusicPlayer\podcasts\sources.txt`
- Default source: **Apple Podcasts** (iTunes Search API, no key required)
- **Podcasts ▸ Search…**: pick a source, type search terms, results list (title + author), **Subscribe** adds the podcast (its RSS feed)
- **Add source...**: type (search directory / direct RSS feed), name, URL — search directories use `{query}` as placeholder (e.g. `https://api.listennotes.com/api/v2/search?q={query}&type=podcast&token=…`, Podcast Index `…/search/byterm?q={query}`)
- Direct RSS sources appear in the source list and can be subscribed with one click
- Engine API: `GET /podcasts/sources`, `POST /podcasts/sources`, `POST /podcasts/sources/del`, `GET /podcasts/search?source=…&query=…` (parses iTunes/Apple, Listen Notes and Podcast Index result formats)

## [2026.08.044-c2] — 2026-08-05

### Fixed (update mismatch: the zip contained the previous version)
- After a version bump **without** `make clean`, the `bin/VERSION` file stayed stale (only copied by the `bin/runtime` rule, which was already up to date) → the zip embedded the old version and the startup check reported « update mismatch »
- New dedicated `bin/VERSION: VERSION` rule: the zip now always embeds the current version
- **Nuanced mismatch message**: if the zip contained an *older* version → informational note (« already applied or newer ») instead of an alarming warning; only a zip *newer* than the binary (extraction failed, files locked) shows the failure message

## [2026.08.044-c1] — 2026-08-05

### Fixed (FFmpeg runtime download failure + full zip fallback)
- Launcher hardened: 3 download retries for the runtime zip, then a fallback downloading the 4 DLLs individually (`repo/ffmpeg/*.dll`, written directly next to the exe, 2 retries each)
- Detailed error message when everything fails: expected folder + manual fix (full zip from the release page)
- New `make zip-full` rule and `MusicPlayer-*-win64-full.zip` release asset (program + FFmpeg DLLs) as a manual plan B

## [2026.08.044] — 2026-08-05

### Changed (FFmpeg 9.0 + runtime as a base plugin)
- **FFmpeg upgraded to 9.0 "Lei"** (released 2026-08-04): avcodec-63, avformat-63, avutil-61, swresample-7 (BtbN win64-lgpl-shared) — verified: playback, duration detection, selftest all pass
- **FFmpeg is now a base plugin (runtime)**: the program zip no longer embeds the FFmpeg DLLs (≈ 33 MB saved); the DLLs are downloadable from the plugin repository — `repo/ffmpeg/ffmpeg-win64-lgpl-shared.zip` (DLLs + LGPL license), entry **"FFmpeg runtime (required)"** in `plugins.json`
- **New launcher `MusicPlayer.exe`** (no FFmpeg imports): checks for the runtime DLLs at startup, downloads + extracts them automatically from the default repository if missing (native `tar.exe`), then starts the real client `MusicPlayerApp.exe` (exit code forwarded). The client cannot import the DLLs lazily — the Windows loader refuses to start without them
- `Settings ▸ Plugin repository… ▸ Download selected` on the FFmpeg entry also installs the runtime (zip extraction into the exe folder)
- The update script now also kills `MusicPlayerApp.exe` before extracting

## [2026.08.043-c5] — 2026-08-05

### Added (Settings ▸ Plugins… ▸ Delete selected...)
- Select a plugin (client or engine) and **Delete selected...** removes it: the DLL is unloaded and the file is deleted
- **Engine plugins** are removed through `POST /api/plugins/del` (unload + delete, `core_plugins/`)
- **Protected plugins cannot be deleted**: Web Server, Metadata, Cover (required), and the active skin
- The menu is rebuilt after a deletion (e.g. File ▸ Podcasts… disappears if the podcasts plugin is removed)

## [2026.08.043-c4] — 2026-08-05

### Fixed (installed podcasts plugin not visible in the Plugins list)
- The Settings ▸ Plugins… list only showed plugins loaded by the **client** process — engine plugins (`core_plugins/`, including Podcasts, Web Server, DLNA, RTP…) were invisible
- New engine API **`GET /api/plugins`** (name, type, description, enabled) and the client dialog now appends the engine plugins with the **`(engine)`** marker; their checkbox reflects the real engine state and is read-only (enable/disable stays on the engine side)

## [2026.08.043-c3] — 2026-08-05

### Fixed (update still not applying — now verified end to end)
- **`VERSION` is now embedded in the zip** (built into `bin/`), and the updater script logs the extracted version: `OK: 2026.08.043-c3` in `updater.log`
- **At startup the client compares its own version to the one the zip contained**: any mismatch (files locked, extraction skipped…) now shows an explicit message telling the user the extraction did not replace the files — no more silent « still on the old version »
- **Download integrity**: the zip must start with the `PK` magic — a 404/HTML page can no longer be deployed

### Changed (menu + logging)
- **File ▸ Podcasts… is now dynamic**: it only appears when the engine's podcasts plugin is present **and** its service answers on port 8082 (disabled plugin = no menu entry)
- **Help ▸ Logs…**: log level selector — **Nothing / Errors only / Info / Debug** — persisted in `config.yml`; logs now live in the **`logs/` folder** next to the executables (`logs/musicplayer.log`, `logs/musicplayer-core.log`), with an **Open logs folder** button; the engine's level follows live via `/api/config`

## [2026.08.043-c2] — 2026-08-05

### Fixed (Add… button garbled in the repository/podcasts dialogs)
- The button label used `…` (U+2026): windres reads the .rc as CP1252 while the file is UTF-8 → the label was garbled in the built dialog. Replaced with ASCII `Add...` (same for the Podcasts dialog)

## [2026.08.043-c1] — 2026-08-05

### Fixed (plugin repository: « Invalid repository index » on the default URL)
- The default repository base points to `…/master/repo`, but `plugins.json` lived at the **repo root** → the fetch hit a 404 and reported « Invalid repository index »
- **`plugins.json` moved into `repo/`** (next to the binaries), with a `url` field per plugin (raw GitHub URL) for the autonomous plugin update checker
- The loader's manifest URL (`PLUGIN_MANIFEST_URL`) aligned to `…/master/repo/plugins.json`
- Verified on GitHub: `repo/plugins.json` serves 34 plugins with working URLs

## [2026.08.043] — 2026-08-05

### Added (Podcasts)
- **Podcasts plugin (engine)** — `core_plugins/podcasts.dll`, HTTP on port 8082:
  - Subscribe to any RSS feed (`POST /podcasts` with the feed URL), unsubscribe, refresh all feeds
  - Episode list: title, date, duration (`itunes:duration`), read/unread state, resume position
  - **Playback streams the episode URL directly** — the engine's FFmpeg opens HTTP/HTTPS audio (verified: remote MP3 played in full with duration)
  - **Download** an episode for offline listening (`%APPDATA%\MusicPlayer\podcasts\`)
  - State persisted in `%APPDATA%\MusicPlayer\podcasts\` (podcasts.txt + episodes.txt)
  - Robust RSS parsing: HTML entities, numeric entities, `enclosure`/`link`, `pubDate` normalized
- **Client: File ▸ Podcasts…** — subscriptions list (title + unread count), episodes list (title/date/duration/state), buttons: **Add…**, Delete, Refresh, **Play** (streams the episode), **Read/Unread**, **Download**; double-click an episode to play it
- The engine already plays remote URLs: `{"cmd":"open","path":"https://…/episode.mp3"}` (HTTP/HTTPS)
- API (engine plugin, port 8082): `GET /podcasts`, `POST /podcasts`, `POST /podcasts/del`, `GET /podcasts/episodes?feed=…`, `POST /episodes`, `POST /refresh`, `POST /download`

## [2026.08.042-c7] — 2026-08-05

### Fixed (manual update still used broken PowerShell path)
- The « Update » button in the update-available dialog was still calling `apply_update_and_restart()` which used PowerShell `Expand-Archive -Force` — this fails silently when `musicplayer-core.exe` still runs (locks `core_plugins/webserver.dll`), so the extraction doesn't replace anything and the user stays on the old version.
- Manual update now uses `mp_update_apply_and_restart()` (the same reliable path as autonomous mode): tar.exe native extraction, kills both client + core before extracting, integrity check.
- Removed the dead `apply_update_and_restart()` function.
- Fixed `tools/vergen.py`: leading zero in version components (08, 042) caused windres `digit exceeds base` warning (octal parsing). Components are now cast to int and formatted as decimal.

## [2026.08.042-c6] — 2026-08-04

### Added (Windows file properties — Details tab)
- **Both executables now carry a VERSIONINFO resource**: right-click → Properties → Details shows File version, Product name, File description, Copyright, Company, Original filename
  - `MusicPlayer.exe` → « MusicPlayer - MP3/MP4 player (client) »
  - `musicplayer-core.exe` → « MusicPlayer Core - headless engine (client/server) »
- The resource is **generated from VERSION** (`tools/vergen.py`) — the file version always matches the release version (e.g. 2026.08.042-c6)

## [2026.08.042-c5] — 2026-08-04

### Fixed (autonomous update still not applying)
- **Extraction now uses `tar.exe` (built into Windows 10/11) instead of PowerShell** — no execution-policy or profile dependencies; the result is checked (`errorlevel` + presence of `MusicPlayer.exe`) and logged to `updater.log`
- **Download integrity check**: a response smaller than 1 MB (404, HTML error page…) is rejected before any deployment — the old code could extract a truncated/garbage file or silently fail
- Download timeout raised to 60 s
- The engine is force-stopped before extraction (its loaded `core_plugins/*.dll` used to lock the archive)

## [2026.08.042-c4] — 2026-08-04

### Added (plugin repository: a LIST of repositories)
- The repository window now manages a **list of repositories** (not a single URL): the project's own is there by default, **Add…** / **Remove** maintain the list, persisted in `%APPDATA%\MusicPlayer\repos.txt` (one URL per line)
- Selecting a repository in the list **fetches its index automatically**; Fetch reloads it manually
- Third-party repositories can be added the same way (any HTTP URL serving a `plugins.json` + files)

### Changed (documentation)
- **API.md translated to English** (and completed: `POST /api/config`, DJ commands) — all documentation is English-first

## [2026.08.042-c3] — 2026-08-04

### Fixed (update applied but the old version stayed)
The autonomous update relaunched the app without actually replacing the binaries. Causes fixed:
- **The updater script now force-stops the client AND the engine before extracting** (`taskkill /IM MusicPlayer.exe /F` + `musicplayer-core.exe`): the engine started at login keeps the `core_plugins/*.dll` loaded, which made `Expand-Archive -Force` fail silently — the old binary stayed in place
- **Extraction result logged** (`updater.log`): a failure is shown in a message box at the next start instead of failing silently
- **The relaunch uses the full path** of the exe (`"%~dp0MusicPlayer.exe"`)
- Release process: the zip asset is now uploaded **before** creating the release, so the update checker never downloads a missing/old asset

### Changed (zip content — even leaner)
- **TeamSpeak removed from the zip** (it is not an essential service; it stays in the plugin repository, one click away)
- Only the **engine web server** stays embedded: the phone remote works right after installation; everything else (TeamSpeak, visuals, effects, skins, other engine services) comes from Settings ▸ Plugin repository…

## [2026.08.042-c2] — 2026-08-04

### Added (plugin repository + third-party plugin guide)
- **Plugin repository** (Settings ▸ Plugin repository…): browse and download plugins/skins from remote repositories — **type filter** (Skin/Visual/Effect/Service) + **name search**, click **Download selected** and the file lands in the right folder (`plugins/`, `skins/`, `core_plugins/`)
- **Default repository = the project's own** (GitHub raw): index `plugins.json` (root) now lists **33 plugins/skins** with name/type/version/description
- **The zip no longer embeds the optional plugins/skins** (only the essential ones: engine services + TeamSpeak) — they are fetched from the default repository
- **`make repo`**: stages the binaries into `repo/bin/` (committed to publish the repository); third parties can host their own repository (any HTTP URL serving a `plugins.json` + files)
- **`PLUGINS.md`**: complete guide for third parties — plugin API v4, the 4 plugin types, building with MinGW, deploying by simple file copy, skins (palette, background, layout, window size), stream readers, lifecycle/threads
- **CHANGELOG.md fully translated to English** (all documentation is English-first from now on)

## [2026.08.042-c1] — 2026-08-04

### Changed (Settings ▸ Interface… : single buttons + compact window)
- **Single autostart button** — « Enable autostart » / « Disable autostart » (the label switches with the state)
- **Single engine button** — « Start engine now » / « Stop engine » (same behaviour)
- **Status line removed** (the registry access caused issues, and a single stateful button is enough)
- **The Full screen group adapts to the number of screens**: hidden rows (missing screens 3/4) shrink the group, the controls below move up and the window compacts — no more empty space

## [2026.08.042] — 2026-08-04

### Added (client/server architecture — DJ mode moves to the engine)
Architecture decision applied: **the DJ mix now happens in the engine**, not on the client.
- **`musicplayer-core.exe`**: the broadcast stream (`/stream`, web server, RTP, Multiroom…) now contains the **mix of decks A (main stream) and B (second decoder of the engine)** with volumes + crossfader
- **REST API**: new commands `dj_open_b` (path), `dj_play_b`, `dj_stop_b`, `dj_xf` (crossfader), `dj_vol_a`, `dj_vol_b`
- **Client**: the DJ console drives the engine through the API (the local mix was removed — the received stream already contains the mix); sliders show the sent values
- Consequence: the DJ mix is identical on **all** consumers (client speakers, phone, RTP, Multiroom)
- Verified under Wine: DJ commands ok, full `/stream` with DJ active, SELFTEST PASS

## [2026.08.041-c2] — 2026-08-04

### Fixed (RTP and Multiroom were stealing samples from each other)
The **RTP** and **Multiroom** broadcasters both used `web_read` (shared reader 0): when both were active, every packet read by one was lost for the other → choppy streams. Each one now reserves **its own reader** (`mp_web_reader_open` / `web_read_n` / close on exit) — completing Claude's phase 2 spec (web server/UPnP/REST were already migrated).
- Verified under Wine with RTP + Multiroom active: the client `/stream` receives the **full** stream (24 620 bytes) and playback stays clean

## [2026.08.041-c1] — 2026-08-04

### Changed (engine starts at login — no admin rights)
The Windows service (041) required administrator rights to install. Replaced by **automatic start at the user login**:
- **Autostart via HKCU\…\CurrentVersion\Run**: **no special rights required**
- **Engine icon in the notification area** (taskbar) with a **right-click** menu:
  - *Open MusicPlayer client* (launches the interface)
  - *Open web remote* (opens the browser on the web remote control)
  - *Exit* (stops the engine)
  - double-click: launches the client
- **Settings ▸ Interface…**: group « Start with Windows (login) » — **Enable autostart / Disable autostart / Start engine now / Stop engine** + status line (autostart enabled? engine running?)
- The client connects to the engine when it is already running (started at login) without relaunching it, and does not stop it on exit; an engine started by the client itself is stopped with it

## [2026.08.041] — 2026-08-04

### Added (client/server architecture — phase 3: Windows service)
- **`musicplayer-core.exe --service`**: the engine runs as a **Windows service** (24/7, without an open session) — `StartServiceCtrlDispatcherW`, SCM control (stop/shutdown → same shutdown path as the API, `WM_APP+1`), SERVICE_RUNNING/STOPPED status
- **Settings ▸ Interface…**: « Windows service (engine 24/7) » group with **Install / Uninstall / Start / Stop** + status line — through the Service Control Manager (`advapi32`)
- **The client adapts**: if the service is running it connects **without launching the engine**; on exit it **does not stop it** (the service outlives the client)
- Installation: `CreateService` (auto start at boot), binPath `"…\musicplayer-core.exe" --service`
- Tested: normal mode unchanged (playback, playlist, web server 200, SELFTEST PASS); `--service` behaves correctly (under Wine, no SCM → clean exit)

## [2026.08.040-c6] — 2026-08-04

### Fixed (transport buttons — Claude spec 3)
The commands were sent and executed correctly, but **~1.5-2 s of audio remained in transit** after pressing a button (core web_ring 0.74 s + TCP buffers 0.37 s + client ring 0.74 s) — the sound kept playing after Stop/Pause, and the icon only switched at the 250 ms polling.

- **`mp_web_flush()`** (core): flushes the broadcast stream (all readers repositioned on the write cursor) — called by `mp_stop`, `mp_seek`, `mp_open` **before** the position reset
- **`SO_SNDBUF 8192`** in `stream_loop`: bounds in-flight socket audio to ~46 ms
- **`sp_flush()`** (client): local ring purge requested by the UI, drained **by the audio callback** (the only owner of the tail) — instant silence on stop/pause/track change
- **`client_transport(cmd, flush)`**: purge + command + **immediate state refresh** (no more delayed icon)
- **Deterministic `client_play_pause()`**: no more `playpause` toggle (two fast clicks on a stale state = dead button) — decides `play`/`pause` from the engine's known state
- **Stop/Seek/Next/Prev**: purge + immediate state; Space and S keys like the buttons; plugin API (host_play_pause/host_stop) wired to the new functions
- No purge on pause: the buffer is kept so playback resumes without a gap; DJ mode untouched

### Verified (Wine)
- Stop: state 0, position 0, immediate ✓ · Pause: state 2, position frozen ✓
- Playback, playlist, web server 8000: 200 ✓ · 0 warnings (client, plugins, core) + SELFTEST PASS

## [2026.08.040-c5] — 2026-08-04

### Fixed (playback impossible — Claude spec 2: ghost reader 0)
- **Ghost reader 0 removed**: reserved in `mp_init()` but never read, its frozen cursor held the read floor → `web_ring_write_bp` saw the ring always full → the decoder blocked after ~0.74 s of audio → no playback at all. `mp_web_read` now opens its reader **on first use** (`mp_web_reader_open_locked` variant without lock)
- **Anti-block safety valve** in `web_ring_write_bp`: if a reader does not consume for 2 s (dead socket, killed client…), it is **force-repositioned** (the engine can never freeze again); decoding stays interruptible (`!g_interrupt && g_state == PLAYING`) → Stop/Seek respond immediately
- **Command deadlock** (playidx/next/prev/open folder): the playlist lock was held during `mp_open` — the decoder waits for that lock at the end of a track → deadlock. Commands now release the lock before `mp_open`

### Verified (Wine)
- `playidx 0` → 01_a.mp3 played **in full** (6.104/6.0 s); next → 02_b.mp3; prev → 01_a.mp3 ✓
- Open folder: playback + automatic track chaining of the whole playlist ✓
- Full client: 3 tracks played, web server 8000: 200 ✓
- 0 warnings (client, plugins, core) + SELFTEST PASS

## [2026.08.040-c4] — 2026-08-04

### Fixed (sound, commands, web server — Claude spec applied in 3 phases)
**Phase 1 — backpressure restored (the sound)**
- `sp_ring_write` (client): **blocking write** — if the ring is full, wait for the sound card to consume instead of dropping: this blockage paces the whole chain on the sound card (a track lasts its real duration, the position advances in real time)
- TeamSpeak **peek cursor bounded** (repositioning when the reader lags too far)
- **~200 ms pre-fill** before the device starts (network thread started first, no initial silence)
- `core_http.c`: the **fake `Sleep(5)` pacing** removed (the blocking `send()` now paces)
- `player.c`: the **`& WEB_RING_MASK`** mask removed from the fill calculation (full ring detected as 0 → overwriting unread data)

**Phase 2 — multiple readers on the core stream**
- **4 independent read cursors** (`mp_web_reader_open/close/read_n`): each consumer (client `/stream`, phone web server, UPnP, REST) has its own cursor — no more sample stealing between consumers
- The track position only advances when the **slowest reader** progresses (no double counting with several clients)
- Plugin API **v4** + `web_reader_open/close/read_n` in the host API (web server, UPnP, REST migrated)

**Phase 3 — web server restarted**
- The core now sends the `MP_SERVICE_WEB_APPLY` event at startup: **web server 8000, RTP, UPnP, Multiroom start** (they were waiting for this event the core never sent)
- `POST /api/config` endpoint (web_enabled/web_port/web_ips applied live) + `cc_push_web_config` on the client side (the 4 settings sites)
- The core **no longer overwrites `config.yml`** on exit (the client is the only owner of the config)

### Verified (Wine)
- Position advancing in real time, pause (state 2, frozen position), stop (state 0), playpause ✓
- Web server 8000: 200 on page and API ✓ · RTP/UPnP/Multiroom started ✓
- 0 warnings (client, plugins, core) + SELFTEST PASS

## [2026.08.040-c3] — 2026-08-04

### Fixed (sound — the client ring buffer was corrupted)
- **The stream player ring wrote at the READ position (tail) instead of the WRITE position (head)**, and advanced the tail while writing: played data was garbled (crossed L/R samples, partial packets, intermittent silence) — the « indescribable sound ». Rewritten as a standard SPSC: the producer (network thread) writes at `head`, the consumer (audio callback) reads at `tail`
- Diagnosed with a **dump of the actually played stream**: before — 30 469 jumps > 0.5 and a 3303 Hz frequency (noise); after — **0 jumps, clean 440 Hz sine**
- Consequence of the bug: the client swallowed the stream too fast → the engine reached the end of the track early → « Playing then Finished then Playing » cycle — fixed (the client now consumes at the sound-card pace, the engine chains at real speed)

## [2026.08.040-c2] — 2026-08-04

### Fixed (sound — root cause)
- **The TeamSpeak plugin was stealing the main device stream**: it read the stream ring via `web_read` **destructively** — each sample was read only once, either by the speakers or by TS → choppy main sound (half the samples) and shifted TS. `sp_web_read` now does a **non-destructive read** (separate read cursor): TS sees the full stream without removing samples from the device
- Resample state reset on every engine reconnection

## [2026.08.040-c1] — 2026-08-04

### Fixed (sound — 040 regression)
- **L/R misalignment of the stream** (the « indescribable sound »): network reads (`InternetReadFile`) arrive at **arbitrary sizes**; the old code dropped the remaining 1-3 bytes at each read, progressively misaligning the left/right channels (37 % of samples mixed — measured). The stream player now keeps **residual bytes** between reads (carry buffer): 0 misaligned samples over 6 144 frames (tested)
- **Wrong pitch on 48 kHz sound cards**: the engine stream is 44 100 Hz but the client device may play at 48 000 — **linear resampling** of the stream to the device's real sample rate (fractional position + interpolation, state kept between blocks)

## [2026.08.040] — 2026-08-04

### Added (client/server architecture — phase 2: wired client)
- **The client drives the engine** (`MusicPlayer.exe` → `musicplayer-core.exe`):
  - at startup, the client **launches the engine** if not running (and **stops it on exit**: API shutdown + forced stop as fallback);
  - every command goes through the REST API (play, pause, stop, next, prev, seek, speed, shuffle, open, playidx);
  - state (position, duration, title, metadata) arrives via **`/api/state` polling** (250 ms);
  - the **client playlist is synchronized** through `/api/plist` (the engine scans, the client displays)
- **Local stream player** (`stream_player.c`): the client receives the engine's PCM (`/stream`) and plays it on its sound card — local volume, **plugin effects** (equalizer, sound quality), **local DJ mix** and **visual analysis** in the callback
- **TeamSpeak on the client**: the plugin broadcasts the received stream (host `web_read` → local stream)
- **Network plugins removed from the client**: web server/restapi/upnp/rtp/multiroom are only loaded by the **core** (`core_plugins/`) — no more double server or port conflicts (filter in the loader)
- Tested under Wine: the client launches the core, syncs the playlist (3 tracks), playback chains and finishes on the engine, TeamSpeak starts on the client

## [2026.08.039] — 2026-08-04

### Added (client/server architecture — phase 1: standalone engine)
- **`musicplayer-core.exe`**: the UI-less engine — playlist, FFmpeg decoder, CD, network service plugins (web server, metadata, cover, UPnP, RTP/AES67, Multiroom) in a dedicated **`core_plugins/`** folder. Invisible window, chaining timers, `musicplayer-core.log`
- **Public REST API** (port 8080, `svc_rest_port`) documented in **`API.md`**: `/health`, `/api/state`, `/api/plist`, `/api/cover`, `/api/levels`, `/stream` (WAV PCM 44.1 kHz stereo), `POST /api/cmd` (play, pause, stop, next, prev, seek, speed, shuffle, open, playidx, shutdown — anti-CSRF JSON Content-Type)
- **Broadcast stream**: no sound card on the engine — the position advances at the pace of the clients consuming `/stream` (backpressure), the end of track is detected by the consumer
- **`player.c` compiles in two modes**: `MP_CORE` (no miniaudio, broadcast) and normal (local sound card) — the current client is **unchanged and works**
- Tested under Wine: state/plist/cover/levels/cmd ✓, `/stream` = real PCM (peak 2752) ✓, clean shutdown ✓
- The zip includes `musicplayer-core.exe` + `core_plugins/`

## [2026.08.038-c1] — 2026-08-04

### Fixed (Update dialog)
- **The 3 radio groups are independent**: `WS_GROUP` was missing on the first button of each group (« Update mode », « Update type », « Delay ») — all radios formed a single group and only one choice was possible. Mode AND type AND delay can now be chosen.

## [2026.08.038] — 2026-08-04

### Added (Settings ▸ Update…)
- **Autonomous mode**: checks **every hour**, applies the update and **restarts without asking** (1 h timer + script-based application: wait for exit, extract the zip, relaunch, self-cleanup)
- **Update types**: all (default) or **fixes only** (`-cX` versions)
- **Delay before applying/reporting**: 0, 1 day, 1 week (default) or 1 month — a release more recent than the delay is neither offered nor applied (`published_at` compared to the local clock)
- Persistence: `upd.txt` as `mode=… / type=… / lag=…` (old single-parameter format still read)

## [2026.08.037-c2] — 2026-08-04

### Added
- **Playback commands in the File menu**: Play/Pause, Stop, Next, **Previous** (new, loops over the playlist), **Shuffle** (checkmark, state synced when the menu opens) — also reachable when a skin hides the menu (right-click)

## [2026.08.037-c1] — 2026-08-04

### Fixed (skins — 2nd pass)
- **Right-click: submenus are no longer destroyed** — `DestroyMenu` recursively destroyed the attached popups of the bar (File/Settings/Plugins/Help): the first right-click worked, the following ones showed dead entries. The popup detaches its submenus (`RemoveMenu`) before being destroyed — tested: 3 right-clicks in a row without crash, menus intact
- **Control bar painted at the right place**: with a « top controls » skin, the background was painted at the bottom (opaque band over the photo) — it is painted where the buttons are (top when `g_skin_ctrl_top`)
- **Semi-transparent veils** on the bars (controls 63 % and progress 50 %) when the skin has a background image: the artwork stays readable, the icons keep their contrast
- Orphan comment of `get_visual_rect` removed

## [2026.08.037] — 2026-08-04

### Changed (full-window skins — overhaul)
- **Plugin API v3**: new host entry point `skin_set_window_size(w, h, fixed)` and extended `skin_set_layout` (menu, controls, **status bar**)
- **Two separate rectangles**: `get_content_rect` (background image, controls and progress surface) ≠ visualizer zone (the skin's, converted to local coordinates) — the background is no longer overpainted in the visualizer zone
- **1:1 background rendering** (native size, no stretching): full-window skins impose the **exact artwork size** (640×300), window **not resizable** (WM_GETMINMAXINFO bounded, non-stretchable border, maximize greyed), status bar hidden
- **Skin state reset between skins** (image, visual zone, layout, size): switching from « Vintage radio » to « Clean » restores the visible menu, the status bar and a resizable window
- **Skins updated**: Vintage radio (dial 222,202,196,36), Winamp (analyzer 30,212,580,54, controls in the band); the 9 palette skins unchanged
- Tested under Wine: radio = exact **640×300** window, 1:1 artwork (wood pixels present), spectrum in the dial; Clean = 632×266, visible menu, default state

## [2026.08.036-c10] — 2026-08-04

### Fixed (code review — 5th pass)
- **`http_post_is_json` tightened**: the `Content-Type` value is **bounded to the end of its line** (a body containing the string « application/json » can no longer pass another type — tested: `text/plain` + trapped body → 403) and the header search is **case-insensitive** (compatibility with HTTP/2/lib clients normalizing to lowercase — tested: `content-type:` → 200)

## [2026.08.036-c9] — 2026-08-04

### Fixed / Refactored (code review — 4th pass)
- **`examples/http_util.h` created** (header-only, one per DLL): shares the robust request reading (recv loop + termination before strstr + Content-Length body + 5 s timeout), bounded-length HTTP responses, the WAV header and float→PCM16 conversion — the 3 HTTP plugins (web server, REST API, UPnP) use it; a fix no longer has to be ported to 3 copies
- **Premature strstr fixed in `plugin_upnp.c` and `plugin_restapi.c`** (their recv loop copies had the same flaw as the web server: `req` not terminated before the strstr) — via `http_read_request`
- Shared anti-CSRF (`http_post_is_json`); stream WAV/PCM16 buffers via common helpers

## [2026.08.036-c8] — 2026-08-04

### Fixed (code review — 3rd pass)
- **UPnP `media_stream`: per-connection buffers** (last remaining static on a multi-client path — `plugin_rtp.c` / `plugin_multiroom.c` keep their statics: single service thread, correct) + `snprintf` everywhere in the plugin
- **player.c: explicit `#include <math.h>`** for `llround` (no longer relying on the transitive include of miniaudio.h)

## [2026.08.036-c7] — 2026-08-04

### Fixed (code review — 2nd pass)
- **Position: speed factor integrated** — each played block is accumulated as `got / speed` in the callback: the bar shows the **track time** whatever the speed (tested: at 2×, the position reaches 6:00/6:00 at the end of a 6-minute track instead of 3:00), and stays exact if the speed changes mid-track
- **`dj_stream_thread` and `api_stream`: per-thread buffers** (no more shared static — the DJ page opens decks A and B simultaneously, nominal usage)
- **Out-of-bounds read fixed in the recv loop**: `req[rn] = 0` is set **before** each `strstr` (the buffer was not terminated → strstr could read beyond rn, a network-reachable path)
- **REST API CORS documented and assumed**: open reading (`Access-Control-Allow-Origin: *` for GET), protected commands (POST requires `Content-Type: application/json` → the cross-origin preflight fails, no `Allow-Headers`)
- **README: vendor instructions fixed** (`win64-gpl-shared` → `win64-lgpl-shared`) — no more risk of regenerating the license problem

## [2026.08.036-c6] — 2026-08-04

### Fixed (build, robustness, CI)
- **Makefile: header dependencies** (`-MMD -MP` + `-include *.d`) — modifying `player.h` now triggers recompilation of the affected .c files
- **Binary hardening**: `-fstack-protector-strong` + `-D_FORTIFY_SOURCE=2` (the code parses network data and ID3 tags)
- **`%APPDATA%` missing: no more uninitialized buffer** — `GetEnvironmentVariableW` tested everywhere (config.c, plugin_loader.c) with fallback to the exe folder
- **update.c: GitHub response buffer raised to 64 KB** — the JSON (tag_name) is no longer truncated by construction
- **GitHub Actions CI added** (`.github/workflows/build.yml`): MinGW-w64 toolchain + Wine, `make setup` (vendor), zero-warning build, plugins, **selftest under Wine**, zip — run on every push/PR

## [2026.08.036-c5] — 2026-08-04

### Fixed (HTTP security & robustness)
- **CSRF blocked**: POST requests (`/api/cmd` of the web server **and** of the REST plugin) now require `Content-Type: application/json` — without this marker, the server answers **403**. A « simple » request (form, headerless fetch) can no longer drive the player from a third-party site; the web pages' JS sends the header (tested: without header → 403, with → 200)
- **A silent/slow client no longer blocks the server**: the dispatcher reads the request **in a loop** until the end of the headers (a single `recv()` could return only part) + **waits for the announced body** (`Content-Length`) + **5 s receive timeout** — a connection that sends nothing is closed after the timeout instead of blocking all the following ones

## [2026.08.036-c4] — 2026-08-04

### Fixed (code review)
- **Exact playback position**: it now counts the frames actually **played** (callback, after resampling → device rate) instead of decoded frames — no more advance (8.8 % at 48 kHz, ×2 at speed 2×, ~6 s of ring)
- **Web mixer deck A fixed**: the `/dj/streamA` URL loaded deck B (wrong index `path[4]` → `path[10]`)
- **Local DJ deck B: no more decoding in the audio callback** — decoding moved to a **dedicated thread + SPSC ring buffer** (like deck A), no more dropouts on slow disk I/O; mix buffer in 4096-sample blocks
- **Use-after-free eliminated**: `mp_dj_b_close` stops the thread (interrupt_callback) and **waits for its end** before freeing the FFmpeg contexts
- **`/stream`: per-connection buffers** (no more shared static between threads — two clients corrupted each other)
- **`snprintf` everywhere** (guaranteed termination) + bounded lengths in HTTP responses (`http_response_len`) — no more out-of-bounds `strlen` on truncation
- **Ring buffers with memory barriers** (`__atomic` RELEASE/ACQUIRE instead of `volatile`): data visibility guaranteed under GCC
- **FFmpeg leaks on error paths**: `mp_dj_b_open` and `dj_open` free cleanly (fmt/codec/swr) via `goto done`

## [2026.08.036-c3] — 2026-08-03

### Changed
- **FFmpeg license: GPL → LGPL switch** — the project is distributed with the **BtbN `win64-lgpl-shared`** builds (FFmpeg n8.1) instead of the GPL ones: the source code stays **MIT** (dynamic linking with LGPL FFmpeg DLLs does not contaminate the project); `LICENSE-FFmpeg.txt` (FFmpeg LGPL notice) is included in the distributed archive

## [2026.08.036-c2] — 2026-08-03

### Fixed
- **Main window flicker eliminated**: `WM_ERASEBKGND` no longer redraws the background directly on screen every frame (the `WM_PAINT` double buffer covers the whole window) and all invalidations go through non-erasing mode — no more « window closing and reopening » impression; the equalizer benefits from the same treatment

## [2026.08.036-c1] — 2026-08-03

### Changed
- **Equalizer docked to the core dimensions**: the equalizer window has **exactly the same width as the main window** and **resizes with it** (sliders automatically redistributed)
- **Equalizer: core theme** — the window uses **the active skin's palette** (background, text, accent, tracks, sliders, border) and updates live when the skin changes (new `get_skin_colors` API)

## [2026.08.036] — 2026-08-03

### Added
- **Equalizer plugin** (Plugins ▸ Effects ▸ Equalizer): **10-band Winamp-style** equalizer (60 Hz – 16 kHz, ±12 dB) + preamp — its window is **detached and docks under the main window** (it follows when moving); clickable/draggable vertical sliders, ON/OFF button, [✕] button to hide (reopen via the Plugins menu)

## [2026.08.035] — 2026-08-03

### Added
- **Independent plugin updates**: each plugin has a **« core.NNN »** version (e.g. `2026.08.034-c1.002` — validated with core 2026.08.034-c1). At startup, the client queries the **`plugins.json`** manifest (GitHub) and **downloads only the plugin DLL** whose version differs — no new program version (restart to load)
- The **Fractal** plugin moves to `2026.08.034-c1.002` (first plugin versioned this way)

### Fixed
- **Fractal: zoom freezing after ~10 s** — the zoom exceeded the double precision (all pixels identical); it is now capped (2^30) and **loops forever** on the boundary

## [2026.08.034-c1] — 2026-08-03

### Fixed
- **Fractal v2.1**:
  - the zoom starts **on the set's boundary** (seahorse valley −0.74529+0.11308i, verified edge point) — the infinite boundary zoom principle is respected
  - **no more flicker**: zoom passed by atomic value (audio thread → UI thread), NaN-free iteration smoothing, stable hue by position with very slow drift (tested: 0–1 pixel changing between two frames)
  - **dynamic iterations** with the zoom (up to 400) to keep resolving the boundary deep down

## [2026.08.034] — 2026-08-03

### Changed
- **Fractal visual plugin rewritten (v2.0)**: a **real Mandelbrot fractal** (Sea Horse Valley) and **Julia** that **zooms to the rhythm of the music** — bass energy is analyzed on the audio stream, each beat pushes the zoom deeper into the fractal (gentle decay between beats), animated hues, smooth rendering at enlarged reduced resolution

## [2026.08.033-c1] — 2026-08-03

### Fixed
- **Multi-screen fullscreen fixed**:
  - the Interface dialog shows **Screen 1, 2, 3…** according to the detected screens (no more fixed « 2, 3, 4 ») and **each screen has its content**, including the 1st (visual, playlist, lyrics or cover applied to the main window)
  - secondary windows cover the **other monitors** (the main window's excluded), all true fullscreen
  - **leaving fullscreen** closes all secondary windows and restores completely

## [2026.08.033] — 2026-08-03

### Changed
- **TeamSpeak Broadcast becomes a plugin**: removed from the Settings menu, it now lives in **Plugins ▸ Services ▸ TeamSpeak Broadcast** (like REST API, DLNA…). At startup it automatically picks the device (exact name in `ts_device.txt` next to the DLL, else a virtual audio cable « CABLE »/« VoiceMeeter », else the default device)

## [2026.08.032] — 2026-08-03

### Added
- **True fullscreen**: the window fully covers the monitor under the cursor (no title bar, no taskbar) — F11/Esc
- **Multi-screen fullscreen**: Settings ▸ Interface… — number of screens used (auto-detection shown) and **content of each screen**: visual effect, playlist, lyrics or cover
- **Settings ▸ Network…**: like the Web server dialog — for each network service (REST API, DLNA/UPnP, RTP/AES67, Multiroom): **editable port** and **IPs to use** (checkboxes); plugins comply at restart

## [2026.08.031] — 2026-08-03

### Added
- **5 new plugins** (Plugins ▸ Services / Effects):
  - **REST API**: HTTP JSON server on port 8080 (`/api/state`, `/api/playlist`, `/api/cover`, `/api/cmd`, `/api/stream`, CORS)
  - **RTP/AES67 Output**: stream broadcast in RTP L16 multicast (239.255.0.1:5004) + SAP announcement (VLC/AES67 reception)
  - **DLNA/UPnP Media Server**: UPnP AV server (SSDP + ContentDirectory) exposing the playlist to DLNA devices (TV, phone…)
  - **Multiroom**: multi-room broadcast (RTP multicast 239.255.0.2:5004 + targets from `multiroom.txt`)
  - **Sound Quality**: audio effect (sub-bass filter, bass boost, presence, soft limiter)

## [2026.08.030] — 2026-08-03

### Added
- **TeamSpeak 3 broadcast**: Settings ▸ **Broadcast to TeamSpeak…** — pick an output device (e.g. **CABLE Input** of a Virtual Audio Cable): the music (and DJ mix) is broadcast on it in addition to the speakers; in TeamSpeak 3, select the cable as **microphone** to broadcast the music on the server

## [2026.08.029] — 2026-08-03

### Added
- **Full local DJ mode on the computer**: each deck has its **▶ play / ⏸ pause / ■ stop** buttons (deck B is a **real independent second decoder** mixed into the audio output) and its **volume slider**; at the bottom of the console: the **A/B crossfader** and the **pitch slider** (speed) — real 2-way mixing now happens on the computer too, not only on the web

## [2026.08.028-c5] — 2026-08-03

### Added
- **Complete local DJ console**: each deck now has a **track selector** (click the deck = menu with the whole playlist, select-style) and a **play button ▶** (plays the chosen track on that deck) — each deck's music is chosen independently, like on the web

## [2026.08.028-c4] — 2026-08-03

### Fixed
- **Local DJ console repaired**: the 2 decks are now well delimited (fixed colors, borders, DECK A/B titles and track names) and the console occupies the whole central zone — no longer distorted by the skin-imposed visualizer zone or skin colors

## [2026.08.028-c3] — 2026-08-03

### Added
- **DJ Mixing mode synchronized computer ⇄ web**:
  - **Settings ▸ DJ Mixing** (or `/api/cmd dj`) toggles the mode on both sides
  - On the computer: the window shows the **local DJ console** (2 decks with tracks, click to play)
  - On the web: the main page **switches automatically to the mixer** when DJ mode is active, and the « Quit » button returns to the remote control

## [2026.08.028-c2] — 2026-08-03

### Changed
- **DJ Mixing mode: real 2-way mixer** — each deck has its **volume**, its **pitch (±12 %)**, its **3-band equalizer** (bass/mid/treble), its **play/pause** and **stop** buttons, and the central **A/B crossfader** — all processed in real time by the Web Audio API (both decks play simultaneously)

## [2026.08.028-c1] — 2026-08-03

### Fixed
- **Web page repaired**: the script referenced an element that no longer existed (`meta`) and crashed on every refresh — the cover and playlist no longer displayed. Metadata is now written to its own zones (title, artist, album, year)
- **DJ Mixing mode link added** on the main page (« 🎚️ DJ Mixing » button)

## [2026.08.028] — 2026-08-03

### Added
- **Web page: full metadata** — under the command buttons: title, artist, album, year (ID3 TYER/TDRC), then the cover and the playlist
- **CD audio playback** — File ▸ Open CD…: the disc tracks replace the playlist (play/pause/stop/next, automatic track chaining)
- **DJ Mixing mode (web page `/dj`)** — mixer: 2 decks (one playlist track per deck), A/B crossfader, each deck streams independently

## [2026.08.027-c2] — 2026-08-03

### Fixed
- **Menu that never came back**: after the Vintage radio skin (hidden menu), returning to the default interface left the window without a menu bar — the bar was created in a local variable at startup, the menu recall could not find it

## [2026.08.027-c1] — 2026-08-03

### Fixed
- **Crash fixed**: when the menu bar is hidden (Vintage radio skin), a click in the context menu (speed, plugins…) caused an error — all menu manipulations now go through the real bar even when hidden

## [2026.08.027] — 2026-08-03

### Added
- **Skin-customizable layout**: each skin can **hide the menu bar** (menu reachable by **right-click**) and **move the control buttons** (top, where the menu was, or bottom) — the background image covers **the whole window**
- **Vintage radio skin**: the menu bar is replaced by the control buttons at the top; the menu stays reachable by right-click

## [2026.08.026-c1] — 2026-08-03

### Fixed
- **Zip fixed**: the `skins/` folder (DLL + textures) is now included in the distribution archive
- **Vintage radio skin**: photorealistic texture generated by AI (ComfyUI) — real 1950s dial radio, the visualizer plays in the speaker grille

## [2026.08.026] — 2026-08-03

### Changed
- **Skins in their own `skins/` folder** (next to the exe) — no longer mixed with plugins in `plugins/`
- **Skins submenu removed from the Plugins menu** — the skin is chosen only in Settings ▸ Interface…
- **Vintage radio skin**: the visualizer plays in the radio's **speaker** (skin-imposed zone), texture redrawn (golden « VINTAGE RADIO » band, graduated AM dial, speaker grille)

## [2026.08.025] — 2026-08-03

### Added
- **Full skins**: a skin now customizes the whole interface:
  - **Background image** of the main window (stretched), displayed under the visualizer
  - **Menu bar** drawn with the skin's palette (background, text, hover) — including the bar background
  - **Configuration windows** (Web server, Plugins, Interface, Update, About) in the skin's colors (background + texts)
- **Vintage radio skin**: full valve radio texture (wood, golden « VINTAGE RADIO » band, graduated AM dial, speaker) displayed behind the visualizer
- **Winamp skin**: classic Winamp-style texture (title bar, playlist zone, equalizer)

## [2026.08.024] — 2026-08-03

### Added
- **Settings ▸ Interface…**: window to choose the **skin** (dropdown, default palette included) and the **language** — the Language submenu is removed from the Settings menu
- **Settings ▸ Update…**: window to configure the **update mode** (automatic at startup / manual / disabled) and **check now** — « Check for updates » and « Check for updates at startup » are removed from the Settings menu

## [2026.08.023-c1] — 2026-08-03

### Fixed
- **Skins: no more deactivation** — choosing a skin in Plugins ▸ Skins no longer removes the other skins from the list: all 11 stay displayed, the (radio) selection only changes the active skin
- **Plugins dialog (Settings ▸ Plugins…)**: it now defines the plugins **shown in the Plugins menu** (label fixed: « Plugins to show in the Plugins menu »); **skins no longer appear there** — they are chosen only in Plugins ▸ Skins
- **Plugins menu**: selecting/deselecting a plugin (check or radio) no longer removes it from the list
- Only one skin is applied at startup (the first active one of the radio selection)

## [2026.08.023] — 2026-08-03

### Added
- **Richer web remote**:
  - **Cover art of the current track** displayed on the page (`/cover` endpoint — MP3-embedded cover or cover.jpg/folder.jpg next to the track)
  - **Metadata** in the state: title, artist, album of the playing track
  - **Track titles** (ID3 tags) in the web playlist instead of file names

### Changed
- **Exclusive skins**: only one active skin at a time (radio selection, like visuals) — re-clicking the active skin returns to the default palette

## [2026.08.022] — 2026-08-03

### Added
- **Skins**: 11 skins in Plugins ▸ Skins — 60s, 70s, 80s (neon), 90s, 2000s retro, vintage radio (wood & gold), Winamp, clean, kitsch, cartoon, black & white. Each skin applies a full color palette (background, text, buttons, volume, progress) instantly
- **Playlist window**: playlist button (L key) — track list with the current track highlighted; double-click or Enter to play the chosen track
- **Cover plugin** (Cover art): displays the current track's image — MP3-embedded cover (APIC frame) or cover.jpg / folder.jpg / cover.png / front.jpg placed next to the track

## [2026.08.021] — 2026-08-03

### Added
- **Settings ▸ Plugins…**: window listing all plugins with checkboxes to enable/disable them (state persisted in `plugins.ini`)
- **Lyrics plugin**: displays the song's lyrics (`.lrc` file placed next to the track) — click in Plugins ▸ Services
- **Clickable progress bar**: click to jump directly to a moment of the music
- **Window and taskbar icon** (WM_SETICON)
- **About: clickable GitHub link** (« Open GitHub » button)

### Changed
- **Automatic update**: 3-choice warning window before installation — « Update now » (downloads the zip, closes and relaunches the application automatically), « Later », « Ignore this version » (only later versions will be offered)

## [2026.08.020] — 2026-08-03

### Changed
- **The web server is now a plugin** (Web Server, Service type) — visible in Plugins ▸ Services, disableable in Settings ▸ Plugins
- **Volume booster**: the audio effect plugin is now called « Volume booster » (+25 % with clipping)

### Added
- **MP3 Metadata plugin** (Service type): reads ID3 tags (title) of MP3 files — the interface shows the title instead of the file name when available
- **Settings ▸ Plugins**: enables/disables each plugin (state persisted in `plugins.ini`); a disabled plugin no longer appears in the Plugins menu

## [2026.08.019] — 2026-08-03

### Added
- **`config.yml` configuration file** (`%APPDATA%\MusicPlayer\config.yml`): player state saved on exit and restored at startup
  - Volume, speed, shuffle mode
  - Last opened path (file or folder) and current file
  - Web server configuration (enabled, port, audio output, listened IPs)
- **Automatic playlist rescan at startup**: new files (folder and subfolders) are added, missing files are dropped, and playback resumes on the current track
- Automatic migration of the old `web.txt` file

## [2026.08.018-c5] — 2026-08-03

### Added
- **Web server window: network interface list with checkboxes** — each IP address (with the interface name) can be enabled or disabled for listening; the server only listens on the checked IPs (default: all)

## [2026.08.018-c4] — 2026-08-03

### Added
- **Web remote: sticky remote** — buttons and title stay visible at the top of the screen when scrolling the playlist
- **Click on a track** in the web playlist to jump directly to it
- **Shuffle mode**: 🔀 button in the application (control bar, orange when active) and on the web remote — the next track is chosen at random

## [2026.08.018-c3] — 2026-08-03

### Added
- **Web remote: clear audio output button** — one button (icon + label) shows the current sound mode: 🖥️ **PC** / 📱 **Phone** / 🔀 **Both**; one click changes the mode (cyclic) directly from the phone, the configuration is saved
- The play button of the page also starts/stops the sound on the phone (Phone/Both modes); stop cuts the sound everywhere

## [2026.08.018-c2] — 2026-08-03

### Fixed
- **Web page: remote icons replaced by inline SVG** — the play (▶/⏸) and other buttons displayed badly on some browsers/phones (badly rendered Unicode ⏸ and emojis)
- **Dedicated « Phone sound » button**: playing sound on the phone is now independent of the play button — in « both » mode, the play button controls the application, and a speaker button (purple) starts/stops the sound on the phone

## [2026.08.018-c1] — 2026-08-03

### Fixed
- **Settings ▸ Web server… did nothing**: the dialog template id in the resource was symbolic (`IDD_WEB`) instead of the expected number (104) — the dialog could not be loaded. Fixed (numeric id 104).

## [2026.08.018] — 2026-08-03

### Added
- **Remote-control web server** (Settings ▸ Web server…) — the web page is reachable from the phone or tablet on the same network
  - **Free port auto-detected from 8000**, user-modifiable
  - **Remote**: play/pause, stop, next, volume +/-, speed +/-
  - **Playlist displayed** with the current track highlighted (refreshed every second)
  - **Audio output**: this computer, phone only, or both simultaneously — the phone receives the sound via a WAV audio stream broadcast by the server (/stream)

## [2026.08.017-c2] — 2026-08-02

### Added
- **File ▸ Open folder…: real folder picker** — back to the classic `SHBrowseForFolderW` dialog (with COM initialized): you choose a **folder**, not a file

### Fixed
- **Update check: `-cX` fixes are detected** — the version comparator handles the correction suffix (e.g. `2026.08.017` → `2026.08.017-c1` reported as an update; `-c2` > `-c1`)

## [2026.08.017-c1] — 2026-08-02

### Fixed
- **File ▸ Open folder…: the application froze / closed** after the picker dialog (unstable shell dialogs: `IFileOpenDialog` hangs, `SHBrowseForFolderW` destroys the window) — replaced by the **classic open dialog** `GetOpenFileNameW` (the same as File ▸ Open…, the most reliable): pick any MP3/MP4 of the folder → the whole folder is played (subfolders included)
- Dialog title shortened

## [2026.08.017] — 2026-08-02

### Fixed
- **File ▸ Open folder… menu showed the « menu_open_folder » key**: the new English keys (`menu_open_folder`, `err_folder`, updates…) were missing in the default table
- **Crash when clicking Open folder…**: the folder picker moves from `SHBrowseForFolderW` (unstable without initialized COM) to the modern **IFileOpenDialog** (FOS_PICKFOLDERS, COM initialized) — more stable on Windows 11

## [2026.08.016] — 2026-08-02

### Added
- **Folder playback (playlist)**: File ▸ Open folder…, drag & drop of a folder or command line — recursive MP3/MP4 scan (subfolders included), sort by name, automatic next-track playback, stop at the end of the playlist; « [n/total] » counter in the status bar
- **⏭ Next button** in the control bar (N shortcut)

### Fixed
- truncated playlist paths: `%ls` mandatory for `wchar_t*` in `swprintf` (MinGW) — the scan was scanning the wrong folder

## [2026.08.015] — 2026-08-02

### Added
- **Update check** (GitHub Releases):
  - manual: **Settings ▸ Check for updates…** (shows the result even when up to date)
  - automatic: **Settings ▸ Check for updates at startup** (4 s after launch, silent when up to date), preference persisted in `%APPDATA%\MusicPlayer\upd.txt`
  - local version compared to the latest release; if a new version exists: dialog with a link to the releases page (Yes button)
  - background query (WinINet thread, 10 s timeout): the interface never blocks

## [2026.08.014] — 2026-08-02

### Changed
- **3D Isometric: low frontal camera** (matching the references) — no more « square split in two » of the diagonal axes: the rows rise while shrinking toward the center (central vanishing point), the floor grid is a trapezoid, the background bars are smaller, taller and darker (2 gradient sets), bright tip on each bar
- Analysis gain increased (18) and smoothing softened (0.95) for a denser landscape with music

## [2026.08.013] — 2026-08-02

### Fixed
- **3D Isometric: critical display bug** — the projected height `gh` became negative with the time axis reversed (`by < 0`), giving a negative grid scale: bars off-screen / inverted scene. Fixed with the absolute value
- Audio analysis stabilized: **continuous 0.74 s buffer** (instead of a single block) + **silence detection** (zeros no longer empty the landscape, very slow fall) — the landscape now fills up regularly, including under Wine

## [2026.08.012] — 2026-08-02

### Changed
- **3D Isometric: real isometric side view** — the axes go diagonally (frequencies → bottom-right, time → top-left), each bar has 3 faces (gradient front, dark side, bright top), grid centered by bounding-box computation
- **No more flicker**: double-buffered rendering (offscreen drawing + single BitBlt) over the whole central zone

## [2026.08.011] — 2026-08-02

### Changed
- **3D Isometric fully reworked, GLBars / WM3DSpectrum style**:
  - **rectangular perspective grid** (background rows smaller and taller) — no more isometric lozenge
  - **vertical gradient on each bar**: dark at the base, vivid and bright at the top (6 segments, white glow)
  - **grid centered** on the screen (horizontal and vertical), 24 columns × 14 rows

## [2026.08.010] — 2026-08-02

### Changed
- **3D Isometric**: grid now **square 24×24** (instead of 30×16) — like the WM3DSpectrum / 3D spectrogram references

## [2026.08.009] — 2026-08-02

### Fixed
- **3D Isometric**: grid and bars now centered on the screen (the grid's logical center was confused with the projection origin — offset right and down), vertical centering including the bar height

## [2026.08.008] — 2026-08-02

### Added
- **« 3D Isometric » visual plugin**: isometric 3D bar landscape (WM3DSpectrum / 3D spectrogram style) — rectangular frequency × time grid, 30 rainbow columns, 16 scrolling history rows, 3D faces (light front, dark side, lozenge top), night-blue background
- Window enlarged to 640×300 (more room for the 3D landscapes), minimum size 420×260

## [2026.08.007] — 2026-08-02

### Changed
- **3D Spectrum aligned on the Spectrum3D reference rendering**: very dark night-blue background, 96 thin bars, rainbow gradient by position (blue → cyan → green → yellow → orange → red), darkened back — wireframe grid removed

### Fixed
- **Settings menu**: the languages submenu no longer appears twice (replacement position fixed)
- **Window resizing**: the background is repainted in white before each render (WM_ERASEBKGND regression), minimum size 420×220 added

## [2026.08.006] — 2026-08-02

### Changed
- **3D Spectrum restyled Spectrum3D-like** (spectrum3d.sourceforge.net): black background, wireframe floor grid, 72 thin bars (white→yellow→red), darkened back
- **Simplified menus**: File · Settings · Plugins · Help — Playback and Volume menus removed
- **Settings** gathers: Speed, Fullscreen, Language
- **Keyboard shortcuts removed from menu labels** (no more « Ctrl+O » shown)
- **Volume 0–200 %**: slider with blue zone (0–100 %) and orange (booster 100–200 %), mark at 100 %

### Fixed
- **Window resizing**: status bar shares recalculated, whole zone redrawn (visual, progress, controls)

## [2026.08.005] — 2026-08-02

### Added
- **Control buttons** in the window: play/pause (blue) and stop (red) — not only in the menu
- **Volume slider**: track + clickable/draggable handle, next to the buttons
- **Fullscreen for visual effects**: ⛶ button, F11 key, Esc to exit
- **Plugins menu organized by type**: Visual / Audio effects / Skins submenus — only one active visual at a time (radio)
- **Application icon** (purple play on rounded background, 5 sizes) — no more default Windows icon
- Visual plugins: **3D Spectrum** (rotating cylinder of bars, 3D projection), **Fractal** (per-pixel plasma, 256 colors, reacts to music), **Hypnotic** (tunnel of rotating rings pulsed by the music)
- Window enlarged (640×240) to host the control bar

### Changed
- Buttons and slider drawn in GDI (no dependency), the visual rendering occupies the zone above the progress

## [2026.08.004] — 2026-08-02

### Added
- **Multilingual**: text files `lang/<code>.lang` (UTF-8, `key=value`, `#` comments) — anyone can add a language without recompiling
- **Language / Langue menu**: instant switch, preference remembered in `%APPDATA%\MusicPlayer\lang.txt`
- **English by default** (embedded in the binary), automatic detection of the Windows language, French provided (`fr.lang`)
- Provided files: `lang/en.lang`, `lang/fr.lang`, guide `lang/README.md`

### Changed
- All interface texts go through the translation engine (menus, status bar, dialogs, messages, states)
- Selftest and log messages in English

## [2026.08.003] — 2026-08-02

### Added
- **Progress bar**: blue→yellow gradient, always visible (text and plugin modes)
- **VUMeter visual plugin**: stereo LED VU meter (24 LED/channel, -45..0 dB, slow-decay peaks, clip indicator) — Winamp/XMMS style
- **Fireworks visual plugin**: fireworks synchronized on the music (adaptive energy beat detection, gravity, trails, 360 colors)
- The loader now shows only **one visual plugin** at a time (choice via the Plugins menu)

### Changed
- **Spectrum plugin restyled**: 256-hue rainbow palette (blue→red), night-blue gradient background, discrete grid, glow effect around the bars

## [2026.08.002] — 2026-08-02

### Added
- **Spectrum visual plugin**: spectrum visualizer (radix-2 1024-point FFT, 48 logarithmic bars, green→red palette, bright peaks, smoothing)
- Plugin API **v2**: `audio_frames()` hook — read-only PCM stream (after effects) for visual plugins; v1 plugins are rejected cleanly
- Visual rendering wired: central zone replaced by the plugin (~30 FPS) when a visual is active
- Example plugin compilation: `make plugins-examples` (gaindemo + spectrum in bin/plugins/)
- Portable archive including the plugins

### Changed
- The engine broadcasts each audio block to visual plugins after the effects

## [2026.08.001] — 2026-08-02

### Added
- MP3/MP4 player for Windows (Win32 + FFmpeg + miniaudio), MinGW cross-compilation under Linux
- Open… menu (Ctrl+O) + file drag & drop
- Play / pause (Space), stop with reset to 0 seconds (S)
- Volume 0–100 % (↑/↓), displayed in the status bar
- Speed 0.5× / 1× / 1.5× / 2× (dynamic resampling)
- Status bar: file, position/duration, speed, volume
- Plugin API v1 (skin, audio effect, visual) + DLL loader with hot reload
- `--selftest` mode: pipeline validation (playback, speed, pause, stop, end) under Wine
- Makefile: `make`, `make test`, `make zip`
