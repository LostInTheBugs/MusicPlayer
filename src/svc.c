/* src/svc.c — gestion du service Windows MusicPlayerCore.
 *
 * Le moteur (musicplayer-core.exe) peut tourner comme service Windows
 * (24/7, sans session ouverte) : `musicplayer-core.exe --service`.
 * Ce module installe/démarre/arrête/désinstalle ce service via le
 * Gestionnaire de contrôle des services (SCM). */

#include <windows.h>
#include <winsvc.h>
#include <stdio.h>

#include "svc.h"

/* Chemin du moteur : le dossier du client + musicplayer-core.exe */
static void svc_binpath(wchar_t* out, int outsz)
{
    GetModuleFileNameW(NULL, out, outsz);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) wcscpy(slash + 1, L"musicplayer-core.exe");
    /* binPath : "C:\...\musicplayer-core.exe" --service */
    wchar_t tmp[MAX_PATH * 2];
    swprintf(tmp, MAX_PATH * 2, L"\"%s\" --service", out);
    wcscpy(out, tmp);
}

int svc_install(void)
{
    SC_HANDLE mgr = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!mgr) return -1;
    wchar_t bin[MAX_PATH * 2];
    svc_binpath(bin, MAX_PATH * 2);
    SC_HANDLE s = CreateServiceW(mgr, MP_SVC_NAME, MP_SVC_DISPLAY,
                                 SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                 SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                 bin, NULL, NULL, NULL, NULL, NULL);
    int rc = 0;
    if (!s) {
        DWORD e = GetLastError();
        rc = (e == ERROR_SERVICE_EXISTS) ? 1 : -1;
    } else {
        CloseServiceHandle(s);
    }
    CloseServiceHandle(mgr);
    return rc;
}

int svc_uninstall(void)
{
    SC_HANDLE mgr = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!mgr) return -1;
    SC_HANDLE s = OpenServiceW(mgr, MP_SVC_NAME, DELETE);
    int rc = 0;
    if (!s) {
        rc = (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) ? 1 : -1;
    } else {
        if (!DeleteService(s)) rc = -1;
        CloseServiceHandle(s);
    }
    CloseServiceHandle(mgr);
    return rc;
}

int svc_start(void)
{
    SC_HANDLE mgr = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!mgr) return -1;
    SC_HANDLE s = OpenServiceW(mgr, MP_SVC_NAME, SERVICE_START);
    int rc = 0;
    if (!s) {
        rc = -1;
    } else {
        if (!StartServiceW(s, 0, NULL)) {
            DWORD e = GetLastError();
            rc = (e == ERROR_SERVICE_ALREADY_RUNNING) ? 1 : -1;
        }
        CloseServiceHandle(s);
    }
    CloseServiceHandle(mgr);
    return rc;
}

int svc_stop(void)
{
    SC_HANDLE mgr = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!mgr) return -1;
    SC_HANDLE s = OpenServiceW(mgr, MP_SVC_NAME, SERVICE_STOP);
    int rc = 0;
    if (!s) {
        rc = -1;
    } else {
        SERVICE_STATUS st;
        if (!ControlService(s, SERVICE_CONTROL_STOP, &st)) {
            DWORD e = GetLastError();
            rc = (e == ERROR_SERVICE_NOT_ACTIVE) ? 1 : -1;
        }
        CloseServiceHandle(s);
    }
    CloseServiceHandle(mgr);
    return rc;
}

int svc_installed(void)
{
    SC_HANDLE mgr = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!mgr) return -1;
    SC_HANDLE s = OpenServiceW(mgr, MP_SVC_NAME, SERVICE_QUERY_STATUS);
    int rc = 0;
    if (s) {
        rc = 1;
        CloseServiceHandle(s);
    }
    CloseServiceHandle(mgr);
    return rc;
}

int svc_running(void)
{
    if (svc_installed() != 1) return 0;
    SC_HANDLE mgr = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!mgr) return -1;
    SC_HANDLE s = OpenServiceW(mgr, MP_SVC_NAME, SERVICE_QUERY_STATUS);
    int rc = 0;
    if (s) {
        SERVICE_STATUS st;
        if (QueryServiceStatus(s, &st) && st.dwCurrentState == SERVICE_RUNNING)
            rc = 1;
        CloseServiceHandle(s);
    }
    CloseServiceHandle(mgr);
    return rc;
}

void svc_status_text(char* out, int outsz)
{
    int inst = svc_installed();
    if (inst == 1) {
        int run = svc_running();
        if (run == 1)      snprintf(out, outsz, "Service : en cours d'exécution");
        else if (run == 0) snprintf(out, outsz, "Service : installé, arrêté");
        else               snprintf(out, outsz, "Service : erreur de statut");
    } else if (inst == 0) {
        snprintf(out, outsz, "Service : non installé");
    } else {
        snprintf(out, outsz, "Service : accès refusé (administrateur requis)");
    }
}
