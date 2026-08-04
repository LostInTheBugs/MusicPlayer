#ifndef MP_STREAM_PLAYER_H
#define MP_STREAM_PLAYER_H

#include <stdint.h>

/* Lecteur de flux du client : reçoit le PCM du moteur (/stream du
 * musicplayer-core.exe) et le joue sur la carte son locale.
 *
 * Le callback local applique, dans l'ordre :
 *   1. le mix DJ local (platine B, mp_dj_mix_into)
 *   2. le volume du client (sp_set_volume)
 *   3. les effets audio des plugins (equalizer, sound quality…)
 *   4. le flux d'analyse des plugins visuels (audio_frames)
 *
 * TeamSpeak (plugin SERVICE côté client) lit le même flux via
 * sp_web_read (host API web_read). */

/* Démarre le device audio + le thread de réception du flux. */
int sp_start(void);

/* Arrête tout. */
void sp_stop(void);

/* Volume local 0.0 .. 1.0 (appliqué dans le callback). */
void  sp_set_volume(float v);
float sp_get_volume(void);

/* Lit jusqu'à `frames` frames du flux reçu (pour le plugin TS). */
uint32_t sp_web_read(float* dst, uint32_t frames);

#endif /* MP_STREAM_PLAYER_H */
