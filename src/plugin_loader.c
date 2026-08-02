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
        _snprintf(msg, sizeof(msg), "Plugin %s : LoadLibrary a échoué (err=%lu)", name_u8, GetLastError());
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
        _snprintf(msg, sizeof(msg), "Plugin %s : symbole %s introuvable, ignoré", name_u8, MP_PLUGIN_ENTRY);
        if (host && host->log) host->log(msg);
        FreeLibrary(dll);
        return -1;
    }

    const mp_plugin_api* api = entry();
    if (!api || api->api_version != MP_PLUGIN_API_VERSION || !api->name || !api->name()) {
        char msg[512];
        _snprintf(msg, sizeof(msg), "Plugin %s : version d'API incompatible (attendu %d), ignoré",
                  name_u8, MP_PLUGIN_API_VERSION);
        if (host && host->log) host->log(msg);
        FreeLibrary(dll);
        return -1;
    }

    mp_plugin* p = &g_plugins[g_count];
    memset(p, 0, sizeof(*p));
    p->dll = dll;
    p->api = api;
    p->enabled = 1;
    wcsncpy(p->path, full, MAX_PATH - 1);
    p->path[MAX_PATH - 1] = L'\0';

    if (api->init && api->init(p, host) != 0) {
        char msg[512];
        _snprintf(msg, sizeof(msg), "Plugin %s : init() a échoué, désactivé", api->name());
        if (host && host->log) host->log(msg);
        p->enabled = 0;
    }

    char msg[512];
    _snprintf(msg, sizeof(msg), "Plugin chargé : %s v%s — %s",
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
        if (host && host->log) host->log("Aucun plugin trouvé (dossier plugins/ vide ou absent)");
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
}

/* Applique les peaux des plugins de type SKIN actifs (appelé par l'UI). */
void mp_plugins_apply_skins(void* hwnd)
{
    for (int i = 0; i < g_count; i++) {
        mp_plugin* p = &g_plugins[i];
        if (!p->enabled || !p->api) continue;
        if (!(p->api->type() & MP_PLUGIN_SKIN)) continue;
        if (p->api->apply_skin)
            p->api->apply_skin(p, hwnd);
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

void mp_plugins_shutdown(void)
{
    for (int i = 0; i < g_count; i++) unload(&g_plugins[i]);
    g_count = 0;
}
