#ifndef MP_CLIENT_CORE_H
#define MP_CLIENT_CORE_H

/* Pont client → moteur (musicplayer-core.exe, API REST 8080).
 * Le client pilote le moteur par HTTP/JSON et lit son état par polling.
 * En mode défaut, cc_start() lance le moteur s'il ne tourne pas. */

#define CC_PORT 8080   /* défaut ; le moteur lit svc_rest_port */

/* État du moteur (cache, rafraîchi par cc_poll) */
typedef struct {
    int    state;      /* 0 stopped, 1 playing, 2 paused, 3 finished */
    double pos, dur;   /* secondes (temps du morceau) */
    int    idx, count;
    float  speed;
    int    shuffle;
    char   name[512];  /* nom de fichier */
    char   title[512], artist[512], album[512], year[64];
} cc_state_t;

/* Démarre le moteur s'il ne répond pas ; attend qu'il soit prêt
 * (jusqu'à ~10 s). Retourne 0 si le moteur répond. */
int cc_start(void);

/* Arrête le moteur (POST /api/cmd shutdown). */
void cc_stop(void);

/* Envoie une commande sans valeur : play, pause, stop, next, prev… */
void cc_cmd(const char* cmd);

/* Envoie une commande avec valeur : seek, speed, playidx, volume… */
void cc_cmd_val(const char* cmd, double value);

/* Envoie une commande avec chemin : open */
void cc_cmd_path(const char* cmd, const char* path);

/* Rafraîchit le cache d'état (GET /api/state). */
void cc_poll(void);

/* Renvoie le cache (après cc_poll). */
const cc_state_t* cc_state(void);

/* Raccourcis sur le cache. */
static inline int    cc_st(void)     { return cc_state()->state; }
static inline double cc_pos(void)    { return cc_state()->pos; }
static inline double cc_dur(void)    { return cc_state()->dur; }
static inline float  cc_speed(void)  { return cc_state()->speed; }
static inline int    cc_shuffle(void){ return cc_state()->shuffle; }

/* Le moteur répond-il ? (GET /health, rapide) */
int cc_ping(void);

/* Rafraîchit la playlist locale depuis le moteur (GET /api/plist).
 * Remplit g_plist/g_plist_n (globals du client). */
void cc_plist_refresh(void);

/* Chemin complet du morceau courant (depuis la playlist locale). */
const wchar_t* cc_current_path(void);

/* Nom de fichier du morceau courant (sans le dossier). */
const char* cc_name(void);

/* Ouvre un fichier (ou un dossier) via le moteur. Retourne 0. */
int cc_open(const char* path);

#endif /* MP_CLIENT_CORE_H */
