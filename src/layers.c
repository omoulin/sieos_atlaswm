/*
 * layers.c - Panels, wallpapers, docks, notifications and launchers:
 * wlr-layer-shell surfaces (waybar, swaybg, mako, fuzzel...).
 *
 * Each sits on one screen, in one of four layers of the scene:
 *   background   over the desk's grid (a wallpaper hides it)
 *   bottom       under the windows
 *   top          over the windows and the Deskbar (a panel keeps clear of it,
 *                its room taken; a notification shows over it)
 *   overlay      over everything but the menus
 * One anchored to an edge with an exclusive zone (a panel) takes that
 * room from the screen: the windows, the Deskbar and the rest clock keep
 * clear of it. One that asks for the keys gets them while it is shown (a
 * launcher), or when clicked (on demand). Everything goes through the look,
 * like the rest.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <wlr/types/wlr_layer_shell_v1.h>
#include "atlas.h"

struct layer {
    struct wl_list link;                              /* layers */
    struct wlr_layer_surface_v1 *ls;
    struct wlr_scene_layer_surface_v1 *sl;
    struct output *o;
    int mapped;
    struct wl_listener map, unmap, commit, destroy;
};
static struct wl_list layers;
static struct wlr_layer_surface_v1 *keys;             /* the one with the keys, or 0 */

static struct wlr_scene_tree *tree_for(enum zwlr_layer_shell_v1_layer l)
{
    return l == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ? srv.l_bg : l == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM ? srv.l_bottom
         : l == ZWLR_LAYER_SHELL_V1_LAYER_TOP ? srv.l_top : srv.l_overlay;
}

int layers_keys(void) { return keys != 0; }

static void give_keys(struct wlr_layer_surface_v1 *ls)
{
    keys = ls;
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv.seat);
    if (kb) wlr_seat_keyboard_notify_enter(srv.seat, ls->surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
    else wlr_seat_keyboard_notify_enter(srv.seat, ls->surface, NULL, 0, NULL);
}
static void drop_keys(struct wlr_layer_surface_v1 *ls)
{
    if (keys != ls) return;
    keys = 0;
    struct view *v = view_focused();                  /* (back to the window in focus) */
    struct wlr_surface *s = v ? view_surface(v) : 0;
    if (s && !locked()) {
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv.seat);
        if (kb) wlr_seat_keyboard_notify_enter(srv.seat, s, kb->keycodes, kb->num_keycodes, &kb->modifiers);
        else wlr_seat_keyboard_notify_enter(srv.seat, s, NULL, 0, NULL);
    } else if (!locked()) wlr_seat_keyboard_notify_clear_focus(srv.seat);
}

/* A press on a layer surface: the keys, if it takes them on demand. */
void layers_focus_at(struct wlr_surface *s)
{
    struct wlr_layer_surface_v1 *ls = s ? wlr_layer_surface_v1_try_from_wlr_surface(s) : 0;
    if (ls && ls->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) give_keys(ls);
    else if (keys && (!ls || ls != keys) && keys->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
        drop_keys(keys);
}

/* Place a screen's layer surfaces: those with an exclusive zone first (they
 * take their room), then the others in what is left. -> 1 if the room left
 * for windows changed. */
static int arrange(struct output *o)
{
    struct wlr_box full, usable;
    wlr_output_layout_get_box(srv.layout, o->wlr, &full);
    usable = full;
    if (wlr_box_empty(&full)) return 0;
    static const enum zwlr_layer_shell_v1_layer order[4] = { ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                                                            ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND };
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < 4; i++) {
            struct layer *l;
            wl_list_for_each(l, &layers, link) {
                if (l->o != o || !l->ls->initialized || l->ls->current.layer != order[i]) continue;
                if ((l->ls->current.exclusive_zone > 0) != (pass == 0)) continue;
                wlr_scene_layer_surface_v1_configure(l->sl, &full, &usable);
            }
        }
    int changed = memcmp(&usable, &o->usable, sizeof usable) != 0;
    o->usable = usable;
    return changed;
}
void layers_arrange(struct output *o) { arrange(o); }

static void relayout(struct output *o) { if (o && arrange(o)) bar_layout(); }

static void on_commit(struct wl_listener *li, void *data)
{
    (void)data;
    struct layer *l = wl_container_of(li, l, commit);
    struct wlr_layer_surface_v1 *ls = l->ls;
    if (!ls->initialized) return;
    struct wlr_scene_tree *t = tree_for(ls->current.layer);
    if (l->sl->tree->node.parent != t) wlr_scene_node_reparent(&l->sl->tree->node, t);   /* (it changed layer) */
    if (ls->initial_commit || ls->current.committed) relayout(l->o);
    if (l->mapped && ls->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE && keys != ls && !locked())
        give_keys(ls);
    else if (keys == ls && ls->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
        drop_keys(ls);
}
static void on_map(struct wl_listener *li, void *data)
{
    (void)data;
    struct layer *l = wl_container_of(li, l, map);
    l->mapped = 1;
    relayout(l->o);
    if (l->ls->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE && !locked()) give_keys(l->ls);
    say("layer surface shown: %s", l->ls->namespace ? l->ls->namespace : "");
    pointer_refocus();
}
static void on_unmap(struct wl_listener *li, void *data)
{
    (void)data;
    struct layer *l = wl_container_of(li, l, unmap);
    l->mapped = 0;
    drop_keys(l->ls);
    relayout(l->o);
    pointer_refocus();
}
static void on_destroy(struct wl_listener *li, void *data)
{
    (void)data;
    struct layer *l = wl_container_of(li, l, destroy);
    struct output *o = l->o;
    drop_keys(l->ls);
    wl_list_remove(&l->map.link); wl_list_remove(&l->unmap.link); wl_list_remove(&l->commit.link);
    wl_list_remove(&l->destroy.link); wl_list_remove(&l->link);
    free(l);
    relayout(o);
}
static void new_surface(struct wl_listener *li, void *data)
{
    (void)li;
    struct wlr_layer_surface_v1 *ls = data;
    if (!ls->output) {                                /* (on the pointer's screen) */
        struct output *o = output_at(srv.px, srv.py);
        if (!o) { wlr_layer_surface_v1_destroy(ls); return; }
        ls->output = o->wlr;
    }
    struct layer *l = calloc(1, sizeof *l);
    l->ls = ls;
    l->o = output_of(ls->output);
    l->sl = wlr_scene_layer_surface_v1_create(tree_for(ls->pending.layer), ls);
    ls->data = l->sl->tree;                           /* (where its popups go: view.c) */
    struct { struct wl_listener *l; struct wl_signal *s; wl_notify_func_t f; } ls_[] = {
        { &l->map, &ls->surface->events.map, on_map }, { &l->unmap, &ls->surface->events.unmap, on_unmap },
        { &l->commit, &ls->surface->events.commit, on_commit }, { &l->destroy, &ls->events.destroy, on_destroy },
    };
    for (size_t i = 0; i < sizeof ls_ / sizeof *ls_; i++) { ls_[i].l->notify = ls_[i].f; wl_signal_add(ls_[i].s, ls_[i].l); }
    wl_list_insert(layers.prev, &l->link);
}

void layers_output_gone(struct output *o)          /* (its surfaces are closed by wlroots; forget the screen) */
{
    struct layer *l;
    wl_list_for_each(l, &layers, link) if (l->o == o) l->o = 0;
}

static struct wl_listener new_layer;
void layers_init(void)
{
    wl_list_init(&layers);
    struct wlr_layer_shell_v1 *sh = wlr_layer_shell_v1_create(srv.display, 4);
    new_layer.notify = new_surface;
    wl_signal_add(&sh->events.new_surface, &new_layer);
}
