/*
 * Configuration persistante (config.yml dans %APPDATA%\MusicPlayer).
 * Format YAML simple : "clé: valeur" — une ligne par champ.
 * Migration : l'ancien web.txt est importé si config.yml n'existe pas.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

app_config g_cfg;

/* Répertoire de configuration (%APPDATA%\MusicPlayer).
 * Si %APPDATA% est absent ou trop long : repli sur le dossier de l'exe. */
static void appdata_dir(wchar_t* out, int chars)
{
    out[0] = 0;
    DWORD n = GetEnvironmentVariableW(L"APPDATA", out, (DWORD)chars);
    if (n == 0 || n >= (DWORD)chars) {
        GetModuleFileNameW(NULL, out, (DWORD)chars);
        wchar_t* slash = wcsrchr(out, L'\\');
        if (slash) *slash = 0;
    }
    wcscat(out, L"\\MusicPlayer");
    CreateDirectoryW(out, NULL);
}

static void config_path(wchar_t* out, int chars)
{
    appdata_dir(out, chars);
    wcscat(out, L"\\config.yml");
}

void config_load(void)
{
    /* valeurs par défaut */
    g_cfg.volume = 80;
    g_cfg.speed = 1.0f;
    g_cfg.last_path[0] = 0;
    g_cfg.last_file[0] = 0;
    g_cfg.web_enabled = 0;
    g_cfg.web_port = 0;
    g_cfg.web_audio = 0;
    g_cfg.web_ips[0] = 0;
    g_cfg.shuffle = 0;
    g_cfg.fs_screens = 0;
    g_cfg.fs_mode1 = g_cfg.fs_mode2 = g_cfg.fs_mode3 = 0;
    g_cfg.svc_rest_port = 0; g_cfg.svc_rest_ips[0] = 0;
    g_cfg.svc_upnp_port = 0; g_cfg.svc_upnp_ips[0] = 0;
    g_cfg.svc_rtp_port = 0;  g_cfg.svc_rtp_ips[0] = 0;
    g_cfg.svc_mr_port = 0;   g_cfg.svc_mr_ips[0] = 0;
    g_cfg.log_level = 2;                 /* info par défaut */

    wchar_t path[MAX_PATH];
    config_path(path, MAX_PATH);
    FILE* f = _wfopen(path, L"r");
    if (f) {
        char line[2048];
        while (fgets(line, sizeof(line), f)) {
            char key[128], val[1800];
            if (sscanf(line, "%127[^:]: %1799[^\r\n]", key, val) == 2) {
                if (!strcmp(key, "volume")) g_cfg.volume = atoi(val);
                else if (!strcmp(key, "speed")) g_cfg.speed = (float)atof(val);
                else if (!strcmp(key, "last_path"))
                    _snprintf(g_cfg.last_path, sizeof(g_cfg.last_path), "%s", val);
                else if (!strcmp(key, "last_file"))
                    _snprintf(g_cfg.last_file, sizeof(g_cfg.last_file), "%s", val);
                else if (!strcmp(key, "web_enabled")) g_cfg.web_enabled = atoi(val);
                else if (!strcmp(key, "web_port")) g_cfg.web_port = atoi(val);
                else if (!strcmp(key, "web_audio")) g_cfg.web_audio = atoi(val);
                else if (!strcmp(key, "web_ips"))
                    _snprintf(g_cfg.web_ips, sizeof(g_cfg.web_ips), "%s", val);
                else if (!strcmp(key, "shuffle")) g_cfg.shuffle = atoi(val);
                else if (!strcmp(key, "fs_screens")) g_cfg.fs_screens = atoi(val);
                else if (!strcmp(key, "fs_mode1")) g_cfg.fs_mode1 = atoi(val);
                else if (!strcmp(key, "fs_mode2")) g_cfg.fs_mode2 = atoi(val);
                else if (!strcmp(key, "fs_mode3")) g_cfg.fs_mode3 = atoi(val);
                else if (!strcmp(key, "svc_rest_port")) g_cfg.svc_rest_port = atoi(val);
                else if (!strcmp(key, "svc_rest_ips"))
                    _snprintf(g_cfg.svc_rest_ips, sizeof(g_cfg.svc_rest_ips), "%s", val);
                else if (!strcmp(key, "svc_upnp_port")) g_cfg.svc_upnp_port = atoi(val);
                else if (!strcmp(key, "svc_upnp_ips"))
                    _snprintf(g_cfg.svc_upnp_ips, sizeof(g_cfg.svc_upnp_ips), "%s", val);
                else if (!strcmp(key, "svc_rtp_port")) g_cfg.svc_rtp_port = atoi(val);
                else if (!strcmp(key, "svc_rtp_ips"))
                    _snprintf(g_cfg.svc_rtp_ips, sizeof(g_cfg.svc_rtp_ips), "%s", val);
                else if (!strcmp(key, "svc_mr_port")) g_cfg.svc_mr_port = atoi(val);
                else if (!strcmp(key, "log_level")) g_cfg.log_level = atoi(val);
                else if (!strcmp(key, "svc_mr_ips"))
                    _snprintf(g_cfg.svc_mr_ips, sizeof(g_cfg.svc_mr_ips), "%s", val);
            }
        }
        fclose(f);
        return;
    }

    /* migration : l'ancien web.txt (pré-config.yml) */
    wchar_t wpath[MAX_PATH];
    appdata_dir(wpath, MAX_PATH);
    wcscat(wpath, L"\\web.txt");
    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        char buf[512] = { 0 };
        DWORD rd = 0;
        ReadFile(h, buf, sizeof(buf) - 1, &rd, NULL);
        CloseHandle(h);
        buf[rd] = 0;
        int on = 0, port = 0, audio = 0;
        if (sscanf(buf, "on=%d\nport=%d\naudio=%d", &on, &port, &audio) == 3) {
            g_cfg.web_enabled = on;
            g_cfg.web_port = port;
            g_cfg.web_audio = audio;
        }
        char* ipsp = strstr(buf, "ips=");
        if (ipsp) {
            ipsp += 4;
            char* e = strchr(ipsp, '\n');
            if (!e) e = ipsp + strlen(ipsp);
            int l = (int)(e - ipsp);
            if (l > 1023) l = 1023;
            memcpy(g_cfg.web_ips, ipsp, (size_t)l);
            g_cfg.web_ips[l] = 0;
        }
    }
}

void config_save(void)
{
    wchar_t path[MAX_PATH];
    config_path(path, MAX_PATH);
    FILE* f = _wfopen(path, L"w");
    if (!f) return;
    fprintf(f, "# MusicPlayer configuration\n");
    fprintf(f, "volume: %d\n", g_cfg.volume);
    fprintf(f, "speed: %.2f\n", (double)g_cfg.speed);
    fprintf(f, "last_path: %s\n", g_cfg.last_path);
    fprintf(f, "last_file: %s\n", g_cfg.last_file);
    fprintf(f, "web_enabled: %d\n", g_cfg.web_enabled);
    fprintf(f, "web_port: %d\n", g_cfg.web_port);
    fprintf(f, "web_audio: %d\n", g_cfg.web_audio);
    fprintf(f, "web_ips: %s\n", g_cfg.web_ips);
    fprintf(f, "shuffle: %d\n", g_cfg.shuffle);
    fprintf(f, "fs_screens: %d\n", g_cfg.fs_screens);
    fprintf(f, "fs_mode1: %d\n", g_cfg.fs_mode1);
    fprintf(f, "fs_mode2: %d\n", g_cfg.fs_mode2);
    fprintf(f, "fs_mode3: %d\n", g_cfg.fs_mode3);
    fprintf(f, "svc_rest_port: %d\n", g_cfg.svc_rest_port);
    fprintf(f, "svc_rest_ips: %s\n", g_cfg.svc_rest_ips);
    fprintf(f, "svc_upnp_port: %d\n", g_cfg.svc_upnp_port);
    fprintf(f, "svc_upnp_ips: %s\n", g_cfg.svc_upnp_ips);
    fprintf(f, "svc_rtp_port: %d\n", g_cfg.svc_rtp_port);
    fprintf(f, "svc_rtp_ips: %s\n", g_cfg.svc_rtp_ips);
    fprintf(f, "svc_mr_port: %d\n", g_cfg.svc_mr_port);
    fprintf(f, "svc_mr_ips: %s\n", g_cfg.svc_mr_ips);
    fprintf(f, "log_level: %d\n", g_cfg.log_level);
    fclose(f);
}
