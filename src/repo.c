/* src/repo.c — plugin repository browser (client).
 *
 * Fetches a plugins.json index over HTTP (WinINet) and downloads the
 * selected DLL/PNG files into the local plugins/ skins/ core_plugins/
 * folders. The default repository is the project's GitHub raw tree. */

#include <windows.h>
#include <wininet.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "repo.h"

/* ------------------------------------------------------------------ */
/* Liste des repositories (persistance)                               */
/* ------------------------------------------------------------------ */
static void repo_appdata_path(wchar_t* out, size_t cap, const wchar_t* file)
{
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, out) == S_OK) {
        wcscat_s(out, cap, L"\\MusicPlayer");
        CreateDirectoryW(out, NULL);
        wcscat_s(out, cap, file);
    } else {
        wcscpy_s(out, cap, file);
    }
}

/* Canal pre-release ? (lu directement dans upd.txt : utilisable par le
 * lanceur et le moteur, qui ne linkent pas update.c) */
int repo_is_pre_channel(void)
{
    wchar_t path[MAX_PATH];
    repo_appdata_path(path, MAX_PATH, L"\\upd.txt");
    FILE* f = _wfopen(path, L"r");
    if (!f) return 0;
    char buf[256];
    int pre = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "channel=1", 9) == 0) { pre = 1; break; }
    }
    fclose(f);
    return pre;
}

const wchar_t* repo_default_base(void)
{
    if (repo_is_pre_channel())
        return REPO_PRE_BASE;
    return REPO_DEFAULT_BASE;
}

int repo_list_load(wchar_t urls[][512], int max)
{
    int n = 0;
    wchar_t path[MAX_PATH];
    repo_appdata_path(path, MAX_PATH, L"\\repos.txt");
    FILE* f = _wfopen(path, L"r");
    if (f) {
        wchar_t line[600];
        while (n < max && fgetws(line, 600, f)) {
            wchar_t* p = line;
            while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;
            int len = (int)wcslen(p);
            while (len > 0 && (p[len - 1] == L'\r' || p[len - 1] == L'\n' ||
                               p[len - 1] == L' '))
                p[--len] = 0;
            if (len > 0) wcscpy(urls[n++], p);
        }
        fclose(f);
    }
    if (n == 0) {
        /* premier démarrage : le repo par défaut du canal courant
         * (pre-release → branche pre-release, sinon master) */
        wcscpy(urls[0], repo_default_base());
        n = 1;
    }
    return n;
}

void repo_list_save(const wchar_t urls[][512], int n)
{
    wchar_t path[MAX_PATH];
    repo_appdata_path(path, MAX_PATH, L"\\repos.txt");
    FILE* f = _wfopen(path, L"w");
    if (f) {
        for (int i = 0; i < n; i++) fwprintf(f, L"%ls\n", urls[i]);
        fclose(f);
    }
}

/* ------------------------------------------------------------------ */
/* HTTP helpers (WinINet, direct connection)                           */
/* ------------------------------------------------------------------ */
static HINTERNET g_rep_inet = NULL;

static int repo_http_get(const wchar_t* url, char** out_body, int* out_len)
{
    if (!g_rep_inet)
        g_rep_inet = InternetOpenW(L"MusicPlayer", INTERNET_OPEN_TYPE_DIRECT,
                                   NULL, NULL, 0);
    if (!g_rep_inet) return -1;

    HINTERNET uh = InternetOpenUrlW(g_rep_inet, url, NULL, 0,
                                    INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!uh) return -1;

    char buf[4096];
    int cap = 8192, len = 0;
    char* body = (char*)malloc(cap);
    if (!body) { InternetCloseHandle(uh); return -1; }
    for (;;) {
        DWORD got = 0;
        if (!InternetReadFile(uh, buf, sizeof(buf), &got) || got == 0) break;
        if (len + (int)got + 1 > cap) {
            cap = (len + (int)got + 1) * 2;
            char* nb = (char*)realloc(body, cap);
            if (!nb) { free(body); InternetCloseHandle(uh); return -1; }
            body = nb;
        }
        memcpy(body + len, buf, got);
        len += (int)got;
    }
    InternetCloseHandle(uh);
    body[len] = 0;
    *out_body = body;
    *out_len = len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Minimal JSON parsing (same style as the engine's core_http)         */
/* ------------------------------------------------------------------ */
static const char* json_find(const char* body, const char* key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(body, pat);
    return p;
}

static void json_str(const char* body, const char* key, char* out, int outsz)
{
    const char* p = json_find(body, key);
    if (!p) { out[0] = 0; return; }
    p = strchr(p, ':');
    if (!p) { out[0] = 0; return; }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') { out[0] = 0; return; }
    p++;
    int n = 0;
    while (*p && *p != '"' && n < outsz - 1) out[n++] = *p++;
    out[n] = 0;
}

/* Extrait le i-ème objet "plugins" du tableau (recherche du i-ème
 * "\"name\""). Retourne le début de l'objet ou NULL. */
static const char* json_plugin_obj(const char* body, int i)
{
    const char* p = body;
    for (int k = 0; k <= i; k++) {
        p = strstr(p, "\"name\"");
        if (!p) return NULL;
        if (k == i) return p;
        p += 6;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
int repo_fetch(const wchar_t* json_url, repo_plugin** out, int* out_n)
{
    *out = NULL;
    *out_n = 0;
    char* body = NULL;
    int len = 0;
    if (repo_http_get(json_url, &body, &len) != 0)
        return -1;
    if (len < 16) { free(body); return -2; }

    repo_plugin* list = (repo_plugin*)calloc(REPO_MAX_PLUGINS, sizeof(repo_plugin));
    int n = 0;
    for (int i = 0; i < REPO_MAX_PLUGINS; i++) {
        const char* obj = json_plugin_obj(body, i);
        if (!obj) break;
        /* chaque champ est cherché DANS l'objet courant : on borne la
         * recherche à la fin de l'objet (prochain "}") */
        const char* end = strchr(obj, '}');
        if (!end) break;
        char tmp[512];
        int olen = (int)(end - obj) + 1;
        if (olen > (int)sizeof(tmp) - 1) olen = (int)sizeof(tmp) - 1;
        memcpy(tmp, obj, olen);
        tmp[olen] = 0;

        repo_plugin* p = &list[n];
        json_str(tmp, "name", p->name, sizeof(p->name));
        json_str(tmp, "type", p->type, sizeof(p->type));
        json_str(tmp, "version", p->version, sizeof(p->version));
        json_str(tmp, "file", p->file, sizeof(p->file));
        json_str(tmp, "desc", p->desc, sizeof(p->desc));
        if (p->name[0] && p->file[0]) n++;
    }
    free(body);
    if (n == 0) { free(list); return -2; }
    *out = list;
    *out_n = n;
    return 0;
}

void repo_free(repo_plugin* list, int n)
{
    (void)n;
    free(list);
}

int repo_download(const wchar_t* base, const repo_plugin* p)
{
    /* URL : base + "/" + file (ex: .../repo/bin/skins/x.dll) */
    wchar_t url[1024];
    wchar_t file_w[256];
    MultiByteToWideChar(CP_UTF8, 0, p->file, -1, file_w, 256);
    swprintf(url, 1024, L"%ls/%ls", base, file_w);

    char* body = NULL;
    int len = 0;
    if (repo_http_get(url, &body, &len) != 0) return -1;
    if (len == 0) { free(body); return -1; }

    /* les DLL/EXE doivent être de vraies images (magic MZ) : un 404
     * (ou une page d'erreur) ne doit JAMAIS remplacer un fichier */
    const char* dot = strrchr(p->file, '.');
    int is_pe = dot && (!strcmp(dot, ".dll") || !strcmp(dot, ".exe"));
    if (is_pe && (len < 1024 || body[0] != 'M' || body[1] != 'Z')) {
        free(body);
        return -1;
    }

    /* destination : <exedir>\ + chemin sans le préfixe "bin/" */
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;

    const wchar_t* rel = file_w;
    if (wcsncmp(rel, L"bin/", 4) == 0) rel += 4;
    if (wcsncmp(rel, L"bin\\", 4) == 0) rel += 4;

    wchar_t dest[MAX_PATH * 2];
    swprintf(dest, MAX_PATH * 2, L"%ls\\%ls", exe, rel);
    /* sous-dossier éventuel (skins/…) */
    wchar_t* last = wcsrchr(dest, L'\\');
    if (last) {
        *last = 0;
        CreateDirectoryW(dest, NULL);
        *last = L'\\';
    }

    HANDLE f = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        /* le fichier existant peut être verrouillé (plugin chargé par
         * le moteur) : on tente de le retirer puis on réessaie */
        DeleteFileW(dest);
        f = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (f == INVALID_HANDLE_VALUE) {
        /* toujours verrouillé (plugin visuel chargé par le client
         * lui-même) : télécharge vers <fichier>.pending, appliqué au
         * prochain démarrage (plugins_apply_pending) */
        wchar_t pend[MAX_PATH * 2];
        swprintf(pend, MAX_PATH * 2, L"%ls.pending", dest);
        f = CreateFileW(pend, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w2 = 0;
            BOOL ok2 = WriteFile(f, body, (DWORD)len, &w2, NULL);
            CloseHandle(f);
            free(body);
            return (ok2 && (int)w2 == len) ? 0 : -1;
        }
        free(body);
        return -1;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(f, body, (DWORD)len, &written, NULL);
    CloseHandle(f);
    free(body);
    if (!(ok && (int)written == len)) return -1;

    /* les archives .zip (runtime : FFmpeg) sont extraites dans le
     * dossier de l'exe (tar.exe natif) puis supprimées */
    if (wcsstr(dest, L".zip")) {
        wchar_t cmd[2048];
        swprintf(cmd, 2048, L"tar -xf \"%ls\" -C \"%ls\"", dest, exe);
        STARTUPINFOW si;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi;
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                           NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 60000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        DeleteFileW(dest);
        /* vérifie que le décodage est opérationnel (avcodec) */
        wchar_t check[MAX_PATH];
        swprintf(check, MAX_PATH, L"%ls\\avcodec-63.dll", exe);
        if (GetFileAttributesW(check) == INVALID_FILE_ATTRIBUTES) return -1;
    }
    return 0;
}
