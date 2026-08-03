/* cd.c — lecture de CD audio (CD-DA) via l'API MCI de Windows.
 * MCI reste la façon la plus simple de piloter un lecteur CD-DA
 * (pistes, pause, position) sans pilote matériel spécifique. */
#include <windows.h>
#include <mmsystem.h>
#include "cd.h"

static int g_open = 0;

static void mci_play_cmd(const wchar_t* cmd)
{
    mciSendStringW(cmd, NULL, 0, NULL);
}

int cd_open(void)
{
    if (g_open) return 1;
    if (mciSendStringW(L"open cdaudio shareable", NULL, 0, NULL) != 0)
        return 0;
    g_open = 1;
    wchar_t buf[16];
    if (mciSendStringW(L"status cdaudio media present", buf,
                       (DWORD)(sizeof(buf) / sizeof(wchar_t)), NULL) == 0 &&
        wcsstr(buf, L"true"))
        return 1;
    return 1;   /* lecteur présent (peut être vide) */
}

int cd_track_count(void)
{
    if (!g_open) return 0;
    wchar_t buf[32];
    if (mciSendStringW(L"status cdaudio number of tracks", buf,
                       (DWORD)(sizeof(buf) / sizeof(wchar_t)), NULL) != 0)
        return 0;
    return _wtoi(buf);
}

void cd_play(int track)
{
    if (!g_open || track < 1) return;
    wchar_t cmd[48];
    wsprintfW(cmd, L"play cdaudio from %d", track);
    mci_play_cmd(cmd);
}

void cd_pause(void)
{
    if (!g_open) return;
    mci_play_cmd(L"pause cdaudio");
}

void cd_resume(void)
{
    if (!g_open) return;
    mci_play_cmd(L"play cdaudio");
}

void cd_stop(void)
{
    if (!g_open) return;
    mci_play_cmd(L"stop cdaudio");
}

void cd_close(void)
{
    if (!g_open) return;
    mci_play_cmd(L"close cdaudio");
    g_open = 0;
}

int cd_playing(void)
{
    if (!g_open) return 0;
    wchar_t buf[16];
    if (mciSendStringW(L"status cdaudio mode", buf,
                       (DWORD)(sizeof(buf) / sizeof(wchar_t)), NULL) != 0)
        return 0;
    return wcsstr(buf, L"playing") != NULL;
}

int cd_paused(void)
{
    if (!g_open) return 0;
    wchar_t buf[16];
    if (mciSendStringW(L"status cdaudio mode", buf,
                       (DWORD)(sizeof(buf) / sizeof(wchar_t)), NULL) != 0)
        return 0;
    return wcsstr(buf, L"paused") != NULL;
}

int cd_current_track(void)
{
    if (!g_open) return 0;
    wchar_t buf[16];
    if (mciSendStringW(L"status cdaudio current track", buf,
                       (DWORD)(sizeof(buf) / sizeof(wchar_t)), NULL) != 0)
        return 0;
    return _wtoi(buf);
}

int cd_position(void)
{
    if (!g_open) return 0;
    /* MCI retourne "mm:ss:ff" — on convertit en secondes */
    wchar_t buf[16];
    if (mciSendStringW(L"status cdaudio position", buf,
                       (DWORD)(sizeof(buf) / sizeof(wchar_t)), NULL) != 0)
        return 0;
    const wchar_t* p = wcschr(buf, L':');
    if (!p) return _wtoi(buf);
    return _wtoi(buf) * 60 + _wtoi(p + 1);
}

int cd_track_length(int track)
{
    if (!g_open || track < 1) return 0;
    wchar_t cmd[48], buf[16];
    wsprintfW(cmd, L"status cdaudio length track %d", track);
    if (mciSendStringW(cmd, buf, (DWORD)(sizeof(buf) / sizeof(wchar_t)),
                       NULL) != 0)
        return 0;
    const wchar_t* p = wcschr(buf, L':');
    if (!p) return _wtoi(buf);
    return _wtoi(buf) * 60 + _wtoi(p + 1);
}
