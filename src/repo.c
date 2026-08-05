/* src/repo.c — plugin repository browser (client).
 *
 * Fetches a plugins.json index over HTTP (WinINet) and downloads the
 * selected DLL/PNG files into the local plugins/ skins/ core_plugins/
 * folders. The default repository is the project's GitHub raw tree. */

#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "repo.h"

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
    if (f == INVALID_HANDLE_VALUE) { free(body); return -1; }
    DWORD written = 0;
    BOOL ok = WriteFile(f, body, (DWORD)len, &written, NULL);
    CloseHandle(f);
    free(body);
    return (ok && (int)written == len) ? 0 : -1;
}
