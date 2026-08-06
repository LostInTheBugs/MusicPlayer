/* src/core/core_playlist.c — playlist du moteur (core).
 * Extraite de main.c : mêmes règles (scan récursif, tri, aléatoire,
 * enchaînement) mais sans UI. Protégée par un verrou : les threads
 * HTTP (/api/cmd) et le timer d'enchaînement s'y côtoient. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_playlist.h"
#include "../player.h"
#include "../cd.h"

wchar_t* g_plist[PLAYLIST_MAX];
wchar_t* g_plist_title[PLAYLIST_MAX];   /* titre d'épisode (podcasts) */
int  g_plist_n = 0;
int  g_plist_idx = -1;
int  g_cd_mode = 0;

static int g_shuffle = 0;
static CRITICAL_SECTION g_plist_cs;

/* ------------------------------------------------------------------ */
/* Helpers UTF-8 <-> UTF-16                                            */
/* ------------------------------------------------------------------ */
static void wide_to_utf8(const wchar_t* in, char* out, int out_bytes)
{
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, out_bytes, NULL, NULL);
}

/* ------------------------------------------------------------------ */
void core_plist_init(void)
{
    InitializeCriticalSection(&g_plist_cs);
}

void core_plist_lock(void)   { EnterCriticalSection(&g_plist_cs); }
void core_plist_unlock(void) { LeaveCriticalSection(&g_plist_cs); }

void core_plist_clear(void)
{
    for (int i = 0; i < g_plist_n; i++) {
        free(g_plist[i]);
        free(g_plist_title[i]);
        g_plist_title[i] = NULL;
    }
    g_plist_n = 0;
    g_plist_idx = -1;
}

void core_plist_add(const wchar_t* path)
{
    core_plist_add2(path, NULL);
}

void core_plist_add2(const wchar_t* path, const wchar_t* title)
{
    if (g_plist_n >= PLAYLIST_MAX) return;
    g_plist[g_plist_n] = _wcsdup(path);
    g_plist_title[g_plist_n] = title && title[0]
        ? _wcsdup(title) : NULL;
    if (g_plist[g_plist_n]) g_plist_n++;
    else {
        free(g_plist_title[g_plist_n]);
        g_plist_title[g_plist_n] = NULL;
    }
}

static void playlist_scan(const wchar_t* dir)
{
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                wchar_t sub[MAX_PATH];
                swprintf(sub, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
                playlist_scan(sub);
            }
        } else {
            const wchar_t* e = wcsrchr(fd.cFileName, L'.');
            if (e && (!_wcsicmp(e, L".mp3") || !_wcsicmp(e, L".mp4")) &&
                g_plist_n < PLAYLIST_MAX) {
                size_t cap = wcslen(dir) + wcslen(fd.cFileName) + 2;
                wchar_t* full = (wchar_t*)malloc(cap * sizeof(wchar_t));
                if (full) {
                    swprintf(full, cap, L"%ls\\%ls", dir, fd.cFileName);
                    g_plist[g_plist_n++] = full;
                }
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int playlist_cmp(const void* a, const void* b)
{
    return _wcsicmp(*(const wchar_t* const*)a, *(const wchar_t* const*)b);
}

int core_plist_open_folder(const wchar_t* dir)
{
    core_plist_clear();
    playlist_scan(dir);
    if (g_plist_n == 0) return -1;
    qsort(g_plist, g_plist_n, sizeof(wchar_t*), playlist_cmp);
    return core_plist_play_index(0);
}

int core_plist_play_index(int i)
{
    if (i < 0 || i >= g_plist_n) return -1;
    g_plist_idx = i;
    if (g_cd_mode) {
        /* CD audio : la piste i+1 via MCI */
        cd_play(i + 1);
        return 0;
    }
    char utf8[MAX_PATH * 3];
    wide_to_utf8(g_plist[i], utf8, sizeof(utf8));
    return mp_open(utf8);
}

void core_plist_set_shuffle(int on)
{
    g_shuffle = on ? 1 : 0;
}

int core_plist_get_shuffle(void)
{
    return g_shuffle;
}

void core_plist_next(void)
{
    if (g_plist_n == 0) return;
    if (g_shuffle && g_plist_n > 1) {
        int ni;
        do { ni = rand() % g_plist_n; } while (ni == g_plist_idx);
        if (core_plist_play_index(ni) == 0) return;
    }
    if (g_plist_idx + 1 >= g_plist_n) {
        g_plist_idx = g_plist_n;       /* marque la fin de la playlist */
        mp_stop();
    } else if (core_plist_play_index(g_plist_idx + 1) != 0) {
        g_plist_idx++;                 /* fichier illisible : on saute */
        core_plist_next();
    }
}

void core_plist_tick(void)
{
    if (g_plist_n > 0 && g_plist_idx >= 0 && g_plist_idx < g_plist_n &&
        mp_get_state() == MP_STATE_FINISHED)
        core_plist_next();
}

void core_plist_prev(void)
{
    if (g_plist_n == 0) return;
    int i = g_plist_idx - 1;
    if (i < 0) i = g_plist_n - 1;
    if (core_plist_play_index(i) != 0) {
        g_plist_idx = i;
        core_plist_prev();
    }
}
