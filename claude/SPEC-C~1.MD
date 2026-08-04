# Spécification — corriger le son et le serveur web en mode client/serveur

Cible : MusicPlayer `2026.08.040-c3`.

**Appliquer les phases dans l'ordre 1 → 2 → 3, en compilant et testant
entre chaque.** Ne pas commencer la phase 2 avant que le critère
d'acceptation de la phase 1 soit vérifié. Les trois phases se recouvrent :
les mélanger rend le débogage impossible.

Ne rien faire qui ne soit pas écrit ici.

---

## 0. Contexte (ne rien coder à partir de cette section)

La chaîne audio est désormais :

```
décodeur FFmpeg → web_ring (core) → GET /stream → socket TCP
                → ring client (stream_player.c) → carte son du client
```

**La carte son du client est la seule horloge temps réel de la chaîne.**
Tout le reste doit se caler dessus par contre-débit (backpressure) :
quand le client ne consomme plus, le socket se remplit, `send()` bloque
dans le core, `mp_web_read` cesse de consommer, `web_ring_write_bp`
bloque, et le décodeur s'arrête. Aucun timer n'est nécessaire.

Ce contre-débit est cassé, d'où le son buggé. Et le serveur web ne
démarre plus parce que personne ne lui envoie son événement de
démarrage. Ce sont deux pannes indépendantes.

---

# PHASE 1 — rétablir le contre-débit (son)

## 1.1 `src/stream_player.c` — le ring client doit bloquer, pas jeter

C'est le correctif central de la phase 1.

Remplacer **intégralement** la fonction `sp_ring_write` par :

```c
/* Écriture bloquante : si le ring est plein, on attend que le callback
 * audio consomme. C'est CE blocage qui ferme la fenêtre TCP et cale
 * tout le reste de la chaîne sur la carte son. Ne jamais jeter de
 * données ici : jeter ferait tourner le core à vitesse maximale. */
static void sp_ring_write(const float* data, uint32_t frames)
{
    uint32_t pushed = 0;
    while (pushed < frames && !g_stop) {
        LONG h = g_head;   /* seul le producteur écrit head */
        LONG t = __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE);
        uint32_t used = (uint32_t)h - (uint32_t)t;
        uint32_t space = SP_RING_FRAMES - used;
        uint32_t w = frames - pushed;
        if (w > space) w = space;
        if (w == 0) { Sleep(2); continue; }   /* plein : on n'aspire plus */

        uint32_t wpos = (uint32_t)h & SP_RING_MASK;
        uint32_t n1 = w;
        if (n1 > SP_RING_FRAMES - wpos) n1 = SP_RING_FRAMES - wpos;
        memcpy(g_ring + (size_t)wpos * 2, data + (size_t)pushed * 2,
               (size_t)n1 * 2 * sizeof(float));
        if (n1 < w)
            memcpy(g_ring, data + (size_t)(pushed + n1) * 2,
                   (size_t)(w - n1) * 2 * sizeof(float));

        __atomic_store_n(&g_head, (LONG)((uint32_t)h + w), __ATOMIC_RELEASE);
        pushed += w;
    }
}
```

> Rappel de la faute corrigée : l'ancienne version faisait
> `h = h + drop` en cas de saturation, c'est-à-dire qu'elle avançait le
> pointeur d'**écriture** pour « jeter les plus anciennes ». Il aurait
> fallu avancer `g_tail` (le pointeur de lecture). Conséquence :
> `head - tail` dépassait `SP_RING_FRAMES`, et `sp_ring_read` relisait
> des zones déjà réécrites (clics, bruit).

## 1.2 `src/stream_player.c` — borner le curseur « peek » du TeamSpeak

`sp_web_read` n'avance que `g_peek_tail`. Si aucun plugin ne consomme
(cas normal : TeamSpeak désactivé), `g_head` s'éloigne indéfiniment et
`filled` finit par dépasser la capacité du ring → lecture de données
écrasées.

Dans `sp_web_read`, juste après le calcul de `filled`, insérer :

```c
    if (filled > SP_RING_FRAMES) {
        /* lecteur trop en retard : on le recale sur la plus ancienne
         * donnée encore valide */
        t = (LONG)((uint32_t)h - SP_RING_FRAMES);
        g_peek_tail = t;
        filled = SP_RING_FRAMES;
    }
```

## 1.3 `src/stream_player.c` — pré-remplissage avant démarrage du device

Sans pré-remplissage, le premier callback part sur un ring vide et
produit un silence puis un rattrapage.

Dans `sp_start()`, **entre** `CreateThread(...)` et le `return 0`, mais
en déplaçant le démarrage du device après l'attente :

1. Déplacer le bloc `ma_device_start(&g_device)` (et le
   `ma_device_set_master_volume`) **après** la création du thread réseau.
2. Juste avant `ma_device_start`, insérer :

```c
    /* pré-remplissage ~200 ms : évite un sous-remplissage au démarrage */
    for (int i = 0; i < 200 && !g_stop; i++) {
        LONG h = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
        if ((uint32_t)h - (uint32_t)g_tail >= 44100 / 5) break;
        Sleep(5);
    }
```

`ma_device_init` reste à sa place actuelle (il faut `g_dev_rate` avant
que le thread réseau ne resample).

## 1.4 `src/core/core_http.c` — retirer le faux pacing

Ligne ~219, dans `stream_loop`, supprimer :

```c
        Sleep(5);    /* pacing : 46 ms de son par paquet */
```

Ce `Sleep(5)` prétendait cadencer 46 ms de son mais n'attendait que
5 ms : le core poussait ~9× le temps réel. Une fois le 1.1 en place,
c'est le `send()` bloquant qui cadence, et ce `Sleep` ne fait que
retarder inutilement.

Garder en revanche le `if (!n) { Sleep(5); continue; }` : celui-là évite
une attente active quand le ring est vide.

## 1.5 `src/player.c` — le masque empêche de détecter un ring plein

Dans `web_ring_write_bp` **et** dans `mp_web_read`, le nombre d'octets
utilisés est calculé ainsi :

```c
uint32_t used = (uint32_t)(t - h) & WEB_RING_MASK;
```

Quand le ring est exactement plein (`t - h == WEB_RING_FRAMES`), le
masque donne `0`. `web_ring_write_bp` calcule alors
`free = WEB_RING_FRAMES` et écrase des données non lues.

Dans les deux fonctions, remplacer par :

```c
uint32_t used = (uint32_t)t - (uint32_t)h;
```

(sans masque — la soustraction non signée gère le débordement, comme
dans `ring_write`/`ring_read`). Le masque reste utilisé pour calculer
les positions `wpos` / `rpos`, ne pas y toucher.

## Critère d'acceptation — PHASE 1

1. Lancer le client, jouer un MP3 de durée connue : le son est propre,
   sans clic ni saccade, du début à la fin.
2. Le morceau dure **exactement** sa durée réelle (chronométrer un
   fichier de 3 min). Avant correctif il se terminait en quelques
   secondes.
3. La position affichée avance à la vitesse réelle et arrive à la durée
   totale en fin de morceau.
4. Vitesse 0.5× et 2× : le son reste propre, la durée est
   respectivement doublée et divisée par deux.
5. `musicplayer-core.log` ne montre aucune erreur.

**Ne pas passer à la phase 2 tant que ces cinq points ne sont pas
vérifiés.**

---

# PHASE 2 — plusieurs lecteurs sur le flux du core

Aujourd'hui `mp_web_read` est **destructif** et un seul appelant peut
l'utiliser. Or dans le core, deux consommateurs le partagent :
`core_http.c` (`/stream` vers le client) et `webserver.dll`
(`/stream` vers le téléphone). Ils se volent les échantillons.

Tant que le serveur web ne démarre pas (phase 3), le bug est dormant :
il faut donc le corriger **avant** la phase 3.

## 2.1 `src/player.h` — nouvelles fonctions

Après la déclaration de `mp_web_read`, ajouter :

```c
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

/* Lit jusqu'à `frames` frames pour le lecteur `id`. Chaque lecteur voit
 * l'intégralité du flux. La mémoire n'est réellement libérée que
 * lorsque TOUS les lecteurs ouverts ont consommé. */
uint32_t mp_web_read_n(int id, float* dst, uint32_t frames);
```

`mp_web_read(dst, frames)` est conservée : elle devient un alias de
`mp_web_read_n(0, dst, frames)` pour la compatibilité.

## 2.2 `src/player.c` — implémentation

Remplacer la variable `g_web_head` par un tableau de curseurs. Garder
`g_web_tail` comme pointeur d'écriture.

```c
static LONG g_web_rd[MP_WEB_READERS];        /* curseur de chaque lecteur */
static LONG g_web_rd_used[MP_WEB_READERS];   /* 1 = curseur réservé */
static CRITICAL_SECTION g_web_rd_lock;       /* protège l'ouverture/fermeture */
```

Initialiser `g_web_rd_lock` dans `mp_init()` (avec `g_swr_lock`) et la
détruire dans `mp_shutdown()`. Réserver le lecteur 0 dès `mp_init()`
(c'est celui de `core_http.c`, et celui qu'utilise `mp_web_read`).

Fonction utilitaire — le « plancher » de lecture, c'est-à-dire la
position du lecteur le plus en retard :

```c
/* Position en deçà de laquelle plus aucun lecteur n'a besoin des
 * données. S'il n'y a aucun lecteur ouvert, on renvoie l'écriture
 * (rien à conserver). */
static LONG web_read_floor(void)
{
    LONG wr = __atomic_load_n(&g_web_tail, __ATOMIC_ACQUIRE);
    LONG floor = wr;
    int any = 0;
    for (int i = 0; i < MP_WEB_READERS; i++) {
        if (!g_web_rd_used[i]) continue;
        LONG r = __atomic_load_n(&g_web_rd[i], __ATOMIC_ACQUIRE);
        if (!any || (int32_t)(r - floor) < 0) floor = r;
        any = 1;
    }
    return floor;
}
```

`mp_web_reader_open` / `mp_web_reader_close` :

```c
int mp_web_reader_open(void)
{
    EnterCriticalSection(&g_web_rd_lock);
    int id = -1;
    for (int i = 0; i < MP_WEB_READERS; i++) {
        if (!g_web_rd_used[i]) {
            g_web_rd_used[i] = 1;
            /* démarre sur les données les plus récentes */
            __atomic_store_n(&g_web_rd[i],
                             __atomic_load_n(&g_web_tail, __ATOMIC_ACQUIRE),
                             __ATOMIC_RELEASE);
            id = i;
            break;
        }
    }
    LeaveCriticalSection(&g_web_rd_lock);
    return id;
}

void mp_web_reader_close(int id)
{
    if (id < 0 || id >= MP_WEB_READERS) return;
    EnterCriticalSection(&g_web_rd_lock);
    g_web_rd_used[id] = 0;
    LeaveCriticalSection(&g_web_rd_lock);
}
```

`mp_web_read_n` remplace le corps de l'ancienne `mp_web_read` : même
logique de copie, mais sur `g_web_rd[id]` au lieu de `g_web_head`, et
avec recalage du lecteur trop en retard :

```c
uint32_t mp_web_read_n(int id, float* dst, uint32_t frames)
{
    if (id < 0 || id >= MP_WEB_READERS || !g_web_rd_used[id]) return 0;
    LONG wr = __atomic_load_n(&g_web_tail, __ATOMIC_ACQUIRE);
    LONG rd = __atomic_load_n(&g_web_rd[id], __ATOMIC_ACQUIRE);
    uint32_t used = (uint32_t)wr - (uint32_t)rd;
    if (used > WEB_RING_FRAMES) {          /* lecteur décroché : recalage */
        rd = (LONG)((uint32_t)wr - WEB_RING_FRAMES);
        used = WEB_RING_FRAMES;
    }
#ifdef MP_CORE
    if (used == 0) {
        if (g_eof && g_state == MP_STATE_PLAYING)
            InterlockedCompareExchange((LONG*)&g_state, MP_STATE_FINISHED,
                                       MP_STATE_PLAYING);
        return 0;
    }
#else
    if (used == 0) return 0;
#endif
    if (frames > used) frames = used;

    LONG floor_before = web_read_floor();

    uint32_t rpos = (uint32_t)rd & WEB_RING_MASK;
    uint32_t n1 = frames;
    if (n1 > WEB_RING_FRAMES - rpos) n1 = WEB_RING_FRAMES - rpos;
    memcpy(dst, g_web_ring + (size_t)rpos * 2, (size_t)n1 * 2 * sizeof(float));
    if (n1 < frames)
        memcpy(dst + (size_t)n1 * 2, g_web_ring,
               (size_t)(frames - n1) * 2 * sizeof(float));
    __atomic_store_n(&g_web_rd[id], (LONG)((uint32_t)rd + frames),
                     __ATOMIC_RELEASE);

#ifdef MP_CORE
    /* La position du morceau avance quand les données sont
     * DÉFINITIVEMENT consommées, c'est-à-dire quand le lecteur le plus
     * en retard progresse — pas à chaque lecture d'un lecteur donné,
     * sinon deux clients feraient avancer la position deux fois. */
    LONG floor_after = web_read_floor();
    int32_t d = (int32_t)(floor_after - floor_before);
    if (d > 0) {
        float s = g_speed;
        if (s < 0.1f) s = 1.0f;
        LONG64 adv = (LONG64)llround((double)d / (double)s);
        if (adv < 1) adv = 1;
        __atomic_add_fetch(&g_played, adv, __ATOMIC_RELAXED);
    }
#endif
    return frames;
}

uint32_t mp_web_read(float* dst, uint32_t frames)
{
    return mp_web_read_n(0, dst, frames);
}
```

Enfin, dans `web_ring_write_bp`, remplacer le calcul de la place libre
pour qu'il s'appuie sur le plancher au lieu de `g_web_head` :

```c
        LONG wr = g_web_tail;
        LONG fl = web_read_floor();
        uint32_t used = (uint32_t)wr - (uint32_t)fl;
        uint32_t free = WEB_RING_FRAMES - used;
```

(le reste de la fonction est inchangé ; l'écriture se fait toujours à
`wr & WEB_RING_MASK` et le store final sur `g_web_tail`).

Supprimer la variable `g_web_head`, devenue inutilisée.

## 2.3 `src/plugin.h` — exposer les lecteurs aux plugins

Passer la version d'API à 4 :

```c
#define MP_PLUGIN_API_VERSION 4
```

À la **toute fin** de `struct mp_host_api`, après `skin_set_window_size`,
ajouter :

```c
    /* --- flux de diffusion : lecteur dédié (multi-clients) ---
     * Un plugin qui diffuse le flux doit réserver son propre curseur,
     * sinon il vole les échantillons des autres consommateurs.
     * NULL si l'hôte ne le gère pas (tester avant d'appeler). */
    int      (*web_reader_open)(void);
    void     (*web_reader_close)(int id);
    uint32_t (*web_read_n)(int id, float* dst, uint32_t frames);
```

## 2.4 `src/core/core_main.c` — brancher les nouvelles entrées

Dans l'initialiseur `g_host` :

- passer le premier champ `3` à `4` ;
- ajouter en fin de liste, dans l'ordre :
  `mp_web_reader_open, mp_web_reader_close, mp_web_read_n`.

## 2.5 `src/main.c` (client) — brancher les mêmes entrées

Le client n'a pas de ring de diffusion multi-lecteurs : fournir des
stubs qui conservent le comportement actuel.

```c
static int      host_web_reader_open(void) { return 0; }
static void     host_web_reader_close(int id) { (void)id; }
static uint32_t host_web_read_n(int id, float* dst, uint32_t frames)
{ (void)id; return sp_web_read(dst, frames); }
```

Les ajouter en fin d'initialiseur `g_host` et passer la version à `4`.

## 2.6 `src/core/core_http.c` — utiliser un lecteur dédié

Dans `stream_loop`, au début :

```c
    int rid = mp_web_reader_open();
    if (rid < 0) return;
```

Remplacer `mp_web_read(fbuf, 2048)` par `mp_web_read_n(rid, fbuf, 2048)`,
et appeler `mp_web_reader_close(rid);` juste avant les `free(fbuf)` /
`free(obuf)` — **y compris sur le chemin d'erreur d'allocation**.

## 2.7 `examples/plugin_webserver.c` — idem

Dans `stream_thread` (celui de `/stream`, pas les decks DJ), au début :

```c
    int rid = -1;
    if (g_h->web_reader_open) rid = g_h->web_reader_open();
```

Remplacer `g_h->web_read(fbuf, 1024)` par :

```c
        uint32_t n = (rid >= 0 && g_h->web_read_n)
                   ? g_h->web_read_n(rid, fbuf, 1024)
                   : g_h->web_read(fbuf, 1024);
```

Et avant `closesocket(c)` en fin de thread :

```c
    if (rid >= 0 && g_h->web_reader_close) g_h->web_reader_close(rid);
```

Faire exactement la même chose dans `examples/plugin_upnp.c`
(`media_stream`) et `examples/plugin_restapi.c` (`api_stream`).

## Critère d'acceptation — PHASE 2

1. Les cinq points de la phase 1 restent vérifiés.
2. `make && make plugins-examples && make core` : zéro warning.
3. Le son du client reste propre (un seul lecteur actif, comportement
   inchangé).

---

# PHASE 3 — redémarrer le serveur web

## 3.1 `src/core/core_main.c` — envoyer l'événement de démarrage

`plugin_webserver.c` ne démarre rien dans `pl_init` : il attend
`MP_SERVICE_WEB_APPLY`. Le core ne l'envoie jamais.

Dans `WinMain`, remplacer :

```c
    load_plugins();
    core_http_start();
```

par :

```c
    load_plugins();
    /* démarre les services réseau (webserver, upnp, rtp, multiroom) :
     * ils n'ouvrent leurs sockets qu'à la réception de cet événement */
    mp_plugins_service(MP_SERVICE_WEB_APPLY, NULL);
    core_http_start();
```

Ajouter `#include "../plugin_loader.h"` s'il n'y est pas déjà (il y est).

## 3.2 `src/core/core_http.c` — endpoint de configuration

Le core lit `config.yml` **une seule fois** au démarrage. Quand
l'utilisateur change le port ou active le serveur web depuis l'UI du
client, le core ne le voit jamais.

Ajouter un endpoint dans le routeur, à côté des autres `POST` :

```c
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/config")) {
        /* corps : "web_enabled=1&web_port=8000&web_ips=..." */
        const char* body = strstr(req, "\r\n\r\n");
        body = body ? body + 4 : "";
        const char* p;
        if ((p = strstr(body, "web_enabled=")) != NULL)
            g_cfg.web_enabled = atoi(p + 12);
        if ((p = strstr(body, "web_port=")) != NULL)
            g_cfg.web_port = atoi(p + 9);
        if ((p = strstr(body, "web_ips=")) != NULL) {
            p += 8;
            const char* e = strchr(p, '&');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n >= sizeof(g_cfg.web_ips)) n = sizeof(g_cfg.web_ips) - 1;
            memcpy(g_cfg.web_ips, p, n);
            g_cfg.web_ips[n] = 0;
        }
        /* applique à chaud */
        mp_plugins_service(MP_SERVICE_WEB_APPLY, NULL);
        http_response(c, 200, "application/json", "{\"status\":\"ok\"}");
```

## 3.3 `src/client_core.c` — appeler cet endpoint

Ajouter une fonction :

```c
/* Pousse la configuration réseau vers le core (le core ne relit pas
 * config.yml de lui-même). */
void cc_push_web_config(int enabled, int port, const char* ips);
```

Son implémentation reprend le schéma des autres `POST` du fichier
(`HttpOpenRequestA` + `HttpSendRequestA` sur `/api/config`), avec
`Content-Type: application/x-www-form-urlencoded`.

## 3.4 `src/main.c` — appeler `cc_push_web_config`

Aux **quatre** sites qui appellent aujourd'hui
`mp_plugins_service(MP_SERVICE_WEB_APPLY, NULL)` (lignes ~1147, ~1328,
~1457, ~3298), ajouter juste après :

```c
    cc_push_web_config(g_cfg.web_enabled, g_cfg.web_port, g_cfg.web_ips);
```

Conserver l'appel `mp_plugins_service` existant : il reste utile si un
jour le client recharge des plugins de service locaux.

## 3.5 `src/core/core_main.c` — ne pas écraser la config du client

À la fin de `WinMain`, supprimer :

```c
    config_save();
```

Le core est un consommateur de configuration, pas son propriétaire :
c'est le client qui écrit `config.yml`. Laisser les deux processus
écrire le même fichier fait perdre les réglages de l'utilisateur au
hasard de l'ordre d'arrêt.

## Critère d'acceptation — PHASE 3

1. Les critères des phases 1 et 2 restent vérifiés.
2. `musicplayer-core.log` contient `Web server listening on port 8000`
   au démarrage quand `web_enabled: 1` dans `config.yml`.
3. Depuis un téléphone du même réseau, `http://<ip-du-pc>:8000/` affiche
   la télécommande, la playlist et la jaquette.
4. Les boutons de la page web pilotent bien la lecture.
5. **Écoute simultanée** : le son du client reste propre pendant que le
   téléphone lit `/stream`, et le son du téléphone est propre aussi.
   C'est le test qui valide la phase 2.
6. Changer le port dans Settings ▸ Web server… : le serveur redémarre
   sur le nouveau port sans relancer le core.
7. Fermer puis rouvrir le client : les réglages sont conservés.

---

## 4. Hors périmètre (ne pas faire)

- Ne pas toucher au décodeur FFmpeg ni à `mp_open` / `mp_seek` /
  `mp_stop`.
- Ne pas modifier `sp_resample` (le resampling 44 100 → taux du device
  fonctionne).
- Ne pas modifier les skins, l'UI, les visuels ni le mode DJ.
- Ne pas transformer le core en service Windows dans cette passe.
- Ne pas ajouter d'authentification sur l'API REST.
- Ne pas changer le format du flux `/stream` (WAV PCM 16 bits 44,1 kHz).

## 5. Remarque pour plus tard (ne pas coder maintenant)

Dans le build client, `player.c` compile encore tout le chemin de
décodage (`decode_thread`, `g_ring`, `web_ring_write`, `mp_web_read`)
alors que le client ne décode plus : seules les fonctions `mp_dj_*` y
sont utilisées. C'est du code mort qui double les chemins à maintenir et
qui a contribué à la confusion entre les deux rings. À isoler dans un
`player_dj.c` séparé lors d'une passe ultérieure.
