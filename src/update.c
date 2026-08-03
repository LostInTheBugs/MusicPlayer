/* src/update.c — vérification de mises à jour via l'API GitHub Releases.
 *
 * La dernière release est comparée à la version locale (MP_VERSION,
 * format AAAA.MM.NNN). La requête se fait sur un thread détaché :
 * l'interface ne bloque jamais. */
#include <windows.h>
#include <wininet.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "update.h"

#ifndef MP_VERSION
#define MP_VERSION "0.0.0"
#endif

#define UPDATE_URL L"https://api.github.com/repos/LostInTheBugs/MusicPlayer/releases/latest"
#define DL_URL     L"https://github.com/LostInTheBugs/MusicPlayer/releases/latest"

static char          g_latest[32];
static volatile LONG g_checking = 0;

/* ------------------------------------------------------------------ */
/* Comparaison de versions "AAAA.MM.NNN[-cX]"                          */
/* ------------------------------------------------------------------ */
static int parse_ver(const char* s, int* y, int* m, int* r, int* c)
{
    *c = 0;
    if (sscanf(s, "%d.%d.%d-c%d", y, m, r, c) == 4) return 1;
    return sscanf(s, "%d.%d.%d", y, m, r) == 3;
}

/* retourne 1 si `lat` est plus récente que `cur`, sinon 0 */
static int cmp_ver(const char* cur, const char* lat)
{
    int cy, cm, cr, cc, ly, lm, lr, lc;
    if (!parse_ver(cur, &cy, &cm, &cr, &cc) || !parse_ver(lat, &ly, &lm, &lr, &lc))
        return 0;
    if (ly != cy) return ly > cy ? 1 : 0;
    if (lm != cm) return lm > cm ? 1 : 0;
    if (lr != cr) return lr > cr ? 1 : 0;
    return lc > cc ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Préférence « vérifier au démarrage » (fichier %APPDATA%\MusicPlayer) */
/* ------------------------------------------------------------------ */
static void appdata_path(wchar_t* out, size_t cap, const wchar_t* file)
{
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, out) == S_OK) {
        wcscat_s(out, cap, L"\\MusicPlayer");
        CreateDirectoryW(out, NULL);
        wcscat_s(out, cap, file);
    } else {
        wcscpy_s(out, cap, file);
    }
}

int mp_update_get_mode(void)
{
    wchar_t path[MAX_PATH];
    appdata_path(path, MAX_PATH, L"\\upd.txt");
    FILE* f = _wfopen(path, L"rb");
    if (!f) return 1;                 /* défaut : automatique */
    int c = fgetc(f);
    fclose(f);
    if (c == '0' || c == '2') return 2;   /* ancien « non auto » = manuel */
    return 1;
}

void mp_update_set_mode(int mode)
{
    wchar_t path[MAX_PATH];
    appdata_path(path, MAX_PATH, L"\\upd.txt");
    FILE* f = _wfopen(path, L"wb");
    if (f) {
        fputc(mode == 0 ? '0' : (mode == 2 ? '2' : '1'), f);
        fclose(f);
    }
}

int mp_update_auto_enabled(void)
{
    return mp_update_get_mode() == 1;
}

void mp_update_set_auto(int on)
{
    mp_update_set_mode(on ? 1 : 2);
}

/* ------------------------------------------------------------------ */
/* Versions ignorées (skip.txt) : l'utilisateur a refusé une version,  */
/* seules les suivantes seront proposées                               */
/* ------------------------------------------------------------------ */
static int is_skipped(const char* ver)
{
    wchar_t path[MAX_PATH];
    appdata_path(path, MAX_PATH, L"\\skip.txt");
    FILE* f = _wfopen(path, L"r");
    if (!f) return 0;
    char line[256];
    int skip = 0;
    while (fgets(line, sizeof(line), f)) {
        char v[64];
        if (sscanf(line, "%63s", v) == 1 && !strcmp(v, ver)) { skip = 1; break; }
    }
    fclose(f);
    return skip;
}

void mp_update_skip(const char* ver)
{
    wchar_t path[MAX_PATH];
    appdata_path(path, MAX_PATH, L"\\skip.txt");
    FILE* f = _wfopen(path, L"a");
    if (f) {
        fprintf(f, "%s\n", ver);
        fclose(f);
    }
}

/* Télécharge le zip d'une release GitHub vers out_path. */
int mp_update_download(const char* tag, const wchar_t* out_path)
{
    wchar_t url[512];
    swprintf(url, 512,
        L"https://github.com/LostInTheBugs/MusicPlayer/releases/download/"
        L"%hs/MusicPlayer-%hs-win64.zip", tag, tag);
    HINTERNET inet = InternetOpenW(L"MusicPlayer-Updater/1.0",
                                   INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!inet) return -1;
    DWORD to = 30000;
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    HINTERNET url_h = InternetOpenUrlW(inet, url, NULL, 0,
                                       INTERNET_FLAG_RELOAD |
                                       INTERNET_FLAG_NO_CACHE_WRITE, 0);
    int rc = -1;
    if (url_h) {
        HANDLE out = CreateFileW(out_path, GENERIC_WRITE, 0, NULL,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out != INVALID_HANDLE_VALUE) {
            char buf[16384];
            DWORD rd = 0;
            rc = 0;
            while (InternetReadFile(url_h, buf, sizeof(buf), &rd) && rd > 0) {
                DWORD wr = 0;
                WriteFile(out, buf, rd, &wr, NULL);
            }
            CloseHandle(out);
        }
        InternetCloseHandle(url_h);
    }
    InternetCloseHandle(inet);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Thread de vérification                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    HWND hwnd;
    int  manual;
} upd_ctx;

static DWORD WINAPI upd_thread(LPVOID arg)
{
    upd_ctx* ctx = (upd_ctx*)arg;
    HWND hwnd = ctx->hwnd;
    int  manual = ctx->manual;
    int  state = 2;                    /* erreur par défaut */
    g_latest[0] = 0;

    HINTERNET inet = InternetOpenW(L"MusicPlayer-Updater/1.0",
                                   INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (inet) {
        DWORD to = 10000;
        InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
        InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));

        HINTERNET url = InternetOpenUrlW(inet, UPDATE_URL, NULL, 0,
                                         INTERNET_FLAG_RELOAD |
                                         INTERNET_FLAG_NO_CACHE_WRITE |
                                         INTERNET_FLAG_SECURE, 0);
        if (url) {
            char  buf[16384];
            DWORD got = 0, total = 0;
            while (total < (DWORD)sizeof(buf) - 1 &&
                   InternetReadFile(url, buf + total,
                                    (DWORD)sizeof(buf) - 1 - total, &got) && got > 0)
                total += got;
            InternetCloseHandle(url);

            if (total > 0) {
                buf[total] = 0;
                const char* t = strstr(buf, "\"tag_name\":\"");
                if (t) {
                    t += 12;
                    const char* e = strchr(t, '"');
                    if (e && e - t < (int)sizeof(g_latest)) {
                        memcpy(g_latest, t, (size_t)(e - t));
                        g_latest[e - t] = 0;
                        state = (cmp_ver(MP_VERSION, g_latest) &&
                                 !is_skipped(g_latest)) ? 1 : 0;
                    }
                }
            }
        }
        InternetCloseHandle(inet);
    }

    InterlockedExchange(&g_checking, 0);
    PostMessageW(hwnd, MP_UPDATE_DONE, (WPARAM)manual, (LPARAM)state);
    free(ctx);
    return 0;
}

void mp_update_check_async(HWND hwnd, int manual)
{
    if (InterlockedCompareExchange(&g_checking, 1, 0) != 0)
        return;                        /* une vérification est déjà en cours */

    upd_ctx* ctx = (upd_ctx*)malloc(sizeof(*ctx));
    if (!ctx) {
        InterlockedExchange(&g_checking, 0);
        return;
    }
    ctx->hwnd = hwnd;
    ctx->manual = manual;

    HANDLE h = CreateThread(NULL, 0, upd_thread, ctx, 0, NULL);
    if (!h) {
        free(ctx);
        InterlockedExchange(&g_checking, 0);
    } else {
        CloseHandle(h);
    }
}

const char* mp_update_latest(void) { return g_latest; }
