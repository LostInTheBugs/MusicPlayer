# Plugins MusicPlayer

Déposez ici les DLL de plugins : elles sont chargées au démarrage
(menu **Plugins ▸ Recharger** pour recharger sans redémarrer).

## Types de plugins (API v1 — voir src/plugin.h)

| Type | Bit | Description |
|---|---|---|
| Skin | `MP_PLUGIN_SKIN` | personnalise l'apparence de la fenêtre (`apply_skin`) |
| Effet audio | `MP_PLUGIN_AUDIO_EFFECT` | traitement PCM temps réel (`process`) |
| Visuel | `MP_PLUGIN_VISUAL` | rendu dans la zone d'affichage (`render`) |

## Contrat d'une DLL de plugin

1. Exporter `const mp_plugin_api* mp_plugin_entry(void)` (nom `mp_plugin_entry`).
2. `api_version` doit valoir `MP_PLUGIN_API_VERSION` (1).
3. Fournir au minimum `name()` et `type()`.
4. Remplir `init()` pour recevoir l'API hôte (`mp_host_api`) : journalisation,
   état du lecteur, position, durée, volume, vitesse.
5. Hooks optionnels : `process()` (effets), `render()` (visuel),
   `apply_skin()` (skin). Ils ne sont appelés que si `type()` les déclare
   et si le plugin est coché dans le menu Plugins.

### Exemple minimal (effet audio)

```c
#include "plugin.h"
#include <string.h>

static void process(mp_plugin* self, float* s, unsigned frames,
                    unsigned ch, unsigned rate) {
    (void)self; (void)rate;
    /* atténuation de moitié (bout de code) */
    for (unsigned i = 0; i < frames * ch; i++) s[i] *= 0.5f;
}

static const mp_plugin_api api = {
    MP_PLUGIN_API_VERSION,
    "GainDemo", "0.1.0", "Atténue le son de moitié",
    (unsigned[]){ MP_PLUGIN_AUDIO_EFFECT },
    NULL, NULL, process, NULL, NULL
};
```

> Note : `type()` doit retourner `unsigned` — dans un vrai plugin,
> écrivez une fonction `type()` qui retourne le masque, pas un littéral
> de tableau comme ci-dessus (l'exemple est simplifié).

## Compiler un plugin

Avec MinGW (Linux) :

```bash
x86_64-w64-mingw32-gcc -O2 -shared -o mon_plugin.dll mon_plugin.c \
    -I../src -static-libgcc
```

## Journal

Les chargements/erreurs de plugins sont écrits dans `musicplayer.log`
(à côté de l'exe) — utile pour déboguer une DLL qui ne se charge pas.
