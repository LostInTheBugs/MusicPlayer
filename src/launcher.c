/*
 * MusicPlayer.exe — lanceur
 * ==========================
 * Le client (MusicPlayerApp.exe) importe statiquement les DLL FFmpeg :
 * sans elles, le chargeur Windows refuse de démarrer le processus.
 * Ce lanceur (qui n'importe AUCUNE DLL FFmpeg) vérifie la présence du
 * runtime de décodage (plugin de base téléchargeable) et le télécharge
 * depuis le repository par défaut si nécessaire, puis lance le client
 * avec les mêmes arguments. Le code de sortie du client est transmis.
 *
 * Séquence de téléchargement :
 *   1. zip du runtime (repo/ffmpeg/ffmpeg-win64-lgpl-shared.zip),
 *      extrait avec le tar.exe natif — 3 tentatives ;
 *   2. en cas d'échec : les 4 DLL individuellement (dossier repo/ffmpeg),
 *      écrites directement à côté de l'exe — 2 tentatives chacune ;
 *   3. échec total : message détaillé (dossier attendu + plan B manuel).
 */
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <string.h>

#include "repo.h"

/* les fichiers du runtime (FFmpeg : décodage + ffmpeg.exe pour la
 * transcription ; whisper.cpp : whisper-cli.exe + ggml pour la
 * transcription) */
static const char* FFMPEG_RUNTIME_DLLS[] = {
    "avcodec-63.dll", "avformat-63.dll", "avutil-61.dll",
    "swresample-7.dll", "avdevice-63.dll", "avfilter-12.dll",
    "swscale-10.dll", "ffmpeg.exe",
    "whisper-cli.exe", "ggml.dll", "ggml-base.dll", "whisper.dll",
    "ggml-cpu-alderlake.dll", "ggml-cpu-cannonlake.dll",
    "ggml-cpu-cascadelake.dll", "ggml-cpu-haswell.dll",
    "ggml-cpu-icelake.dll", "ggml-cpu-sandybridge.dll",
    "ggml-cpu-skylakex.dll", "ggml-cpu-sse42.dll", "ggml-cpu-x64.dll"
};
#define FFMPEG_DLL_COUNT 21

static int ffmpeg_present(void)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return 0;
    for (int i = 0; i < FFMPEG_DLL_COUNT; i++) {
        wchar_t dll[64];
        MultiByteToWideChar(CP_UTF8, 0, FFMPEG_RUNTIME_DLLS[i], -1, dll, 64);
        wcscpy(slash + 1, dll);
        if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return 0;
    }
    return 1;
}

/* télécharge url → dest ; retourne 0 si > 100 Ko reçus (pas une erreur) */
static int http_download(const wchar_t* url, const wchar_t* dest)
{
    HINTERNET inet = InternetOpenW(L"MusicPlayer", INTERNET_OPEN_TYPE_DIRECT,
                                   NULL, NULL, 0);
    if (!inet) return -1;
    DWORD to = 60000;
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    HINTERNET uh = InternetOpenUrlW(inet, url, NULL, 0,
                                    INTERNET_FLAG_RELOAD |
                                    INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!uh) { InternetCloseHandle(inet); return -1; }
    HANDLE f = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        InternetCloseHandle(uh);
        InternetCloseHandle(inet);
        return -1;
    }
    char buf[32768];
    DWORD total = 0;
    for (;;) {
        DWORD got = 0;
        if (!InternetReadFile(uh, buf, sizeof(buf), &got) || got == 0) break;
        DWORD wr = 0;
        WriteFile(f, buf, got, &wr, NULL);
        total += got;
    }
    CloseHandle(f);
    InternetCloseHandle(uh);
    InternetCloseHandle(inet);
    return total > 100000 ? 0 : -1;
}

/* extraction zip → dossier exe (tar.exe natif Windows) */
static void extract_zip(const wchar_t* zip, const wchar_t* dir)
{
    wchar_t cmd[2048];
    swprintf(cmd, 2048, L"tar -xf \"%ls\" -C \"%ls\"", zip, dir);
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
}

/* télécharge le runtime ; retourne 0 si les 4 DLL sont en place */
static int ffmpeg_download(void)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;
    wchar_t base[512];
    wcscpy(base, REPO_DEFAULT_BASE);   /* déjà en wchar_t : pas de conversion */

    /* 1) le zip (3 tentatives) puis extraction tar */
    wchar_t url[1024], zpath[MAX_PATH];
    swprintf(url, 1024, L"%ls/ffmpeg/ffmpeg-win64-lgpl-shared.zip", base);
    swprintf(zpath, MAX_PATH, L"%ls\\ffmpeg-runtime.zip", exe);
    int ok = 0;
    for (int i = 0; i < 3 && !ok; i++) {
        ok = http_download(url, zpath) == 0;
        if (!ok) Sleep(1500);
    }
    if (ok) {
        extract_zip(zpath, exe);
        DeleteFileW(zpath);
        if (ffmpeg_present()) return 0;
    }

    /* 2) les 4 DLL individuellement (2 tentatives chacune) */
    for (int i = 0; i < FFMPEG_DLL_COUNT; i++) {
        wchar_t durl[1024], dpath[MAX_PATH];
        swprintf(durl, 1024, L"%ls/ffmpeg/%hs", base, FFMPEG_RUNTIME_DLLS[i]);
        swprintf(dpath, MAX_PATH, L"%ls\\%hs", exe, FFMPEG_RUNTIME_DLLS[i]);
        int done = 0;
        for (int j = 0; j < 2 && !done; j++) {
            done = http_download(durl, dpath) == 0;
            if (!done) Sleep(1000);
        }
        if (!done) return -1;
    }
    return ffmpeg_present() ? 0 : -1;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShow)
{
    (void)hInst;
    (void)hPrev;
    (void)nShow;

    /* runtime FFmpeg : téléchargement si absent */
    if (!ffmpeg_present() && ffmpeg_download() != 0) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) *slash = 0;
        wchar_t msg[1400];
        swprintf(msg, 1400,
            L"FFmpeg runtime (decoding engine) is missing and the "
            L"automatic download failed.\n\n"
            L"Expected folder: %ls\n\n"
            L"Manual fix: download the full zip from\n"
            L"https://github.com/LostInTheBugs/MusicPlayer/releases/latest\n"
            L"and extract its content into this folder, then restart "
            L"MusicPlayer.", exe);
        MessageBoxW(NULL, msg, L"MusicPlayer", MB_ICONERROR);
        return 1;
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
