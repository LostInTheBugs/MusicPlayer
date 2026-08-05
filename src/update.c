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
#include <time.h>
#include "update.h"

#ifndef MP_VERSION
#define MP_VERSION "0.0.0"
#endif

#define UPDATE_URL L"https://api.github.com/repos/LostInTheBugs/MusicPlayer/releases/latest"
#define DL_URL     L"https://github.com/LostInTheBugs/MusicPlayer/releases/latest"

static char          g_latest[32];
static volatile LONG g_checking = 0;
static int           g_mode = -1;   /* cache : 0 désactivé, 1 auto, 2 manuel, 3 autonome */
static int           g_type = -1;   /* 0 toutes, 1 correctives (-cX) */
static int           g_lag = -1;    /* jours : 0, 1, 7, 30 */

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

static void cfg_save(void)
{
    wchar_t path[MAX_PATH];
    appdata_path(path, MAX_PATH, L"\\upd.txt");
    FILE* f = _wfopen(path, L"wb");
    if (f) {
        fprintf(f, "mode=%d\ntype=%d\nlag=%d\n", g_mode, g_type, g_lag);
        fclose(f);
    }
}

static void cfg_load(void)
{
    wchar_t path[MAX_PATH];
    appdata_path(path, MAX_PATH, L"\\upd.txt");
    FILE* f = _wfopen(path, L"rb");
    g_mode = 1;                    /* défaut : automatique */
    g_type = 0;                    /* défaut : toutes les mises à jour */
    g_lag = 7;                     /* défaut : 1 semaine */
    if (f) {
        char buf[128] = "";
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
        char* p;
        if ((p = strstr(buf, "mode="))) g_mode = atoi(p + 5);
        if ((p = strstr(buf, "type="))) g_type = atoi(p + 5);
        if ((p = strstr(buf, "lag=")))  g_lag  = atoi(p + 4);
        if (!strstr(buf, "mode=")) {
            /* ancien format : un seul caractère '0'/'1'/'2' */
            int c = buf[0];
            g_mode = (c == '0' || c == '2') ? 2 : 1;
        }
    }
}

int mp_update_get_mode(void)
{
    if (g_mode < 0) cfg_load();
    return g_mode;
}

void mp_update_set_mode(int mode)
{
    if (mode < 0 || mode > 3) return;
    g_mode = mode;
    cfg_save();
}

int mp_update_get_type(void)
{
    if (g_type < 0) cfg_load();
    return g_type;
}

void mp_update_set_type(int type)
{
    g_type = type ? 1 : 0;
    cfg_save();
}

int mp_update_get_lag(void)
{
    if (g_lag < 0) cfg_load();
    return g_lag;
}

void mp_update_set_lag(int days)
{
    if (days < 0) days = 0;
    g_lag = days;
    cfg_save();
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
    DWORD to = 60000;
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
            DWORD total = 0;
            rc = 0;
            while (InternetReadFile(url_h, buf, sizeof(buf), &rd) && rd > 0) {
                DWORD wr = 0;
                WriteFile(out, buf, rd, &wr, NULL);
                total += rd;
            }
            CloseHandle(out);
            /* un zip valide fait ~40 Mo : une réponse plus petite est
             * une erreur (404, page HTML…) → on ne déploie pas */
            if (total < (1u << 20)) rc = -1;
            /* le magic "PK" doit ouvrir le fichier : sinon ce n'est pas
             * un zip (page d'erreur, HTML…) */
            if (rc == 0) {
                HANDLE chk = CreateFileW(out_path, GENERIC_READ, 0, NULL,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                         NULL);
                if (chk != INVALID_HANDLE_VALUE) {
                    unsigned char magic[2] = { 0, 0 };
                    DWORD rd = 0;
                    ReadFile(chk, magic, 2, &rd, NULL);
                    CloseHandle(chk);
                    if (magic[0] != 'P' || magic[1] != 'K') rc = -1;
                } else {
                    rc = -1;
                }
            }
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

/* ------------------------------------------------------------------ */
/* Filtres : type (toutes / correctives) et délai (lag)                */
/* ------------------------------------------------------------------ */
static int update_allowed(const char* ver)
{
    /* correctives seulement : la release doit porter un -cX */
    if (mp_update_get_type() == 1) {
        int y, m, r, c;
        if (!parse_ver(ver, &y, &m, &r, &c) || c == 0) return 0;
    }
    return 1;
}

/* "2026-08-04T09:13:13Z" → temps UNIX (UTC) */
static time_t parse_iso8601(const char* s)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec) != 6)
        return 0;
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    return _mkgmtime(&t);
}

static int update_in_lag(const char* json, int lag_days)
{
    if (lag_days <= 0) return 0;
    const char* pa = strstr(json, "\"published_at\":\"");
    if (!pa) return 0;
    time_t rel = parse_iso8601(pa + 16);
    if (rel <= 0) return 0;
    return (time(NULL) - rel) < (time_t)lag_days * 86400;
}

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
            /* 64 Ko : la réponse JSON de l'API GitHub (avec le
             * tag_name en tête) ne doit jamais être tronquée */
            char  buf[65536];
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
                                 !is_skipped(g_latest) &&
                                 update_allowed(g_latest)) ? 1 : 0;
                        /* délai : ne pas signaler une release trop fraîche */
                        if (state == 1 && update_in_lag(buf, mp_update_get_lag()))
                            state = 0;
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

/* ------------------------------------------------------------------ */
/* Mode autonome : télécharge, déploie via un script, relance          */
/* ------------------------------------------------------------------ */
int mp_update_apply_and_restart(void)
{
    wchar_t appdir[MAX_PATH];
    GetModuleFileNameW(NULL, appdir, MAX_PATH);
    wchar_t* slash = wcsrchr(appdir, L'\\');
    if (slash) *slash = 0;

    wchar_t zip_path[MAX_PATH];
    wcscpy(zip_path, appdir);
    wcscat(zip_path, L"\\update.zip");
    if (mp_update_download(mp_update_latest(), zip_path) != 0) return -1;

    /* script : arrête le client ET le moteur (le moteur lancé au login
     * verrouille les DLL des core_plugins et ferait échouer
     * l'extraction), extrait le zip avec tar.exe (intégré à Windows 10+,
     * pas de dépendance PowerShell), vérifie le résultat, relance, puis
     * se supprime. Le résultat est noté dans updater.log (relu au
     * démarrage suivant). */
    wchar_t bat[MAX_PATH];
    wcscpy(bat, appdir);
    wcscat(bat, L"\\updater.bat");
    FILE* f = _wfopen(bat, L"w");
    if (!f) return -1;
    fwprintf(f,
        L"@echo off\r\n"
        L"rem ===== MusicPlayer updater =====\r\n"
        L"taskkill /IM MusicPlayer.exe /F >nul 2>&1\r\n"
        L"taskkill /IM MusicPlayerApp.exe /F >nul 2>&1\r\n"
        L"taskkill /IM musicplayer-core.exe /F >nul 2>&1\r\n"
        L"timeout /t 2 /nobreak >nul\r\n"
        L"cd /d \"%~dp0\"\r\n"
        L"tar -xf update.zip >updater.err 2>&1\r\n"
        L"if errorlevel 1 (\r\n"
        L"  echo FAIL: tar errorlevel %errorlevel% > updater.log\r\n"
        L"  type updater.err >> updater.log\r\n"
        L") else (\r\n"
        L"  if exist MusicPlayer.exe (\r\n"
        L"    setlocal enabledelayedexpansion\r\n"
        L"    set /p VER=<VERSION\r\n"
        L"    echo OK: !VER! > updater.log\r\n"
        L"  ) else (\r\n"
        L"    echo FAIL: MusicPlayer.exe absent apres extraction >> updater.log\r\n"
        L"  )\r\n"
        L")\r\n"
        L"del update.zip >nul 2>&1\r\n"
        L"del updater.err >nul 2>&1\r\n"
        L"start \"\" \"%~dp0MusicPlayer.exe\"\r\n"
        L"del \"%~f0\"\r\n");
    fclose(f);

    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = bat;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return -1;
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return 0;
}
