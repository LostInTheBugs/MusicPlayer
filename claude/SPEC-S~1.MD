# Spécification — skins « fenêtre entière » (Winamp, Vintage radio)

Cible : MusicPlayer `2026.08.036-c10`. À exécuter littéralement, dans l'ordre.
Ne rien faire qui ne soit pas écrit ici. Ne pas refactorer autre chose.

---

## 0. Diagnostic (contexte — ne rien coder à partir de cette section)

Trois défauts de l'hôte, pas des skins :

1. **`get_center_rect()` sert à deux choses incompatibles.** Elle renvoie
   `g_skin_vis` (la zone du visualiseur) dès qu'un skin l'a définie. Or
   `WM_PAINT` utilise son résultat comme **destination de l'image de fond**
   et comme surface du double buffer. Avec le skin radio
   (`skin_set_visual_rect(36,165,568,80)`), toute l'image 640×300 est
   écrasée dans une bande de 568×80 à y=165, et le reste de la fenêtre
   n'est jamais peint (`WM_ERASEBKGND` renvoie 1).

2. **L'image de fond est étirée** : `GdipDrawImageRectI(g, g_skin_bg, 0, 0, w, h)`
   avec `w`/`h` = taille courante. Les coordonnées des skins sont absolues
   (pixels de l'artwork), donc elles ne tombent juste que si la zone client
   fait exactement la taille de l'image. Rien ne le garantit.

3. **L'état du skin n'est pas réinitialisé entre deux skins.**
   `mp_plugins_apply_skins()` appelle `apply_skin()` du nouveau skin sans
   remettre `g_skin_bg`, `g_skin_vis`, `g_skin_menu_visible`,
   `g_skin_ctrl_top` à leurs valeurs par défaut. Passer de « Vintage radio »
   à « Winamp » conserve la zone visuelle et le menu caché de la radio.

Décision retenue : **taille fixe, rendu 1:1**. La fenêtre prend la taille
native de l'image, n'est plus redimensionnable, la barre d'état est masquée.
Les coordonnées absolues des skins deviennent donc toujours exactes.

---

## 1. `src/plugin.h`

### 1.1 Bump de version

Remplacer :

```c
#define MP_PLUGIN_API_VERSION 2
```

par :

```c
#define MP_PLUGIN_API_VERSION 3
```

### 1.2 Nouveau point d'entrée hôte

Dans `struct mp_host_api`, **à la toute fin de la structure**, après le champ
`get_skin_colors`, ajouter :

```c
    /* --- skins : taille de la fenêtre ---
     * w, h = taille voulue de la zone client, en pixels (typiquement la
     * taille native de l'image de fond). fixed = 1 : la fenêtre n'est plus
     * redimensionnable et la barre d'état est masquée (skin « fenêtre
     * entière »). w <= 0 ou h <= 0 : retour au comportement libre. */
    void (*skin_set_window_size)(int w, int h, int fixed);
```

### 1.3 Signature de `skin_set_layout`

Remplacer la déclaration existante par :

```c
    /* --- skins : disposition de la fenêtre ---
     * menu_visible : 1 = barre de menus en haut (défaut), 0 = cachée
     *   (menu accessible par clic droit).
     * ctrl_top : 1 = boutons de contrôle en haut, 0 = en bas (défaut).
     * status_visible : 1 = barre d'état en bas (défaut), 0 = masquée. */
    void (*skin_set_layout)(int menu_visible, int ctrl_top, int status_visible);
```

---

## 2. `src/main.c`

### 2.1 Nouveaux globaux

À côté de `g_skin_menu_visible` / `g_skin_ctrl_top` (vers la ligne 399),
ajouter :

```c
static int g_skin_status_visible = 1;  /* barre d'état affichée ? */
static int g_skin_win_w = 0;           /* taille client imposée par le skin */
static int g_skin_win_h = 0;
static int g_skin_win_fixed = 0;       /* fenêtre non redimensionnable ? */
```

### 2.2 Séparer les deux rectangles

C'est le correctif central. Remplacer la fonction `get_center_rect` par
**deux** fonctions :

```c
/* Zone de contenu : toute la zone client, moins la barre d'état si
 * elle est visible. C'est la surface de l'image de fond, de la barre de
 * progression et des contrôles. */
static void get_content_rect(HWND hwnd, RECT* rc)
{
    GetClientRect(hwnd, rc);
    if (g_status && IsWindowVisible(g_status)) {
        RECT sr;
        GetWindowRect(g_status, &sr);
        rc->bottom -= (sr.bottom - sr.top);
    }
}

/* Zone du visualiseur, en coordonnées client. Par défaut = zone de
 * contenu ; un skin peut la restreindre (skin_set_visual_rect). */
static void get_visual_rect(HWND hwnd, RECT* rc)
{
    if (g_skin_vis.left >= 0) {
        *rc = g_skin_vis;
        return;
    }
    get_content_rect(hwnd, rc);
}
```

Puis remplacer **tous** les appels existants à `get_center_rect(...)` par
`get_content_rect(...)`, sauf celui qui sert à calculer la zone du
visualiseur dans `paint_center` (traité en 2.3).

Appels concernés (vérifier qu'il n'en reste aucun) :
`WM_PAINT`, `mouse_down`, et tout autre site retourné par
`grep -n get_center_rect src/main.c`.

> Attention : `mouse_down` et `layout_controls` doivent recevoir **le même**
> rectangle que `paint_controls`, sinon les clics ne tombent plus sur les
> boutons dessinés. Les deux utilisent désormais `get_content_rect`.

### 2.3 `WM_PAINT` : fond 1:1 sur toute la zone de contenu

Dans `case WM_PAINT:`, remplacer le bloc de dessin du fond par :

```c
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        get_content_rect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w > 0 && h > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP oldbmp = (HBITMAP)SelectObject(mem, bmp);
            if (g_skin_bg) {
                /* fond du skin dessiné à sa taille native (1:1) : avec
                 * skin_set_window_size la fenêtre fait exactement cette
                 * taille, donc aucune déformation */
                UINT iw = 0, ih = 0;
                GdipGetImageWidth(g_skin_bg, &iw);
                GdipGetImageHeight(g_skin_bg, &ih);
                if (iw == 0 || ih == 0) { iw = (UINT)w; ih = (UINT)h; }
                GpGraphics* g = NULL;
                if (GdipCreateFromHDC(mem, &g) == Ok) {
                    /* si l'image est plus petite que la zone, combler
                     * d'abord avec la couleur de fond du skin */
                    if ((int)iw < w || (int)ih < h) {
                        RECT rf = { 0, 0, w, h };
                        HBRUSH wb = CreateSolidBrush(g_skin.bg);
                        FillRect(mem, &rf, wb);
                        DeleteObject(wb);
                    }
                    GdipDrawImageRectI(g, g_skin_bg, 0, 0, (INT)iw, (INT)ih);
                    GdipDeleteGraphics(g);
                }
            } else {
                RECT rf = { 0, 0, w, h };
                HBRUSH wb = CreateSolidBrush(g_skin.bg);
                FillRect(mem, &rf, wb);
                DeleteObject(wb);
            }
            RECT rc2 = { 0, 0, w, h };
            paint_center(mem, &rc2);
            BitBlt(hdc, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldbmp);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
```

Dans `paint_center(HDC hdc, RECT* rc)`, le paramètre `rc` est désormais la
**zone de contenu** en coordonnées locales du DC mémoire (origine 0,0).
Modifier uniquement le calcul de `vis_rc` (hors plein écran) :

```c
    /* zone du visualiseur : imposée par le skin (coordonnées client
     * converties en coordonnées locales du DC), sinon zone de contenu */
    if (g_skin_vis.left >= 0) {
        RECT cont;
        get_content_rect(g_hwnd, &cont);
        vis_rc.left   = g_skin_vis.left   - cont.left;
        vis_rc.top    = g_skin_vis.top    - cont.top;
        vis_rc.right  = g_skin_vis.right  - cont.left;
        vis_rc.bottom = g_skin_vis.bottom - cont.top;
    }
```

Ne pas toucher au calcul de `bar_rc` ni de `ctrl_rc` : ils restent basés sur
`rc` (zone de contenu), ce qui remet les contrôles en bas (ou en haut) de la
fenêtre au lieu de les entasser dans la zone du visualiseur.

### 2.4 `host_skin_set_visual_rect` : gérer la réinitialisation

Remplacer le corps par :

```c
static void host_skin_set_visual_rect(int x, int y, int w, int h)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0) {
        /* réinitialisation : zone du visualiseur par défaut */
        g_skin_vis.left = g_skin_vis.top = -1;
        g_skin_vis.right = g_skin_vis.bottom = -1;
    } else {
        g_skin_vis.left = x;
        g_skin_vis.top = y;
        g_skin_vis.right = x + w;
        g_skin_vis.bottom = y + h;
    }
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, TRUE);
}
```

### 2.5 `host_skin_set_layout` : troisième paramètre

```c
static void host_skin_set_layout(int menu_visible, int ctrl_top,
                                 int status_visible)
{
    g_skin_menu_visible = menu_visible ? 1 : 0;
    g_skin_ctrl_top = ctrl_top ? 1 : 0;
    g_skin_status_visible = status_visible ? 1 : 0;
    if (g_hwnd) {
        apply_menu_visibility();
        if (g_status && !g_fullscreen)
            ShowWindow(g_status, g_skin_status_visible ? SW_SHOW : SW_HIDE);
        InvalidateRect(g_hwnd, NULL, TRUE);
        status_update();
    }
}
```

### 2.6 Nouvelle fonction `host_skin_set_window_size`

À placer juste après `host_skin_set_layout` :

```c
/* Taille de la zone client imposée par le skin. fixed = 1 : fenêtre non
 * redimensionnable (skin « fenêtre entière », rendu pixel-perfect). */
static void host_skin_set_window_size(int w, int h, int fixed)
{
    g_skin_win_w = w;
    g_skin_win_h = h;
    g_skin_win_fixed = (w > 0 && h > 0 && fixed) ? 1 : 0;
    if (!g_hwnd || g_fullscreen) return;

    LONG style = GetWindowLongW(g_hwnd, GWL_STYLE);
    if (g_skin_win_fixed) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    else                  style |= (WS_THICKFRAME | WS_MAXIMIZEBOX);
    SetWindowLongW(g_hwnd, GWL_STYLE, style);

    if (w > 0 && h > 0) {
        int ch = h;
        if (g_status && g_skin_status_visible) {
            RECT sr;
            GetWindowRect(g_status, &sr);
            ch += (sr.bottom - sr.top);
        }
        RECT wr = { 0, 0, w, ch };
        AdjustWindowRectEx(&wr, style, g_skin_menu_visible ? TRUE : FALSE,
                           GetWindowLongW(g_hwnd, GWL_EXSTYLE));
        SetWindowPos(g_hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    InvalidateRect(g_hwnd, NULL, TRUE);
}
```

### 2.7 `WM_GETMINMAXINFO`

Remplacer le `case` existant par :

```c
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        if (g_skin_win_fixed && !g_fullscreen) {
            /* skin à taille fixe : borne haute = borne basse */
            int ch = g_skin_win_h;
            if (g_status && g_skin_status_visible) {
                RECT sr;
                GetWindowRect(g_status, &sr);
                ch += (sr.bottom - sr.top);
            }
            RECT wr = { 0, 0, g_skin_win_w, ch };
            AdjustWindowRectEx(&wr, GetWindowLongW(hwnd, GWL_STYLE),
                               g_skin_menu_visible ? TRUE : FALSE,
                               GetWindowLongW(hwnd, GWL_EXSTYLE));
            mmi->ptMinTrackSize.x = mmi->ptMaxTrackSize.x = wr.right - wr.left;
            mmi->ptMinTrackSize.y = mmi->ptMaxTrackSize.y = wr.bottom - wr.top;
        } else {
            mmi->ptMinTrackSize.x = 420;
            mmi->ptMinTrackSize.y = 260;
        }
        return 0;
    }
```

### 2.8 Table `g_host`

Dans l'initialiseur de `g_host`, ajouter `host_skin_set_window_size` **en
dernière position**, dans le même ordre que la déclaration de `mp_host_api`.

---

## 3. `src/plugin_loader.c` — réinitialiser l'état du skin

Dans `mp_plugins_apply_skins`, **avant** la boucle `for`, insérer :

```c
    /* remise à zéro : sans cela, un skin hérite des réglages du
     * précédent (zone visuelle, image de fond, menu caché…) */
    if (g_host) {
        if (g_host->skin_set_bg)          g_host->skin_set_bg("");
        if (g_host->skin_set_visual_rect) g_host->skin_set_visual_rect(-1, -1, 0, 0);
        if (g_host->skin_set_layout)      g_host->skin_set_layout(1, 0, 1);
        if (g_host->skin_set_window_size) g_host->skin_set_window_size(0, 0, 0);
    }
```

---

## 4. `examples/plugin_skin_radio.c`

`radio_bg.png` fait **640×300**. Le cadran éclairé (verre + aiguille rouge)
occupe environ x 222→418, y 202→238.

Dans `pl_apply_skin`, remplacer les appels `skin_set_visual_rect` et
`skin_set_layout` par :

```c
    /* fenêtre à la taille native de l'artwork, non redimensionnable */
    if (g_h->skin_set_window_size) g_h->skin_set_window_size(640, 300, 1);
    /* le visualiseur s'anime dans le cadran de la radio */
    if (g_h->skin_set_visual_rect) g_h->skin_set_visual_rect(222, 202, 196, 36);
    /* menu caché (clic droit), contrôles en haut, barre d'état masquée */
    if (g_h->skin_set_layout) g_h->skin_set_layout(0, 1, 0);
```

Ordre impératif : `skin_set_layout` **avant** `skin_set_window_size` si les
deux modifient la taille — sinon appeler `skin_set_window_size` en dernier.
Retenir : **layout d'abord, taille ensuite.**

---

## 5. `examples/plugin_skin_winamp.c`

`winamp_bg.png` fait **640×300** et comporte déjà une mise en page :
bandeau titre « WINAMP » sur les 32 premiers pixels, panneau playlist de
y≈42 à y≈200, et un cadre d'analyseur de spectre en bas, x≈20→620,
y≈207→270.

Ce skin n'appelle actuellement **ni** `skin_set_layout` **ni**
`skin_set_visual_rect` : il hérite du layout par défaut (menu visible,
contrôles en bas) posé sur une image étirée. C'est la raison directe du
« ça ne ressemble pas à Winamp ».

Dans `pl_apply_skin`, après l'appel à `skin_set_colors`, ajouter :

```c
    /* fenêtre à la taille native de l'artwork, non redimensionnable */
    if (g_h->skin_set_window_size) g_h->skin_set_window_size(640, 300, 1);
    /* analyseur de spectre dans le cadre du bas */
    if (g_h->skin_set_visual_rect) g_h->skin_set_visual_rect(30, 212, 580, 54);
    /* menu caché, contrôles dans le bandeau du haut, barre d'état masquée */
    if (g_h->skin_set_layout) g_h->skin_set_layout(0, 1, 0);
```

`CTRL_H` vaut 32 et le bandeau « WINAMP » fait exactement 32 px : la barre de
contrôles se superpose donc pile dessus. **Conséquence à accepter : le
logo « WINAMP » de l'image sera recouvert.** Pour un rendu propre,
régénérer `winamp_bg.png` avec les 32 premiers pixels laissés vides
(dégradé sombre uni), sans texte.

---

## 6. Les neuf autres skins

`skin_retro60/70/80/90`, `skin_2000s`, `skin_clean`, `skin_kitsch`,
`skin_cartoon`, `skin_bnw` sont des skins **de palette uniquement** : ils
n'appellent que `skin_set_colors`. Ne rien y changer. La réinitialisation
de la section 3 leur rend automatiquement le layout par défaut.

---

## 7. Compilation

Aucune modification du `Makefile` n'est nécessaire : `plugin.h` est déjà
inclus via `-Isrc`, et `make plugins-examples` recompile tous les skins,
ce qui aligne `MP_PLUGIN_API_VERSION` à 3 partout.

```
make && make plugins-examples
```

La CI exige **zéro warning**. Vérifier en particulier que
`GdipGetImageWidth` / `GdipGetImageHeight` sont bien déclarés (ils le sont
via `<gdiplus.h>` déjà inclus dans `main.c`) et que `IsWindowVisible` ne
provoque pas de warning de déclaration implicite.

---

## 8. Critères d'acceptation

1. Skin « Vintage radio » activé : la fenêtre se redimensionne seule à
   640×300 de zone client, l'image de la radio remplit exactement la
   fenêtre sans déformation, aucune bande noire.
2. Le spectre s'anime **dans le cadran**, pas sur toute la largeur.
3. Les boutons lecture/stop/suivant sont en haut, cliquables, et le clic
   tombe bien sur le bouton dessiné (vérifier chaque bouton).
4. Le bord de la fenêtre ne se redimensionne plus à la souris ; le bouton
   « agrandir » est grisé.
5. Passer de « Vintage radio » à « Winamp » puis à « Clean » : chaque skin
   repart d'un état propre. En particulier « Clean » retrouve le menu
   visible, la barre d'état, la fenêtre redimensionnable et aucune image
   de fond.
6. F11 (plein écran) fonctionne toujours avec un skin fixe, et revenir du
   plein écran restaure la taille fixe.
7. `make && make plugins-examples` : zéro warning.

---

## 9. Hors périmètre (ne pas faire)

- Ne pas positionner les boutons via le skin (pas de `skin_set_controls_rect`).
- Ne pas implémenter les vrais formats de skin Winamp (`.wsz`, découpe de
  `main.bmp`, `cbuttons.bmp`…).
- Ne pas ajouter de fenêtre à régions (`SetWindowRgn`) ni de bords
  transparents.
- Ne pas toucher au moteur audio, aux plugins réseau, ni au reste de
  `main.c`.
