#ifndef MP_SERVER_H
#define MP_SERVER_H

/*
 * MusicPlayer — serveur web de contrôle à distance.
 * Page télécommande + API JSON + flux audio /stream pour le téléphone.
 */

#include <windows.h>

/* Démarre le serveur sur `port`. Retourne 0 si OK, -1 si le port est
 * occupé (ou toute autre erreur de socket). */
int server_start(int port, HWND hwnd);

/* Arrête le serveur (threads + sockets). */
void server_stop(void);

int server_is_running(void);

/* Premier port libre à partir de 8000 (port par défaut proposé). */
int server_find_free_port(void);

#endif /* MP_SERVER_H */
