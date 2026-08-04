/* src/svc.c — démarrage automatique du moteur au login de l'utilisateur.
 *
 * Contrairement à un service Windows (qui exige des droits
 * administrateur pour s'installer), l'autostart utilise la clé
 * HKCU\...\CurrentVersion\Run : AUCUN droit spécial requis. Le moteur
 * (musicplayer-core.exe) se lance au login, affiche une icône dans la
 * zone de notification (clic droit : lancer le client / la page web /
 * quitter), et le client s'y connecte sans le relancer. */

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

#include "svc.h"

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VAL L"MusicPlayerCore"

/* Chemin du moteur : le dossier du client + musicplayer-core.exe */
static void svc_core_path(wchar_t* out, int outsz)
{
    GetModuleFileNameW(NULL, out, outsz);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) wcscpy(slash + 1, L"musicplayer-core.exe");
}

/* 1 = l'autostart est activé (valeur Run présente). */
int svc_install(void)
{
    wchar_t exe[MAX_PATH];
    svc_core_path(exe, MAX_PATH);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k)
        != ERROR_SUCCESS)
        return -1;
    LONG r = RegSetValueExW(k, RUN_VAL, 0, REG_SZ,
                            (const BYTE*)exe,
                            (DWORD)((wcslen(exe) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS ? 0 : -1;
}

int svc_uninstall(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k)
        != ERROR_SUCCESS)
        return -1;
    LONG r = RegDeleteValueW(k, RUN_VAL);
    RegCloseKey(k);
    if (r == ERROR_SUCCESS) return 0;
    if (r == ERROR_FILE_NOT_FOUND) return 1;   /* déjà absent */
    return -1;
}

/* Lance le moteur maintenant (mode normal, avec l'icône de la barre). */
int svc_start(void)
{
    wchar_t exe[MAX_PATH];
    svc_core_path(exe, MAX_PATH);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return -1;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (!CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

/* Arrête le moteur (terminaison du processus musicplayer-core.exe). */
int svc_stop(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int rc = 1;   /* pas trouvé = déjà arrêté */
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"musicplayer-core.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) {
                    if (TerminateProcess(h, 0)) rc = 0;
                    CloseHandle(h);
                } else {
                    rc = -1;
                }
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return rc;
}

int svc_installed(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k)
        != ERROR_SUCCESS)
        return -1;
    LONG r = RegQueryValueExW(k, RUN_VAL, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    if (r == ERROR_SUCCESS) return 1;
    if (r == ERROR_FILE_NOT_FOUND) return 0;
    return -1;
}

int svc_running(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int rc = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"musicplayer-core.exe") == 0) {
                rc = 1;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return rc;
}

void svc_status_text(char* out, int outsz)
{
    int inst = svc_installed();
    int run = svc_running();
    if (inst == 1 && run == 1)
        snprintf(out, outsz, "Autostart : activé — moteur en cours d'exécution");
    else if (inst == 1)
        snprintf(out, outsz, "Autostart : activé — moteur arrêté (lancer au prochain login)");
    else if (inst == 0)
        snprintf(out, outsz, "Autostart : désactivé (le client lance le moteur seul)");
    else if (inst == -1 || run == -1)
        snprintf(out, outsz, "Autostart : erreur de lecture du registre");
}
