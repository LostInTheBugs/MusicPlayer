/*
 * MusicPlayer — chargeur de plugins
 * Scanne le répertoire "plugins/" (à côté de l'exe), charge chaque DLL
 * exportant mp_plugin_entry et l'enregistre. Rechargement à chaud via
 * le menu Plugins > Recharger. Chemins en UTF-16, logs en UTF-8.
 */
#include <stdio.h>
#include <string.h>

#include "plugin_loader.h"

#define MP_MAX_PLUGINS 32

static mp_plugin g_plugins[MP_MAX_PLUGINS];
static int g_count = 0;
static const mp_host_api* g_host = NULL;

/* convertit un nom UTF-16 en UTF-8 pour le journal */
static void name_to_utf8(const wchar_t* in, char* out, int out_bytes)
{
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, out_bytes, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* Activation persistée : %APPDATA%\MusicPlayer\plugins.ini            */
/* ------------------------------------------------------------------ */
static void cfg_path(wchar_t* out, int chars)
{
    GetEnvironmentVariableW(L"APPDATA", out, chars);
    wcscat(out, L"\\MusicPlayer");
    CreateDirectoryW(out, NULL);
    wcscat(out, L"\\plugins.ini");
}

/* 1 si le plugin est activé (défaut : activé) */
static int cfg_get(const char* key)
{
    wchar_t path[MAX_PATH];
    cfg_path(path, MAX_PATH);
    FILE* f = _wfopen(path, L"r");
    if (!f) return 1;
    char line[512];
    int v = 1;
    while (fgets(line, sizeof(line), f)) {
        char k[256];
        int val = 0;
        if (sscanf(line, "%255[^=]=%d", k, &val) == 2 && !strcmp(k, key)) {
            v = val ? 1 : 0;
            break;
        }
    }
    fclose(f);
    return v;
}

static void cfg_set(const char* key, int on)
{
    wchar_t path[MAX_PATH];
    cfg_path(path, MAX_PATH);
    char lines[32][512];
    int n = 0;
    FILE* f = _wfopen(path, L"r");
    if (f) {
        while (fgets(lines[n], 512, f) && n < 31) n++;
        fclose(f);
    }
    FILE* out = _wfopen(path, L"w");
    if (!out) return;
    int written = 0;
    for (int i = 0; i < n; i++) {
        char k[256];
        int val;
        if (sscanf(lines[i], "%255[^=]=%d", k, &val) == 2 && !strcmp(k, key)) {
            fprintf(out, "%s=%d\n", key, on ? 1 : 0);
            written = 1;
        } else {
            fputs(lines[i], out);
        }
    }
    if (!written) fprintf(out, "%s=%d\n", key, on ? 1 : 0);
    fclose(out);
}

/* clé de configuration d'un plugin : nom du fichier sans ".dll" */
static void plugin_key(const wchar_t* dll_path, char* out, int out_bytes)
{
    const wchar_t* slash = wcsrchr(dll_path, L'\\');
    const wchar_t* base = slash ? slash + 1 : dll_path;
    char u8[256];
    name_to_utf8(base, u8, sizeof(u8));
    strncpy(out, u8, (size_t)out_bytes - 1);
    out[out_bytes - 1] = 0;
    char* dot = strrchr(out, '.');
    if (dot && !_stricmp(dot, ".dll")) *dot = 0;
}

static void unload(mp_plugin* p)
{
    if (p->dll) {
        if (p->api && p->api->destroy)
            p->api->destroy(p);
        FreeLibrary(p->dll);
    }
    memset(p, 0, sizeof(*p));
}

static int load_one(const wchar_t* dir, const wchar_t* name, const mp_host_api* host)
{
    char name_u8[256];
    name_to_utf8(name, name_u8, sizeof(name_u8));

    if (g_count >= MP_MAX_PLUGINS) return -1;

    wchar_t full[MAX_PATH];
    swprintf(full, MAX_PATH, L"%ls\\%ls", dir, name);

    HMODULE dll = LoadLibraryW(full);
    if (!dll) {
        char msg[512];
        _snprintf(msg, sizeof(msg), "Plugin %s : LoadLibrary failed (err=%lu)", name_u8, GetLastError());
        if (host && host->log) host->log(msg);
        return -1;
    }

    mp_plugin_entry_fn entry;
    /* GetProcAddress retourne FARPROC : passage par copie mémoire pour
       éviter le warning de cast de pointeur de fonction (C11) */
    FARPROC fp = GetProcAddress(dll, MP_PLUGIN_ENTRY);
    memcpy(&entry, &fp, sizeof(entry));
    if (!entry) {
        char msg[512];
        _snprintf(msg, sizeof(msg), "Plugin %s : symbol %s not found, ignored", name_u8, MP_PLUGIN_ENTRY);
        if (host && host->log) host->log(msg);
        FreeLibrary(dll);
        return -1;
    }

    const mp_plugin_api* api = entry();
    if (!api || api->api_version != MP_PLUGIN_API_VERSION || !api->name || !api->name()) {
        char msg[512];
        _snprintf(msg, sizeof(msg), "Plugin %s : incompatible API version (expected %d), ignored",
                  name_u8, MP_PLUGIN_API_VERSION);
        if (host && host->log) host->log(msg);
        FreeLibrary(dll);
        return -1;
    }

    mp_plugin* p = &g_plugins[g_count];
    memset(p, 0, sizeof(*p));
    p->dll = dll;
    p->api = api;
    wcsncpy(p->path, full, MAX_PATH - 1);
    p->path[MAX_PATH - 1] = L'\0';
    {
        char key[256];
        plugin_key(full, key, sizeof(key));
        p->enabled = cfg_get(key);
    }

    if (api->init && api->init(p, host) != 0) {
        char msg[512];
        _snprintf(msg, sizeof(msg), "Plugin %s : init() failed, disabled", api->name());
        if (host && host->log) host->log(msg);
        p->enabled = 0;
    }

    char msg[512];
    _snprintf(msg, sizeof(msg), "Plugin loaded : %s v%s — %s",
              api->name(), api->version ? api->version() : "?",
              api->description ? api->description() : "");
    if (host && host->log) host->log(msg);

    g_count++;
    return 0;
}

void mp_plugins_scan(const wchar_t* dir, const mp_host_api* host)
{
    /* décharge tout d'abord */
    for (int i = 0; i < g_count; i++) unload(&g_plugins[i]);
    g_count = 0;
    g_host = host;

    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*.dll", dir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (host && host->log) host->log("No plugin found (plugins/ folder empty or missing)");
        return;
    }
    do {
        load_one(dir, fd.cFileName, host);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int mp_plugins_count(void) { return g_count; }

mp_plugin* mp_plugins_get(int i)
{
    if (i < 0 || i >= g_count) return NULL;
    return &g_plugins[i];
}

void mp_plugins_set_enabled(int i, int on)
{
    mp_plugin* p = mp_plugins_get(i);
    if (!p) return;
    p->enabled = on ? 1 : 0;
    char key[256];
    plugin_key(p->path, key, sizeof(key));
    cfg_set(key, p->enabled);
}

/* Diffuse un événement aux plugins SERVICE actifs. */
void mp_plugins_service(int event, void* data)
{
    for (int i = 0; i < g_count; i++) {
        mp_plugin* p = &g_plugins[i];
        if (!p->enabled || !p->api) continue;
        if (!(p->api->type() & MP_PLUGIN_SERVICE)) continue;
        if (p->api->service) p->api->service(p, event, data);
    }
}

/* Titre (métadonnées) d'un fichier : premier plugin SERVICE actif. */
const char* mp_plugins_get_title(const char* path)
{
    return mp_plugins_get_metadata(path, "title");
}

/* Métadonnée d'un fichier : premier plugin SERVICE actif. */
const char* mp_plugins_get_metadata(const char* path, const char* field)
{
    if (!field) return NULL;
    for (int i = 0; i < g_count; i++) {
        mp_plugin* p = &g_plugins[i];
        if (!p->enabled || !p->api) continue;
        if (!(p->api->type() & MP_PLUGIN_SERVICE)) continue;
        if (p->api->get_metadata) {
            const char* v = p->api->get_metadata(p, path, field);
            if (v && v[0]) return v;
        }
    }
    return NULL;
}

/* Applique les peaux des plugins de type SKIN actifs (appelé par l'UI).
 * Aucun skin actif → palette par défaut de l'hôte. */
void mp_plugins_apply_skins(void* hwnd)
{
    int applied = 0;
    for (int i = 0; i < g_count; i++) {
        mp_plugin* p = &g_plugins[i];
        if (!p->enabled || !p->api) continue;
        if (!(p->api->type() & MP_PLUGIN_SKIN)) continue;
        if (p->api->apply_skin) {
            p->api->apply_skin(p, hwnd);
            applied = 1;
        }
    }
    if (!applied && g_host && g_host->skin_set_colors) {
        /* palette par défaut */
        mp_skin_colors def = {
            RGB(255, 255, 255), RGB(30, 30, 30),
            RGB(238, 240, 246), RGB(210, 214, 224),
            RGB(52, 120, 246), RGB(226, 66, 56),
            RGB(230, 126, 34), RGB(110, 118, 136),
            RGB(205, 210, 222), RGB(120, 126, 140),
            RGB(255, 255, 255), RGB(28, 30, 38),
            RGB(92, 98, 116)
        };
        g_host->skin_set_colors(&def);
    }
}

void mp_plugins_audio_process(float* samples, unsigned frames,
                              unsigned channels, unsigned sample_rate)
{
    for (int i = 0; i < g_count; i++) {
        mp_plugin* p = &g_plugins[i];
        if (!p->enabled || !p->api) continue;
        if (!(p->api->type() & MP_PLUGIN_AUDIO_EFFECT)) continue;
        if (p->api->process)
            p->api->process(p, samples, frames, channels, sample_rate);
    }
}

void mp_plugins_audio_frames(const float* samples, unsigned frames,
                             unsigned channels, unsigned sample_rate)
{
    for (int i = 0; i < g_count; i++) {
        mp_plugin* p = &g_plugins[i];
        if (!p->enabled || !p->api) continue;
        if (p->api->audio_frames)
            p->api->audio_frames(p, samples, frames, channels, sample_rate);
    }
}

int mp_plugins_has_visual(void)
{
    for (int i = 0; i < g_count; i++) {
        mp_plugin* p = &g_plugins[i];
        if (!p->enabled || !p->api) continue;
        if (!(p->api->type() & MP_PLUGIN_VISUAL)) continue;
        if (p->api->render) return 1;
    }
    return 0;
}

/* Appelle le hook render() du premier plugin visuel actif (thread UI).
 * Un seul visuel est affiché à la fois : le premier de la liste (on peut
 * choisir lequel en activant/désactivant les plugins via le menu). */
void mp_plugins_visual_render(void* hdc, int width, int height)
{
    for (int i = 0; i < g_count; i++) {
        mp_plugin* p = &g_plugins[i];
        if (!p->enabled || !p->api) continue;
        if (!(p->api->type() & MP_PLUGIN_VISUAL)) continue;
        if (p->api->render) {
            p->api->render(p, hdc, width, height);
            return;
        }
    }
}

void mp_plugins_shutdown(void)
{
    for (int i = 0; i < g_count; i++) unload(&g_plugins[i]);
    g_count = 0;
}
