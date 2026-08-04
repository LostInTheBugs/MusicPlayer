/*
 * http_util.h — helpers HTTP partagés par les plugins SERVICE réseau
 * (webserver, restapi, upnp). Header-only : chaque plugin est une DLL
 * indépendante, l'inclusion est donc gratuite.
 *
 * Mutualise : la lecture robuste de requête (boucle recv + terminaison
 * AVANT strstr + corps Content-Length + timeout), les réponses HTTP,
 * l'en-tête WAV et la conversion float → PCM16 — pour qu'un correctif
 * n'ait plus à être porté sur plusieurs copies.
 */
#ifndef MP_HTTP_UTIL_H
#define MP_HTTP_UTIL_H

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Envoie tout le buffer (boucle sur les envois partiels). */
static inline void http_send_all(SOCKET c, const char* data, int len)
{
    int off = 0;
    while (off < len) {
        int n = send(c, data + off, len - off, 0);
        if (n <= 0) return;
        off += n;
    }
}

/* Lit la requête en boucle jusqu'à la fin des en-têtes, puis le corps
 * annoncé (Content-Length). req est TOUJOURS terminé par \0 avant le
 * moindre strstr (jamais de lecture au-delà des octets reçus).
 * Timeout 5 s : un client muet ne bloque pas le serveur.
 * Retourne la longueur reçue (0 = erreur / connexion fermée). */
static inline int http_read_request(SOCKET c, char* req, int req_size)
{
    DWORD rto = 5000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rto, sizeof(rto));
    int got = 0;
    while (got < req_size - 1) {
        int n = recv(c, req + got, req_size - 1 - got, 0);
        if (n <= 0) break;
        got += n;
        req[got] = 0;                 /* termine avant strstr */
        if (strstr(req, "\r\n\r\n")) break;   /* en-têtes complets */
        if (strstr(req, "\n\n")) break;
    }
    if (got <= 0) return 0;
    /* corps annoncé : l'attendre en entier */
    const char* hdr_end = strstr(req, "\r\n\r\n");
    if (!hdr_end) hdr_end = strstr(req, "\n\n");
    if (hdr_end) {
        const char* cl = strstr(req, "Content-Length:");
        if (cl) {
            int need = atoi(cl + 15);
            int body_off = (int)(hdr_end + (hdr_end[0] == '\r' ? 4 : 2) - req);
            while (got - body_off < need && got < req_size - 1) {
                int n = recv(c, req + got, req_size - 1 - got, 0);
                if (n <= 0) break;
                got += n;
            }
            req[got] = 0;
        }
    }
    return got;
}

/* Réponse HTTP complète avec longueur explicite (pas de strlen sur un
 * buffer potentiellement tronqué). */
static inline void http_response_len(SOCKET c, int code, const char* type,
                                     const char* body, int body_len)
{
    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n",
        code, code == 200 ? "OK" :
              code == 403 ? "Forbidden" : "Not Found",
        type ? type : "application/json", body_len);
    if (hl < 0) hl = 0;
    if (hl >= (int)sizeof(hdr)) hl = (int)sizeof(hdr) - 1;
    http_send_all(c, hdr, hl);
    http_send_all(c, body, body_len);
}

static inline void http_response(SOCKET c, int code, const char* type,
                                 const char* body)
{
    http_response_len(c, code, type, body, (int)strlen(body));
}

/* En-tête WAV 44,1 kHz stéréo 16 bits (44 octets). */
static inline void http_wav_header44k(unsigned char hdr[44])
{
    static const unsigned char w[] = {
        'R','I','F','F', 0xff,0xff,0xff,0x7f, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
        0x44,0xac,0,0, 0x10,0xb1,0x02,0, 4,0, 16,0,
        'd','a','t','a', 0xff,0xff,0xff,0x7f
    };
    memcpy(hdr, w, 44);
}

/* Convertit frames × 2 flottants en PCM 16 bits petit-boutiste.
 * Volume appliqué, signal clampé à [-1, 1]. */
static inline void http_f32_to_s16(const float* in, unsigned char* out,
                                   uint32_t frames, float vol)
{
    for (uint32_t i = 0; i < frames * 2; i++) {
        float v = in[i] * vol;
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        short s16 = (short)(v * 32767.0f);
        out[i * 2] = (unsigned char)(s16 & 0xff);
        out[i * 2 + 1] = (unsigned char)((s16 >> 8) & 0xff);
    }
}

/* Anti-CSRF : le POST doit porter un Content-Type non-simple
 * (application/json) — sinon une requête « simple » pourrait être
 * envoyée par n'importe quel site sans CORS. */
static inline int http_post_is_json(const char* req)
{
    const char* ct = strstr(req, "Content-Type:");
    return ct && strstr(ct, "application/json") != NULL;
}

#endif /* MP_HTTP_UTIL_H */
