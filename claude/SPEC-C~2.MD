# Correctif — plus aucune lecture depuis la phase 2 (lecteur 0 fantôme)

Cible : MusicPlayer `2026.08.040-c4`.
Un seul fichier à modifier : `src/player.c`.

Ce correctif rattrape une **contradiction dans la spec précédente**
(`SPEC-client-serveur-son-et-webserver.md`) : sa section 2.2 demandait
de réserver le lecteur 0 dans `mp_init()`, et sa section 2.6 demandait
à `core_http.c` d'ouvrir son propre lecteur. Les deux instructions ont
été appliquées, ce qui est incohérent.

---

## 0. Cause de la panne (ne rien coder à partir de cette section)

```
src/player.c:756          mp_web_reader_open();        → réserve le lecteur 0
src/core/core_http.c:192  int rid = mp_web_reader_open();  → obtient le lecteur 1
```

Le lecteur 0 est réservé mais **personne ne l'appelle jamais**. Son
curseur reste figé à sa valeur initiale.

`web_read_floor()` renvoie le minimum des curseurs des lecteurs ouverts,
donc toujours celui du lecteur 0, donc toujours la valeur initiale.
Dans `web_ring_write_bp` :

```
used = wr - floor        → grandit sans jamais redescendre
free = WEB_RING_FRAMES - used → tombe à 0 au bout de 32768 frames
```

À partir de là, `web_ring_write_bp` boucle indéfiniment sur son
`Sleep(5)`. Le thread de décodage est bloqué après ~0,74 s d'audio, et
plus rien n'est produit. D'où « ça ne lit plus du tout ».

Le correctif 1 supprime le lecteur fantôme. Le correctif 2 fait en sorte
qu'un lecteur qui cesse de consommer ne puisse **plus jamais** figer le
moteur — c'est le défaut de conception de fond, et il se reproduirait à
la première connexion `/stream` coupée brutalement (socket mort sans
appel à `mp_web_reader_close`).

---

## 1. Supprimer la réservation du lecteur 0

### 1.1 Dans `mp_init()`

Supprimer cette ligne (actuellement `src/player.c:756`) :

```c
    mp_web_reader_open();   /* lecteur 0 : celui de core_http / mp_web_read */
```

Ne rien mettre à la place. `InitializeCriticalSection(&g_web_rd_lock);`
juste au-dessus doit être **conservé**.

### 1.2 Scinder `mp_web_reader_open` en deux

`mp_web_read` va devoir ouvrir un lecteur alors qu'il tient déjà
`g_web_rd_lock`. Il faut donc une variante sans verrou, sinon on
s'auto-bloque sur la section critique (les `CRITICAL_SECTION` Windows
sont réentrantes pour le même thread, mais on ne s'appuie pas là-dessus :
on sépare proprement).

Remplacer la fonction `mp_web_reader_open` existante par ces **deux**
fonctions :

```c
/* Variante sans verrou : l'appelant doit déjà tenir g_web_rd_lock. */
static int mp_web_reader_open_locked(void)
{
    for (int i = 0; i < MP_WEB_READERS; i++) {
        if (!g_web_rd_used[i]) {
            g_web_rd_used[i] = 1;
            /* démarre sur les données les plus récentes */
            __atomic_store_n(&g_web_rd[i],
                             __atomic_load_n(&g_web_tail, __ATOMIC_ACQUIRE),
                             __ATOMIC_RELEASE);
            return i;
        }
    }
    return -1;
}

int mp_web_reader_open(void)
{
    EnterCriticalSection(&g_web_rd_lock);
    int id = mp_web_reader_open_locked();
    LeaveCriticalSection(&g_web_rd_lock);
    return id;
}
```

`mp_web_reader_close` reste inchangée.

### 1.3 Rendre `mp_web_read` auto-suffisant

`mp_web_read` sert encore de repli pour les plugins qui n'utilisent pas
`web_read_n` (elle est branchée dans la table `g_host` de
`core_main.c`). Elle doit ouvrir son lecteur **à la première
utilisation**, jamais avant.

Remplacer :

```c
uint32_t mp_web_read(float* dst, uint32_t frames)
{
    return mp_web_read_n(0, dst, frames);
}
```

par :

```c
uint32_t mp_web_read(float* dst, uint32_t frames)
{
    /* Lecteur ouvert à la PREMIÈRE utilisation seulement : un lecteur
     * réservé mais jamais lu retiendrait le plancher de lecture et
     * bloquerait le moteur entier (cause de la panne 2026.08.040-c4). */
    static LONG s_id = -1;
    if (__atomic_load_n(&s_id, __ATOMIC_ACQUIRE) < 0) {
        EnterCriticalSection(&g_web_rd_lock);
        if (s_id < 0) {
            int id = mp_web_reader_open_locked();
            __atomic_store_n(&s_id, (LONG)id, __ATOMIC_RELEASE);
        }
        LeaveCriticalSection(&g_web_rd_lock);
    }
    LONG id = __atomic_load_n(&s_id, __ATOMIC_ACQUIRE);
    if (id < 0) return 0;
    return mp_web_read_n((int)id, dst, frames);
}
```

> Placer cette fonction **après** `mp_web_read_n` et après
> `mp_web_reader_open_locked` dans le fichier, sinon il faut des
> déclarations anticipées.

---

## 2. Soupape : un lecteur bloqué ne doit plus figer le moteur

Remplacer **intégralement** la fonction `web_ring_write_bp` par :

```c
#ifdef MP_CORE
/* Variante bloquante (core) : attend que de la place se libère, pour
 * que le décodage avance au rythme des clients qui consomment. */
static uint32_t web_ring_write_bp(const float* data, uint32_t frames)
{
    uint32_t pushed = 0;
    int stalled = 0;
    /* Le décodage doit rester interruptible : sans les tests
     * g_interrupt / g_state, mp_stop() et mp_seek() attendent
     * g_decoding indéfiniment dès que le ring est plein. */
    while (pushed < frames && !g_shutdown && !g_interrupt &&
           g_state == MP_STATE_PLAYING) {
        LONG wr = g_web_tail;
        LONG fl = web_read_floor();
        uint32_t used = (uint32_t)wr - (uint32_t)fl;
        uint32_t free = WEB_RING_FRAMES - used;
        uint32_t w = frames - pushed;
        if (w > free) w = free;
        if (w == 0) {
            if (++stalled > 400) {          /* 400 × 5 ms = 2 s sans progrès */
                /* Un lecteur ne consomme plus (socket mort, client tué…).
                 * On le recale de force plutôt que de bloquer le moteur
                 * pour tout le monde : il perd des données, les autres
                 * continuent. */
                EnterCriticalSection(&g_web_rd_lock);
                for (int i = 0; i < MP_WEB_READERS; i++) {
                    if (!g_web_rd_used[i]) continue;
                    LONG r = __atomic_load_n(&g_web_rd[i], __ATOMIC_ACQUIRE);
                    if ((uint32_t)((uint32_t)wr - (uint32_t)r) >= WEB_RING_FRAMES)
                        __atomic_store_n(&g_web_rd[i],
                            (LONG)((uint32_t)wr - WEB_RING_FRAMES / 2),
                            __ATOMIC_RELEASE);
                }
                LeaveCriticalSection(&g_web_rd_lock);
                stalled = 0;
            }
            Sleep(5);
            continue;
        }
        stalled = 0;
        uint32_t wpos = (uint32_t)wr & WEB_RING_MASK;
        uint32_t n1 = w;
        if (n1 > WEB_RING_FRAMES - wpos) n1 = WEB_RING_FRAMES - wpos;
        memcpy(g_web_ring + (size_t)wpos * 2, data + (size_t)pushed * 2,
               (size_t)n1 * 2 * sizeof(float));
        if (n1 < w)
            memcpy(g_web_ring, data + (size_t)(pushed + n1) * 2,
                   (size_t)(w - n1) * 2 * sizeof(float));
        __atomic_store_n(&g_web_tail, (LONG)((uint32_t)wr + w),
                         __ATOMIC_RELEASE);
        pushed += w;
    }
    return pushed;
}
#endif /* MP_CORE */
```

Points à ne pas rater dans ce remplacement :

- la condition de boucle gagne `!g_interrupt && g_state == MP_STATE_PLAYING` ;
- le compteur `stalled` est remis à 0 dès qu'une écriture réussit ;
- la boucle de recalage prend `g_web_rd_lock` (elle lit `g_web_rd_used`).

---

## 3. Compilation

```
make && make core && make plugins-examples
```

La CI exige **zéro warning**. Vérifier en particulier qu'aucun
avertissement « defined but not used » n'apparaît sur
`mp_web_reader_open_locked` dans le build client (non-`MP_CORE`) : elle
est utilisée par `mp_web_read`, qui est compilée dans les deux builds,
donc il ne devrait pas y en avoir.

---

## 4. Critères d'acceptation

1. Lancer le client, jouer un MP3 de 3 minutes : le son démarre, reste
   propre, et le morceau dure **3 minutes** (chronométrer).
2. La position affichée avance à vitesse réelle et atteint la durée
   totale en fin de morceau.
3. Stop et Seek répondent **immédiatement** pendant la lecture (avant
   correctif, ils pouvaient geler l'UI dès que le ring était plein).
4. Pause puis reprise : pas de blocage, pas de saut.
5. **Test du lecteur mort** : lancer la lecture, tuer `MusicPlayer.exe`
   depuis le gestionnaire des tâches sans fermer le core, puis relancer
   le client. La lecture doit repartir en moins de 2 secondes. Avant
   correctif, le core restait figé définitivement.
6. `musicplayer-core.log` ne contient aucune erreur.

---

## 5. Hors périmètre (ne pas faire)

- Ne pas modifier `src/core/core_http.c` : son
  `mp_web_reader_open()` / `mp_web_read_n(rid, …)` /
  `mp_web_reader_close(rid)` est correct et doit rester tel quel.
- Ne pas modifier `src/stream_player.c` : le contre-débit côté client
  est correct.
- Ne pas modifier `MP_WEB_READERS` (rester à 4).
- Ne pas toucher aux plugins, aux skins, ni à l'UI.
- Ne pas revenir sur la phase 3 (serveur web) : elle est indépendante.
