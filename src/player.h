#ifndef MP_PLAYER_H
#define MP_PLAYER_H

#include <stdint.h>

/*
 * Moteur de lecture MusicPlayer.
 * Décodage MP3/MP4 via FFmpeg (libavformat/libavcodec/libswresample),
 * sortie audio via miniaudio (WASAPI/DirectSound/WinMM).
 */

typedef enum {
    MP_STATE_STOPPED  = 0,  /* fichier chargé ou non, position 0 */
    MP_STATE_PLAYING  = 1,
    MP_STATE_PAUSED   = 2,  /* position conservée */
    MP_STATE_FINISHED = 3   /* fin du fichier atteinte */
} mp_state;

/* Initialise le moteur (périphérique audio + thread de décodage).
 * Retourne 0 si OK (le périphérique peut être absent : mode silencieux). */
int mp_init(void);

/* Libère tout (périphérique, threads, contexte). */
void mp_shutdown(void);

/* Ouvre un fichier mp3/mp4 et démarre la lecture. 0 = OK, -1 = erreur. */
int mp_open(const char* path);

/* Ferme le fichier courant sans fermer le moteur. */
void mp_close(void);

/* Lecture / pause / bascule lecture-pause. */
void mp_play(void);
void mp_pause(void);
void mp_play_pause(void);

/* Stop : arrête la lecture et ramène la position à 0 seconde. */
void mp_stop(void);

/* Déplace la lecture à `seconds` (0 = début). Fonctionne en lecture
 * comme en pause. Sans effet si aucun fichier n'est chargé. */
void mp_seek(double seconds);

/* Volume 0.0 .. 1.0. */
void  mp_set_volume(float v);
float mp_get_volume(void);

/* Vitesse 0.5 .. 2.0 (par pas de 0.5 côté UI). */
void  mp_set_speed(float s);
float mp_get_speed(void);

/* Indique si un périphérique audio a pu être ouvert (mode silencieux sinon). */
int mp_audio_device_ok(void);

mp_state mp_get_state(void);

/* Position et durée en secondes. */
double mp_get_position(void);
double mp_get_duration(void);

/* Chemin du fichier en cours (NULL si aucun). */
const char* mp_get_file_name(void);

/* ------------------------------------------------------------------ */
/* Diffusion web (serveur de contrôle à distance)                      */
/* ------------------------------------------------------------------ */

/* Lit jusqu'à `frames` frames (float stéréo) du flux de diffusion
 * destiné au téléphone. Consomme les données. 0 si rien de disponible. */
uint32_t mp_web_read(float* dst, uint32_t frames);

/* ------------------------------------------------------------------ */
/* Flux de diffusion : plusieurs lecteurs indépendants                 */
/* ------------------------------------------------------------------ */
#define MP_WEB_READERS 4

/* Réserve un curseur de lecture. Renvoie un id (0..MP_WEB_READERS-1)
 * ou -1 si plus de place. Le curseur démarre sur les données les plus
 * récentes. */
int  mp_web_reader_open(void);

/* Libère un curseur (il cesse de retenir les données). */
void mp_web_reader_close(int id);

/* Purge le flux de diffusion (stop / seek / nouveau morceau). */
void mp_web_flush(void);

/* Lit jusqu'à `frames` frames pour le lecteur `id`. Chaque lecteur voit
 * l'intégralité du flux. La mémoire n'est réellement libérée que
 * lorsque TOUS les lecteurs ouverts ont consommé. */
uint32_t mp_web_read_n(int id, float* dst, uint32_t frames);

/* Sortie audio : 0 = cet ordinateur, 1 = téléphone seul, 2 = les deux. */
void mp_set_audio_out(int mode);
int  mp_get_audio_out(void);

/* ------------------------------------------------------------------ */
/* Mode DJ local : platine B (2e décodeur) mixée dans le device        */
/* ------------------------------------------------------------------ */
int      mp_dj_b_open(const char* path);
void     mp_dj_b_close(void);
int      mp_dj_b_active(void);
void     mp_dj_b_pause(void);
int      mp_dj_b_paused(void);
void     mp_dj_b_set_vol(float v);
float    mp_dj_b_get_vol(void);
void     mp_dj_a_set_vol(float v);
float    mp_dj_a_get_vol(void);
void     mp_dj_set_xf(float x);
float    mp_dj_get_xf(void);
uint32_t mp_dj_b_read(float* dst, uint32_t frames);
void     mp_dj_mix_into(float* dst, uint32_t frames);

#endif /* MP_PLAYER_H */
