/* cd.h — lecture de CD audio (CD-DA) via MCI.
 * Les pistes sont numérotées à partir de 1 (convention CD). */
#ifndef MP_CD_H
#define MP_CD_H

/* Ouvre le lecteur CD. Retourne 1 si un CD est présent, 0 sinon. */
int  cd_open(void);
/* Nombre de pistes audio du disque (0 si aucun). */
int  cd_track_count(void);
/* Joue la piste n (1-based). */
void cd_play(int track);
void cd_pause(void);
void cd_resume(void);
void cd_stop(void);
void cd_close(void);
/* 1 si une piste est en cours de lecture. */
int  cd_playing(void);
/* 1 si en pause. */
int  cd_paused(void);
/* Position courante de la piste (secondes). */
int  cd_position(void);
/* Durée de la piste n (secondes). */
int  cd_track_length(int track);
/* Piste en cours (1-based, 0 si aucune). */
int  cd_current_track(void);

#endif /* MP_CD_H */
