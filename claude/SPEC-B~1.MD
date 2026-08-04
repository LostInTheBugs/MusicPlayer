# Spécification — rendre les boutons de transport réactifs (client/serveur)

Cible : MusicPlayer `2026.08.040-c5`.
Fichiers concernés : `src/player.c`, `src/player.h`, `src/stream_player.c`,
`src/stream_player.h`, `src/core/core_http.c`, `src/main.c`.

Appliquer les sections dans l'ordre. Compiler après chaque section.

---

## 0. Cause (ne rien coder à partir de cette section)

Les commandes **partent bien** et le moteur les **exécute bien** : les
noms (`playpause`, `stop`, `next`, `prev`) correspondent des deux côtés.
Le problème est que rien ne purge l'audio déjà décodé, et que l'UI
n'apprend le nouvel état qu'au prochain sondage.

Quand on appuie sur Stop, de l'audio reste en transit à trois endroits :

| Emplacement | Quantité | Durée |
|---|---|---|
| `g_web_ring` du core | 32768 frames | 0,74 s |
| Tampons socket TCP | ~64 Ko | ~0,37 s |
| Ring du client (`stream_player.c`) | 32768 frames | 0,74 s |

Soit **~1,5 à 2 secondes** de son qui continue après l'appui. `mp_stop()`
appelle bien `ring_clear(&g_ring)`, mais sous `MP_CORE` cette fonction
est un no-op, et le ring de diffusion n'est purgé nulle part.

S'ajoutent deux effets d'interface :

- `cc_poll()` n'est appelé que par le timer de 250 ms : l'icône
  lecture/pause bascule avec un retard visible.
- `playpause` est une **bascule**. Sur un état affiché périmé, deux clics
  rapides font deux bascules et ramènent à l'état de départ : l'utilisateur
  a l'impression que le bouton ne répond pas.

---

## 1. `src/player.c` — purger le ring de diffusion

### 1.1 Nouvelle fonction

À placer après `mp_web_reader_close` :

```c
/* Purge le flux de diffusion : tous les lecteurs sont recalés sur la
 * position d'écriture, donc l'audio déjà décodé mais non encore envoyé
 * est abandonné. À appeler sur stop / seek / changement de morceau,
 * sinon les clients continuent de jouer l'ancien son ~1 s. */
void mp_web_flush(void)
{
    EnterCriticalSection(&g_web_rd_lock);
    LONG wr = __atomic_load_n(&g_web_tail, __ATOMIC_ACQUIRE);
    for (int i = 0; i < MP_WEB_READERS; i++) {
        if (!g_web_rd_used[i]) continue;
        __atomic_store_n(&g_web_rd[i], wr, __ATOMIC_RELEASE);
    }
    LeaveCriticalSection(&g_web_rd_lock);
}
```

Déclarer dans `src/player.h`, près de `mp_web_reader_close` :

```c
/* Purge le flux de diffusion (stop / seek / nouveau morceau). */
void mp_web_flush(void);
```

### 1.2 Appeler la purge

**Ordre impératif** : purger **avant** de réinitialiser `g_played`.
`mp_web_read_n` fait avancer `g_played` en fonction de la progression du
plancher de lecture ; purger après l'affectation ferait sauter la
position.

Dans `mp_stop()`, remplacer :

```c
    ring_clear(&g_ring);
    g_eof = 0;
    g_played = 0;
```

par :

```c
    ring_clear(&g_ring);
    mp_web_flush();      /* AVANT g_played : la purge fait bouger le
                            plancher de lecture, donc la position */
    g_eof = 0;
    g_played = 0;
```

Dans `mp_seek()`, remplacer :

```c
    ring_clear(&g_ring);
    g_eof = 0;
    __atomic_store_n(&g_played, (LONG64)(seconds * (double)g_device_rate),
                     __ATOMIC_RELAXED);
```

par :

```c
    ring_clear(&g_ring);
    mp_web_flush();
    g_eof = 0;
    __atomic_store_n(&g_played, (LONG64)(seconds * (double)g_device_rate),
                     __ATOMIC_RELAXED);
```

Dans `mp_open()`, ajouter `mp_web_flush();` juste après le
`ring_clear(&g_ring);` existant (avant `g_played = 0;`).

> Ne **pas** purger sur pause : on veut garder le tampon pour reprendre
> sans trou. Le silence immédiat à la pause est obtenu côté client
> (section 3).

---

## 2. `src/core/core_http.c` — limiter l'audio en transit

Réduire le tampon d'émission de la socket : sans cela, ~64 Ko d'ancien
PCM (≈ 0,37 s) restent en vol même après la purge du ring.

Dans `stream_loop`, juste après `int rid = mp_web_reader_open();` :

```c
    /* petit tampon d'émission : borne l'audio en transit à ~46 ms, pour
     * que stop/seek prennent effet quasi immédiatement chez le client */
    int sndbuf = 8192;
    setsockopt(c, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
```

---

## 3. `src/stream_player.c` — silence immédiat côté client

### 3.1 Demande de purge

Ajouter près des autres globaux :

```c
static volatile LONG g_flush_req = 0;
```

Nouvelle fonction publique (à placer avant `data_cb`) :

```c
/* Demande la purge du ring local. Le vidage est fait PAR le callback
 * audio : lui seul possède g_tail, l'appeler depuis le thread UI
 * corromprait le ring. */
void sp_flush(void)
{
    InterlockedExchange(&g_flush_req, 1);
}
```

Déclarer dans `src/stream_player.h` :

```c
/* Vide le ring local : le son s'arrête immédiatement (stop, pause,
 * changement de morceau). */
void sp_flush(void);
```

### 3.2 Traiter la demande dans le callback

Au **tout début** de `data_cb`, avant `sp_ring_read` :

```c
    if (InterlockedExchange(&g_flush_req, 0)) {
        LONG h = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
        __atomic_store_n(&g_tail, h, __ATOMIC_RELEASE);
        g_peek_tail = h;
    }
```

---

## 4. `src/main.c` — commandes déterministes et état immédiat

### 4.1 Fonction centrale de transport

Ajouter, après les déclarations de `cc_*` (vers le haut du fichier,
avant le premier usage) :

```c
/* Commande de transport : purge le son local pour un effet immédiat,
 * envoie la commande au moteur, puis rafraîchit l'état tout de suite
 * (sans attendre le timer de 250 ms, qui donnait une icône en retard).
 * flush = 0 pour les commandes qui ne doivent pas couper le son. */
static void client_transport(const char* cmd, int flush)
{
    if (flush) sp_flush();
    cc_cmd(cmd);
    cc_poll();          /* état à jour immédiatement */
    status_update();
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}
```

### 4.2 Remplacer la bascule par des commandes explicites

`playpause` est une bascule : sur un état affiché périmé, elle donne le
contraire de ce que l'utilisateur attend. Le moteur accepte déjà `play`
et `pause` explicitement (`core_http.c` lignes 130-133).

Ajouter :

```c
/* Lecture / pause déterministe : on décide d'après l'état connu du
 * moteur plutôt que d'envoyer une bascule. */
static void client_play_pause(void)
{
    if (cc_st() == MP_STATE_PLAYING) client_transport("pause", 1);
    else                             client_transport("play", 0);
}
```

### 4.3 Sites d'appel à modifier

Remplacer **tous** les appels listés ci-dessous. Les numéros de ligne
sont indicatifs : chercher les motifs `cc_cmd("playpause")`,
`cc_cmd("stop")`, `playlist_next()`, `playlist_prev()`.

| Actuel | Remplacer par |
|---|---|
| `cc_cmd("playpause")` | `client_play_pause()` |
| `cc_cmd("stop")` | `client_transport("stop", 1)` |
| `cc_cmd_val("seek", …)` | `sp_flush();` puis l'appel existant, puis `cc_poll();` |

Sites connus de `cc_cmd("playpause")` : ~3054, ~3105, ~3208, ~3364
(touche Espace).
Sites connus de `cc_cmd("stop")` : ~190, ~3056, ~3109, ~3213, ~3365
(touche S).
Site de `seek` : ~ligne 58 de `mouse_down` (clic sur la barre de
progression).

Dans `playlist_next()` et `playlist_prev()` du client, ajouter
`sp_flush();` avant l'envoi de la commande au moteur, et `cc_poll();`
après.

**Exception importante** : les deux appels de `host_play_pause` et
`host_stop` (~lignes 284-285) sont l'API offerte aux plugins. Les faire
pointer sur les nouvelles fonctions :

```c
static void host_play_pause(void) { client_play_pause(); }
static void host_stop(void)       { client_transport("stop", 1); }
```

### 4.4 Ne pas toucher au mode DJ

Les appels `cc_cmd` du bloc `if (g_dj_mode)` dans `mouse_down`
(platines A/B) restent **inchangés** : le mixage DJ a sa propre logique
de tampons.

---

## 5. Compilation

```
make && make core && make plugins-examples
```

Zéro warning exigé par la CI.

---

## 6. Critères d'acceptation

1. **Stop** : le son s'arrête en moins de 150 ms (avant : ~1,5 s).
2. **Pause** : idem, et la reprise repart sans trou ni saut.
3. **Play après Stop** : le morceau repart du début, sans entendre la
   fin du tampon précédent.
4. **Suivant / Précédent** : le morceau change en moins de 200 ms, sans
   résidu de l'ancien.
5. **Seek** (clic sur la barre de progression) : le son saute à la
   nouvelle position sans jouer l'ancienne pendant une seconde.
6. **Icône lecture/pause** : bascule immédiatement au clic, plus de
   retard visible.
7. **Double-clic rapide sur Play** : l'état final correspond au nombre
   de clics (2 clics = retour à l'état initial, mais l'icône suit
   correctement à chaque étape). Plus d'impression de bouton mort.
8. **Espace** et **S** au clavier se comportent comme les boutons.
9. Le son reste propre en lecture continue (non-régression de la
   correction précédente) : un MP3 de 3 min dure 3 min.

---

## 7. Hors périmètre (ne pas faire)

- Ne pas modifier le décodeur, `web_ring_write_bp`, ni les curseurs de
  lecteurs (ils fonctionnent).
- Ne pas modifier `sp_ring_write` / `sp_ring_read` / `sp_resample`.
- Ne pas changer le protocole REST ni le format du flux `/stream`.
- Ne pas rendre `cc_cmd` asynchrone : sur `127.0.0.1` l'aller-retour est
  de l'ordre de la milliseconde, le thread UI n'en souffre pas.
- Ne pas toucher au mode DJ, aux skins, ni aux plugins.

---

## 8. Remarque pour plus tard (ne pas coder maintenant)

Après ce correctif, la latence résiduelle vient du ring client
(0,74 s de tampon de gigue). Elle n'est plus perceptible sur les
commandes puisqu'on purge, mais elle fixe le délai de démarrage.
Si vous voulez un démarrage plus vif, réduire `SP_RING_FRAMES` à
`1 << 14` (0,37 s) et le pré-remplissage à 100 ms — à faire seulement
après avoir vérifié qu'aucun sous-remplissage n'apparaît sur la machine
la plus lente du parc.
