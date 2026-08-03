/*
 * MusicPlayer — moteur de langues
 * Fichiers "lang/<code>.lang" : UTF-8, "cle=valeur", '#' commentaires,
 * séquence \n interprétée. L'anglais est embarqué (tableau g_en) et sert
 * de langue de secours.
 */
#include "lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 128
#define MAX_LANGS 32

typedef struct {
    char key[48];
    wchar_t* val;
} entry_t;

static entry_t g_table[MAX_KEYS];
static int g_nkeys = 0;

static wchar_t g_code[8] = L"en";
static wchar_t g_native[48] = L"English";

static wchar_t g_lang_dir[MAX_PATH] = { 0 };

static lang_info g_langs[MAX_LANGS];
static int g_nlangs = 0;

/* ------------------------------------------------------------------ */
/* Anglais embarqué (langue de secours)                                */
/* ------------------------------------------------------------------ */
static const struct { const char* key; const wchar_t* val; } g_en[] = {
    { "lang_name",        L"English" },
    { "menu_file",        L"&File" },
    { "menu_play",        L"&Playback" },
    { "menu_volume",      L"&Volume" },
    { "menu_settings",    L"&Settings" },
    { "menu_plugins",     L"P&lugins" },
    { "menu_lang",        L"&Language" },
    { "menu_help",        L"&Help" },
    { "open",             L"&Open…" },
    { "menu_open_folder", L"Open &folder…" },
    { "menu_open_cd", L"Open &CD…" },
    { "open_folder_title", L"Pick a file — the whole folder plays" },
    { "quit",             L"&Quit" },
    { "play_pause",       L"&Play / Pause" },
    { "stop",             L"&Stop" },
    { "speed",            L"&Speed" },
    { "vol_up",           L"&Increase" },
    { "vol_down",         L"&Decrease" },
    { "vol_show",         L"Volume: %d%%" },
    { "plugins_reload",   L"&Reload plugins" },
    { "plugins_none",     L"No plugin loaded" },
    { "plugins_visual",   L"Visual" },
    { "plugins_effects",  L"Audio effects" },
    { "plugins_skins",    L"Skins" },
    { "plugins_services", L"Services" },
    { "menu_plugins_cfg", L"&Plugins…" },
    { "menu_interface",   L"&Interface…" },
    { "menu_update_cfg", L"&Update…" },
    { "menu_dj", L"DJ &Mixing" },
    { "interface_skin",   L"Skin :" },
    { "interface_lang",   L"Language :" },
    { "updcfg_group",     L"Update mode" },
    { "updcfg_auto",      L"&Automatic (check at startup)" },
    { "updcfg_manual",    L"&Manual" },
    { "updcfg_off",       L"&Disabled" },
    { "updcfg_check",     L"Check &now…" },
    { "plugins_dlg_title", L"Plugins" },
    { "plugins_dlg_lbl",  L"Plugins to show in the Plugins menu :" },
    { "fullscreen",       L"&Fullscreen" },
    { "menu_web_server",  L"&Web server…" },
    { "web_dlg_title",   L"Web server" },
    { "web_enable",      L"Enable web server (remote control)" },
    { "web_port",        L"Port:" },
    { "web_audio_out",   L"Audio output:" },
    { "web_listen",      L"Listen on:" },
    { "web_pc",          L"This computer" },
    { "web_phone",       L"Phone" },
    { "web_both",        L"Both" },
    { "web_ok",          L"OK" },
    { "web_cancel",      L"Cancel" },
    { "web_err_port",    L"Cannot start the web server on port %d.\nThe port may be already in use." },
    { "upd_title",        L"Update" },
    { "upd_new",          L"A new version is available : %hs\nYour version : %s\n\nUpdate now ?" },
    { "upd_now",          L"Update now" },
    { "upd_later",        L"Later" },
    { "upd_skip",         L"Skip this version" },
    { "upd_downloading",  L"Downloading %hs…\nThe application will restart automatically." },
    { "upd_dl_error",     L"Update download failed. Check your connection and try again." },
    { "upd_uptodate",     L"You are up to date (version %hs)." },
    { "upd_error",        L"Update check failed — network error or GitHub unreachable." },
    { "about",            L"&About…" },
    { "no_file",          L" (no file)" },
    { "state_stopped",    L"Stopped" },
    { "state_playing",    L"Playing" },
    { "state_paused",     L"Paused" },
    { "state_finished",   L"Finished" },
    { "center_stopped",   L"Stopped" },
    { "center_playing",   L"Now playing" },
    { "center_paused",    L"Paused" },
    { "center_finished",  L"Finished" },
    { "open_title",       L"Open audio file" },
    { "filter_audio",     L"Audio files (*.mp3;*.mp4)" },
    { "filter_mp3",       L"MP3 (*.mp3)" },
    { "filter_mp4",       L"MP4 (*.mp4)" },
    { "filter_all",       L"All files (*.*)" },
    { "err_open",         L"Cannot open \"%s\".\nUnsupported format or corrupted file." },
    { "err_folder",       L"No playable audio files (MP3/MP4) found in \"%s\"." },
    { "about_title",      L"About" },
    { "about_text",       L"MusicPlayer %hs (app)\n\nAudio player for MP3 / MP4 (Windows).\n"
                          L"Decoding : FFmpeg %hs\nAudio : miniaudio\nPlugins : %d loaded\n\n"
                          L"Shortcuts : Space = play/pause, S = stop,\n↑/↓ = volume, Ctrl+O = open" },
    { NULL, NULL }
};

/* ------------------------------------------------------------------ */
/* Parser d'un fichier .lang                                           */
/* ------------------------------------------------------------------ */
static void clear_table(void)
{
    for (int i = 0; i < g_nkeys; i++) free(g_table[i].val);
    g_nkeys = 0;
}

static int load_file(const wchar_t* path)
{
    FILE* f = _wfopen(path, L"rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }
    if (sz > 1 << 20) sz = 1 << 20;   /* limite de sécurité : 1 Mo */

    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;

    /* BOM UTF-8 éventuel */
    char* p = buf;
    if (got >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) p += 3;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, p, -1, NULL, 0);
    wchar_t* wbuf = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wbuf) { free(buf); return -1; }
    MultiByteToWideChar(CP_UTF8, 0, p, -1, wbuf, wlen);
    free(buf);

    clear_table();

    wchar_t* save = NULL;
    wchar_t* line = wcstok(wbuf, L"\r\n", &save);
    while (line && g_nkeys < MAX_KEYS) {
        wchar_t* q = line;
        while (*q == L' ' || *q == L'\t') q++;
        if (*q && *q != L'#') {
            wchar_t* eq = wcschr(q, L'=');
            if (eq) {
                *eq = 0;
                /* clé : trim droite */
                wchar_t* ke = q + wcslen(q);
                while (ke > q && (ke[-1] == L' ' || ke[-1] == L'\t')) *--ke = 0;
                /* valeur : trim gauche + droite */
                wchar_t* v = eq + 1;
                while (*v == L' ' || *v == L'\t') v++;
                size_t vlen = wcslen(v);
                while (vlen > 0 && (v[vlen - 1] == L' ' || v[vlen - 1] == L'\t')) vlen--;

                WideCharToMultiByte(CP_UTF8, 0, q, -1, g_table[g_nkeys].key, 48, NULL, NULL);
                /* dé-échappement de \n */
                wchar_t* val = (wchar_t*)malloc((vlen + 1) * sizeof(wchar_t));
                size_t j = 0;
                for (size_t i = 0; i < vlen; i++) {
                    if (v[i] == L'\\' && i + 1 < vlen && v[i + 1] == L'n') {
                        val[j++] = L'\n';
                        i++;
                    } else {
                        val[j++] = v[i];
                    }
                }
                val[j] = 0;
                g_table[g_nkeys].val = val;
                g_nkeys++;
            }
        }
        line = wcstok(NULL, L"\r\n", &save);
    }
    free(wbuf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* API publique                                                        */
/* ------------------------------------------------------------------ */
const wchar_t* lang_get(const char* key)
{
    for (int i = 0; i < g_nkeys; i++)
        if (strcmp(g_table[i].key, key) == 0) return g_table[i].val;
    for (int i = 0; g_en[i].key; i++)
        if (strcmp(g_en[i].key, key) == 0) return g_en[i].val;
    static wchar_t fallback[48];
    MultiByteToWideChar(CP_UTF8, 0, key, -1, fallback, 48);
    return fallback;
}

const wchar_t* lang_code(void)        { return g_code; }
const wchar_t* lang_native_name(void) { return g_native; }

const lang_info* lang_list(int* count)
{
    *count = g_nlangs;
    return g_langs;
}

/* Lit le nom natif d'une langue : parse léger, ne touche PAS la table */
static void read_native_name(const wchar_t* path, wchar_t* out, int out_chars)
{
    out[0] = 0;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 << 20)) { fclose(f); return; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;

    char* p = buf;
    if (got >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) p += 3;

    /* cherche la ligne "lang_name = valeur" */
    char* hit = strstr(p, "lang_name");
    if (hit) {
        char* eq = strchr(hit, '=');
        if (eq) {
            char* v = eq + 1;
            while (*v == ' ' || *v == '\t') v++;
            char* e = v;
            while (*e && *e != '\r' && *e != '\n') e++;
            while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
            *e = 0;
            MultiByteToWideChar(CP_UTF8, 0, v, -1, out, out_chars);
            out[out_chars - 1] = 0;
        }
    }
    free(buf);
}

/* Scan du dossier : liste des fichiers .lang + nom natif de chacun */
static void scan_dir(const wchar_t* dir)
{
    g_nlangs = 0;

    /* anglais embarqué, toujours disponible */
    wcscpy(g_langs[g_nlangs].code, L"en");
    wcscpy(g_langs[g_nlangs].name, L"English");
    g_nlangs++;

    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*.lang", dir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (g_nlangs >= MAX_LANGS) break;
        /* code = nom du fichier sans extension */
        wchar_t code[8];
        wcsncpy(code, fd.cFileName, 7);
        code[7] = 0;
        wchar_t* dot = wcschr(code, L'.');
        if (dot) *dot = 0;
        if (wcscmp(code, L"en") == 0) continue;   /* déjà embarqué */

        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);

        wchar_t nm[48];
        read_native_name(path, nm, 48);
        if (nm[0]) {
            wcscpy(g_langs[g_nlangs].code, code);
            wcsncpy(g_langs[g_nlangs].name, nm, 47);
            g_langs[g_nlangs].name[47] = 0;
            g_nlangs++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void lang_init(const wchar_t* dir, const wchar_t* code)
{
    if (dir) {
        wcsncpy(g_lang_dir, dir, MAX_PATH - 1);
        g_lang_dir[MAX_PATH - 1] = 0;
    }
    scan_dir(g_lang_dir);

    const wchar_t* wanted = code;
    wchar_t syscode[8] = L"en";
    if (!wanted) {
        /* langue du système Windows */
        LANGID lid = GetUserDefaultUILanguage();
        switch (PRIMARYLANGID(lid)) {
        case LANG_FRENCH:     wcscpy(syscode, L"fr"); break;
        case LANG_GERMAN:     wcscpy(syscode, L"de"); break;
        case LANG_SPANISH:    wcscpy(syscode, L"es"); break;
        case LANG_ITALIAN:    wcscpy(syscode, L"it"); break;
        case LANG_PORTUGUESE: wcscpy(syscode, L"pt"); break;
        case LANG_DUTCH:      wcscpy(syscode, L"nl"); break;
        case LANG_JAPANESE:   wcscpy(syscode, L"ja"); break;
        case LANG_CHINESE:    wcscpy(syscode, L"zh"); break;
        case LANG_RUSSIAN:    wcscpy(syscode, L"ru"); break;
        case LANG_POLISH:     wcscpy(syscode, L"pl"); break;
        case LANG_TURKISH:    wcscpy(syscode, L"tr"); break;
        case LANG_ARABIC:     wcscpy(syscode, L"ar"); break;
        default:              wcscpy(syscode, L"en"); break;
        }
        wanted = syscode;
    }

    wcscpy(g_code, L"en");
    wcscpy(g_native, L"English");
    if (wcscmp(wanted, L"en") != 0) {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\%ls.lang", dir, wanted);
        if (load_file(path) == 0) {
            wcscpy(g_code, wanted);
            const wchar_t* nm = lang_get("lang_name");
            wcsncpy(g_native, nm, 47);
            g_native[47] = 0;
        }
    }
}

int lang_set(const wchar_t* code)
{
    if (!code || !code[0]) return -1;
    /* l'anglais embarqué n'a pas besoin de fichier */
    if (wcscmp(code, L"en") == 0) {
        clear_table();
        wcscpy(g_code, L"en");
        wcscpy(g_native, L"English");
        return 0;
    }
    /* vérifier que la langue est disponible */
    int n;
    const lang_info* li = lang_list(&n);
    for (int i = 0; i < n; i++)
        if (wcscmp(li[i].code, code) == 0) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%ls\\%ls.lang", g_lang_dir, code);
            if (load_file(path) == 0) {
                wcscpy(g_code, code);
                const wchar_t* nm = lang_get("lang_name");
                wcsncpy(g_native, nm, 47);
                g_native[47] = 0;
                return 0;
            }
        }
    return -1;
}
