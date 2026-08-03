/*
 * TeamSpeak Broadcast — plugin SERVICE
 * ====================================
 * Diffuse le flux audio courant vers un périphérique de sortie choisi
 * (ex. "CABLE Input" d'un Virtual Audio Cable), à sélectionner comme
 * microphone dans TeamSpeak 3 pour diffuser la musique sur le serveur.
 *
 * Périphérique choisi automatiquement (dans l'ordre) :
 *   1. le nom exact contenu dans le fichier ts_device.txt placé à côté
 *      de la DLL (une ligne, UTF-8)
 *   2. le premier périphérique contenant "CABLE", "Virtual Audio" ou
 *      "VoiceMeeter"
 *   3. le périphérique de sortie par défaut
 *
 * Activable dans Plugins ▸ Services ▸ TeamSpeak Broadcast.
 */
#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/plugin.h"

static const mp_host_api* g_h = NULL;
static volatile LONG g_running = 0;
static ma_device g_ts_dev;
static char g_cfg_name[256] = "";
static wchar_t g_cfg_path[MAX_PATH];

static void log_line(const char* msg)
{
    if (g_h && g_h->log) g_h->log(msg);
}

static void ts_cb(ma_device* dev, void* out, const void* in, ma_uint32 frames)
{
    (void)dev; (void)in;
    float* dst = (float*)out;
    uint32_t got = g_h->web_read(dst, frames);
    if (got < frames)
        memset(dst + (size_t)got * 2, 0,
               (size_t)(frames - got) * 2 * sizeof(float));
}

/* Choix du périphérique : 1 = ID trouvé (dans *id), 0 = périph. par défaut */
static int pick_device(ma_device_id* id, char* name_out, int name_max)
{
    int found = 0;
    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) return 0;
    ma_device_info* infos = NULL;
    ma_uint32 n = 0;
    if (ma_context_get_devices(&ctx, NULL, NULL, &infos, &n) == MA_SUCCESS) {
        /* 1. nom exact du fichier ts_device.txt */
        if (g_cfg_name[0]) {
            for (ma_uint32 i = 0; i < n && !found; i++) {
                if (!strcmp(infos[i].name, g_cfg_name)) {
                    *id = infos[i].id;
                    strncpy(name_out, infos[i].name, name_max - 1);
                    name_out[name_max - 1] = 0;
                    found = 1;
                }
            }
        }
        /* 2. câble audio virtuel */
        for (ma_uint32 i = 0; i < n && !found; i++) {
            if (strstr(infos[i].name, "CABLE") ||
                strstr(infos[i].name, "Virtual Audio") ||
                strstr(infos[i].name, "VoiceMeeter")) {
                *id = infos[i].id;
                strncpy(name_out, infos[i].name, name_max - 1);
                name_out[name_max - 1] = 0;
                found = 1;
            }
        }
    }
    ma_context_uninit(&ctx);
    return found;
}

static int ts_start(void)
{
    if (g_running) return 0;
    ma_device_id id;
    char name[256] = "";
    int has_id = pick_device(&id, name, sizeof(name));

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = 44100;
    cfg.dataCallback = ts_cb;
    if (has_id) cfg.playback.pDeviceID = &id;
    if (ma_device_init(NULL, &cfg, &g_ts_dev) != MA_SUCCESS) return -1;
    if (ma_device_start(&g_ts_dev) != MA_SUCCESS) {
        ma_device_uninit(&g_ts_dev);
        return -1;
    }
    InterlockedExchange(&g_running, 1);
    {
        char msg[320];
        _snprintf(msg, sizeof(msg), "TeamSpeak: broadcasting to %s",
                  has_id ? name : "(default output device)");
        log_line(msg);
    }
    return 0;
}

static void ts_stop(void)
{
    if (!g_running) return;
    ma_device_uninit(&g_ts_dev);
    InterlockedExchange(&g_running, 0);
}

/* ------------------------------------------------------------------ */
static const char* pl_name(void) { return "TeamSpeak Broadcast"; }
static const char* pl_version(void) { return "1.0"; }
static const char* pl_description(void)
{ return "Sends the audio to an output device (e.g. Virtual Audio Cable) to broadcast music on TeamSpeak 3"; }
static unsigned pl_type(void) { return MP_PLUGIN_SERVICE; }

static int pl_init(mp_plugin* self, const mp_host_api* host)
{
    g_h = host;
    /* fichier ts_device.txt à côté de la DLL : nom du périphérique */
    wcsncpy(g_cfg_path, self->path, MAX_PATH - 1);
    g_cfg_path[MAX_PATH - 1] = 0;
    wchar_t* slash = wcsrchr(g_cfg_path, L'\\');
    if (slash) slash[1] = 0;
    wcscat(g_cfg_path, L"ts_device.txt");
    FILE* f = _wfopen(g_cfg_path, L"r");
    if (f) {
        if (fgets(g_cfg_name, sizeof(g_cfg_name), f)) {
            char* p = strchr(g_cfg_name, '\n');
            if (p) *p = 0;
            p = strchr(g_cfg_name, '\r');
            if (p) *p = 0;
        }
        fclose(f);
    }
    return 0;
}

static void pl_destroy(mp_plugin* self)
{
    (void)self;
    ts_stop();
    g_h = NULL;
}

static void pl_service(mp_plugin* self, int event, void* data)
{
    (void)data;
    if (!g_h) return;
    if (event != MP_SERVICE_WEB_APPLY && event != MP_SERVICE_CLICK) return;
    if (self->enabled) {
        if (!g_running) {
            if (ts_start() != 0)
                log_line("TeamSpeak: no output device available");
        }
    } else {
        ts_stop();
    }
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    pl_name, pl_version, pl_description, pl_type,
    pl_init, pl_destroy,
    NULL, NULL, NULL, NULL,   /* process, audio_frames, render, apply_skin */
    pl_service, NULL          /* service, get_title */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
