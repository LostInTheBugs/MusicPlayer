/*
 * MusicPlayer.exe — lanceur
 * ==========================
 * Le client (MusicPlayerApp.exe) importe statiquement les DLL FFmpeg :
 * sans elles, le chargeur Windows refuse de démarrer le processus.
 * Ce lanceur (qui n'importe AUCUNE DLL FFmpeg) vérifie la présence du
 * runtime de décodage (plugin de base téléchargeable) et le télécharge
 * depuis le repository par défaut si nécessaire, puis lance le client
 * avec les mêmes arguments. Le code de sortie du client est transmis.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "repo.h"

/* les DLL du runtime FFmpeg (décodage) */
static const char* FFMPEG_RUNTIME_DLLS[] = {
    "avcodec-63.dll", "avformat-63.dll", "avutil-61.dll", "swresample-7.dll"
};

static int ffmpeg_present(void)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    for (int i = 0; i < 4; i++) {
        if (slash) wcscpy(slash + 1, L"");
        if (slash) {
            wchar_t dll[64];
            MultiByteToWideChar(CP_UTF8, 0, FFMPEG_RUNTIME_DLLS[i], -1,
                                dll, 64);
            wcscpy(slash + 1, dll);
        }
        if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return 0;
    }
    return 1;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShow)
{
    (void)hInst;
    (void)hPrev;
    (void)nShow;

    /* runtime FFmpeg : téléchargement + extraction si absent */
    if (!ffmpeg_present()) {
        repo_plugin rp;
        memset(&rp, 0, sizeof(rp));
        strcpy(rp.file, "ffmpeg/ffmpeg-win64-lgpl-shared.zip");
        if (repo_download(REPO_DEFAULT_BASE, &rp) != 0 || !ffmpeg_present()) {
            MessageBoxA(NULL,
                "FFmpeg runtime (decoding engine) is missing and the "
                "automatic download failed.\n\n"
                "Check your internet connection and restart, or download "
                "the zip from the GitHub release page.",
                "MusicPlayer", MB_ICONERROR);
            return 1;
        }
    }

    /* lance le client avec les mêmes arguments */
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) wcscpy(slash + 1, L"MusicPlayerApp.exe");
    wchar_t cmdline[1200];
    swprintf(cmdline, 1200, L"\"%ls\" %hs", exe, lpCmdLine ? lpCmdLine : "");
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmdline, NULL, NULL, FALSE, 0, NULL, NULL,
                        &si, &pi)) {
        MessageBoxA(NULL, "Failed to start MusicPlayer.",
                    "MusicPlayer", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    /* attend la fin du client et transmet son code de sortie */
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}
