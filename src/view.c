/*
 * view.c - The apps' windows: xdg-shell toplevels and their popups, what
 * every window shares with X11's (xwayland.c), and the decorations.
 *
 * Each window is a tree of the scene, in this order:
 *   the frame's four bands (wm.c), the ground under the app (it may not
 *   fill its box), the app's surfaces, the dim over them (Settings, not
 *   in focus), the grip over the app's corner, and the app's menus
 *   (popups), over everything of its window.
 *
 * Decorations are ours: an app that speaks xdg-decoration (Qt, foot,
 * Firefox...) or KDE's server-decoration (GTK 3) draws none. One that
 * keeps its own title bar (GTK 4 and libadwaita: their bar holds the app's
 * buttons) gets no strip from us, its bar under our tab; it is not offered
 * minimize and maximize, so it hides those buttons (the tab has them).
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/xwayland.h>
#include "atlas.h"

static struct view *focused;
static struct wlr_server_decoration_manager *kde;

struct view *view_focused(void) { return focused; }
const char *view_title(struct view *v)
{
    if (v->internal) return "Settings";
    const char *t = v->xs ? v->xs->title : v->top ? v->top->title : 0;
    return t ? t : "";
}
const char *view_app(struct view *v)
{
    if (v->internal) return "atlas.settings";
    const char *a = v->xs ? v->xs->class : v->top ? v->top->app_id : 0;
    return a ? a : "";
}
int view_icon(struct view *v) { return icon_for_app(view_app(v)); }
void view_close(struct view *v)
{
    if (!v) return;
    if (v->internal) settings_gone(v);
    else if (v->xs) wlr_xwayland_surface_close(v->xs);
    else if (v->top) wlr_xdg_toplevel_send_close(v->top);
}
struct view *view_of(struct wlr_xdg_toplevel *t)
{
    struct view *v;
    wl_list_for_each(v, &srv.views, link) if (v->top && v->top == t) return v;
    return 0;
}
struct wlr_surface *view_surface(struct view *v) { return v->xs ? v->xs->surface : v->top ? v->top->base->surface : 0; }

void view_min_size(struct view *v, int *w, int *h)
{
    *w = *h = 0;
    if (v->top) { *w = v->top->current.min_width; *h = v->top->current.min_height; }
    else if (v->xs && v->xs->size_hints && (v->xs->size_hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)) {
        *w = v->xs->size_hints->min_width; *h = v->xs->size_hints->min_height;
    }
}
void view_own_size(struct view *v, int *w, int *h)
{
    if (v->internal) { *w = *h = 0; return; }
    if (v->xs) { *w = v->xs->width; *h = v->xs->height; return; }
    struct wlr_box g;
    wlr_xdg_surface_get_geometry(v->top->base, &g);
    *w = g.width; *h = g.height;
}
struct view *view_parent(struct view *v)
{
    if (v->top) return v->top->parent ? view_of(v->top->parent) : 0;
    if (!v->xs || !v->xs->parent) return 0;
    struct view *p;
    wl_list_for_each(p, &srv.views, link) if (p->xs == v->xs->parent) return p;
    return 0;
}
void view_set_resizing(struct view *v, int on) { if (v->top) wlr_xdg_toplevel_set_resizing(v->top, on); }

void view_restack(void)
{
    struct view *v;
    wl_list_for_each(v, &srv.views, link) wlr_scene_node_raise_to_top(&v->tree->node);
}

/* (wlroots' xdg tree already moves the surface by its geometry: a CSD
 * app's shadow lies outside its box) */
void view_place_app(struct view *v)
{
    int x0, y0, x1, y1;
    wm_client_box(v, &x0, &y0, &x1, &y1);
    wlr_scene_node_set_position(&v->app->node, L(x0), L(y0));
    wlr_scene_node_set_position(&v->popups->node, L(x0), L(y0));
}

/* Tell the app its size and state (only what changed is sent). */
void view_configure(struct view *v)
{
    if (!v->known || v->internal) return;
    int x0, y0, x1, y1;
    wm_client_box(v, &x0, &y0, &x1, &y1);
    int w = L(x1) - L(x0), h = L(y1) - L(y0);
    if (v->xs) {
        wlr_xwayland_surface_set_maximized(v->xs, v->max);
        wlr_xwayland_surface_set_fullscreen(v->xs, v->fs);
        if (w > 0 && h > 0) xwayland_configure(v, L(x0), L(y0), w, h);
        view_place_app(v);
        return;
    }
    if (!v->top || !v->top->base->initialized) return;
    wlr_xdg_toplevel_set_maximized(v->top, v->max);
    wlr_xdg_toplevel_set_fullscreen(v->top, v->fs);
    wlr_xdg_toplevel_set_activated(v->top, v == focused);
    if (w > 0 && h > 0 && (abs(w - v->sent_w) > 1 || abs(h - v->sent_h) > 1)) {   /* (a pixel of rounding: not news) */
        v->sent_w = w; v->sent_h = h;
        v->sent_serial = wlr_xdg_toplevel_set_size(v->top, w, h);
    }
    view_place_app(v);
}

static void activate(struct view *v, int on)
{
    if (v->xs) xwayland_activate(v, on);
    else if (v->top && v->top->base->initialized) wlr_xdg_toplevel_set_activated(v->top, on);
}
void view_focus(struct view *v)
{
    struct view *old = focused;
    if (v && !v->mapped) return;
    if (v) {
        v->focus_ns = now_ns();
        bar_attention(v, 0);                             /* (shown: it has it) */
        if (!wm_shown(v)) wm_show(v);
        wl_list_remove(&v->link);
        wl_list_insert(srv.views.prev, &v->link);
        view_restack();
    }
    if (old == v) return;
    focused = v;
    if (old) { activate(old, 0); wm_draw(old); }
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv.seat);
    struct wlr_surface *s = v ? view_surface(v) : 0;
    if (v) { activate(v, 1); wm_draw(v); }
    if (!locked() && !layers_keys()) {                   /* (a locker or a launcher keeps the keys) */
        if (s && kb) wlr_seat_keyboard_notify_enter(srv.seat, s, kb->keycodes, kb->num_keycodes, &kb->modifiers);
        else if (s) wlr_seat_keyboard_notify_enter(srv.seat, s, NULL, 0, NULL);
        else wlr_seat_keyboard_notify_clear_focus(srv.seat);
    }
    wm_focus_changed();
    bar_draw();
}

/* ---- Who draws the title bar. */
static int draws_own_bar(struct view *v)
{
    if (v->internal) return 0;
    if (v->xs) return v->xs->decorations != WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;   /* (Motif hints: none of ours) */
    if (v->deco) return 0;
    struct wlr_surface *s = view_surface(v);
    struct wlr_server_decoration *d;
    if (kde && s) wl_list_for_each(d, &kde->decorations, link)
        if (d->surface == s) return d->mode != WLR_SERVER_DECORATION_MANAGER_MODE_SERVER;
    return 1;
}
void view_csd_update(struct view *v)
{
    int c = draws_own_bar(v);
    if (c == v->csd) return;
    v->csd = c;
    if (!v->known) return;
    view_configure(v);
    wm_draw(v);
}

/* KDE's protocol: a decoration object per surface; its mode may change. */
struct kdeco { struct wlr_server_decoration *d; struct wl_listener mode, destroy; };
static struct view *view_by_surface(struct wlr_surface *s)
{
    struct view *v;
    wl_list_for_each(v, &srv.views, link) if (view_surface(v) == s) return v;
    return 0;
}
static void kde_mode(struct wl_listener *l, void *data)
{
    (void)data;
    struct kdeco *k = wl_container_of(l, k, mode);
    struct view *v = view_by_surface(k->d->surface);
    if (v) view_csd_update(v);
}
static void kde_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct kdeco *k = wl_container_of(l, k, destroy);
    wl_list_remove(&k->mode.link);
    wl_list_remove(&k->destroy.link);
    free(k);
}
static void new_kde(struct wl_listener *l, void *data)
{
    (void)l;
    struct kdeco *k = calloc(1, sizeof *k);
    k->d = data;
    k->mode.notify = kde_mode; wl_signal_add(&k->d->events.mode, &k->mode);
    k->destroy.notify = kde_destroy; wl_signal_add(&k->d->events.destroy, &k->destroy);
    struct view *v = view_by_surface(k->d->surface);
    if (v) view_csd_update(v);
}

/* ---- What every window shares. */
void view_tree_init(struct view *v)
{
    v->tree = wlr_scene_tree_create(srv.l_wins);
    for (int i = 0; i < NBAND; i++) chrome_init(&v->band[i], v->tree, CH_VIEW, v, &v->hits);
    v->ground = solid_create(v->tree, 0xff000000u | C_PANEL, true);
    v->ground->node.data = 0;
    chrome_init(&v->smoke, v->tree, CH_VIEW, v, &v->shits);
    v->app = v->top ? wlr_scene_xdg_surface_create(v->tree, v->top->base) : wlr_scene_tree_create(v->tree);
    v->dim = solid_create(v->tree, 0x30000000u, false);   /* (3/16 darker) */
    chrome_init(&v->grip, v->tree, CH_VIEW, v, &v->hits);
    v->popups = wlr_scene_tree_create(v->tree);
    v->tree->node.data = v;
    wlr_scene_node_set_enabled(&v->tree->node, false);
    wl_list_insert(srv.views.prev, &v->link);
}
void view_mapped(struct view *v)
{
    v->mapped = 1;
    view_csd_update(v);
    wm_show(v);
    if (v->fs) { v->fs = 0; wm_fullscreen(v, 1); }       /* (it asked to start full screen) */
    view_focus(v);
    say("window opened: %s (%s)%s", view_title(v), view_app(v), v->xs ? " X11" : "");
    bar_draw();
    desk_draw();
    pointer_refocus();
}
void view_unmapped(struct view *v)
{
    v->mapped = 0;
    v->attn_ns = 0;
    menu_forget(v);
    wm_gone(v);
    wlr_scene_node_set_enabled(&v->tree->node, false);
    if (focused == v) { focused = 0; view_focus(wm_top()); if (!focused && !locked() && !layers_keys()) wlr_seat_keyboard_notify_clear_focus(srv.seat); }
    v->sent_w = v->sent_h = 0; v->sent_serial = 0;
    bar_draw();
    desk_draw();
    pointer_refocus();
}
void view_destroyed(struct view *v)
{
    if (focused == v) focused = 0;
    toplevels_gone(v);
    menu_forget(v);
    wm_gone(v);
    wl_list_remove(&v->link);
    for (int i = 0; i < NBAND; i++) chrome_fini(&v->band[i]);   /* (off the glass list: frame() reads it) */
    chrome_fini(&v->grip);
    chrome_fini(&v->smoke);
    if (v->internal) chrome_fini(&v->body);              /* (Settings' inside: only it has one) */
    wlr_scene_node_destroy(&v->tree->node);              /* (the chrome's buffers with it) */
    free(v->hits.h);
    free(v->shits.h);
    free(v->bhits.h);
    free(v);
    bar_draw();
}

/* ---- Popups: menus and tooltips, over their window (or panel), kept on
 * its screen. A popup is put in the scene at its first commit: a panel's
 * gets its parent only then. */
struct popup { struct wlr_xdg_popup *p; struct wl_listener commit, destroy; };
static struct wlr_scene_tree *parent_tree(struct wlr_surface *s)
{
    if (!s) return 0;
    struct wlr_xdg_surface *x = wlr_xdg_surface_try_from_wlr_surface(s);
    if (x) return x->data;
    struct wlr_layer_surface_v1 *ls = wlr_layer_surface_v1_try_from_wlr_surface(s);
    return ls ? ls->data : 0;
}
static void popup_commit(struct wl_listener *l, void *data)
{
    (void)data;
    struct popup *pp = wl_container_of(l, pp, commit);
    if (!pp->p->base->initial_commit) return;
    struct wlr_scene_tree *parent = parent_tree(pp->p->parent);
    if (parent && !pp->p->base->data) pp->p->base->data = wlr_scene_xdg_surface_create(parent, pp->p->base);
    if (parent) {                                        /* (its screen, in its parent's coordinates) */
        int lx, ly;
        wlr_scene_node_coords(&parent->node, &lx, &ly);
        struct output *o = output_at(lx, ly);
        struct wlr_box ob = { 0, 0, L(PW), L(PH) };
        if (o) wlr_output_layout_get_box(srv.layout, o->wlr, &ob);
        struct wlr_box box = { ob.x - lx, ob.y - ly, ob.width, ob.height };
        wlr_xdg_popup_unconstrain_from_box(pp->p, &box);
    }
    wlr_xdg_surface_schedule_configure(pp->p->base);
}
static void popup_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct popup *pp = wl_container_of(l, pp, destroy);
    wl_list_remove(&pp->commit.link);
    wl_list_remove(&pp->destroy.link);
    free(pp);
}
static void new_popup(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_xdg_popup *p = data;
    struct popup *pp = calloc(1, sizeof *pp);
    pp->p = p;
    pp->commit.notify = popup_commit;
    wl_signal_add(&p->base->surface->events.commit, &pp->commit);
    pp->destroy.notify = popup_destroy;
    wl_signal_add(&p->events.destroy, &pp->destroy);
}

/* ---- xdg-decoration: always the server's. */
static void deco_set(struct view *v)
{
    if (v->deco && v->top->base->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(v->deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static void deco_mode(struct wl_listener *l, void *data) { (void)data; struct view *v = wl_container_of(l, v, deco_mode); deco_set(v); }
static void deco_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, deco_destroy);
    wl_list_remove(&v->deco_mode.link);
    wl_list_remove(&v->deco_destroy.link);
    v->deco = 0;
    view_csd_update(v);
}
static void new_deco(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_xdg_toplevel_decoration_v1 *d = data;
    struct view *v = view_of(d->toplevel);
    if (!v || v->deco) return;
    v->deco = d;
    v->deco_mode.notify = deco_mode;
    wl_signal_add(&d->events.request_mode, &v->deco_mode);
    v->deco_destroy.notify = deco_destroy;
    wl_signal_add(&d->events.destroy, &v->deco_destroy);
    deco_set(v);
    view_csd_update(v);
}

/* ---- Toplevels. */
static void on_commit(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, commit);
    struct wlr_xdg_toplevel *t = v->top;
    if (t->base->initial_commit) {
        deco_set(v);
        v->csd = draws_own_bar(v);
        uint32_t caps = WLR_XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU | WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN;
        if (!v->csd) caps |= WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE | WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE;
        wlr_xdg_toplevel_set_wm_capabilities(t, caps);   /* (its own bar: no minimize or maximize buttons there) */
        wlr_xdg_toplevel_set_tiled(t, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
        int fixed = t->parent || (t->pending.min_width && t->pending.min_width == t->pending.max_width &&
                                  t->pending.min_height && t->pending.min_height == t->pending.max_height);
        if (fixed) { v->sent_w = v->sent_h = 0; wlr_xdg_toplevel_set_size(t, 0, 0); }   /* (it chooses; wm_show places it) */
        else { wm_place(v); view_configure(v); }
        if (t->requested.fullscreen) { v->fs = 1; wlr_xdg_toplevel_set_fullscreen(t, true); }
        return;
    }
    if (!v->mapped || !v->known) return;
    struct wlr_box g;
    wlr_xdg_surface_get_geometry(t->base, &g);
    int x0, y0, x1, y1;
    wm_client_box(v, &x0, &y0, &x1, &y1);
    int w = L(x1) - L(x0), h = L(y1) - L(y0);
    if (!v->fs && !wm_grabbing() && g.width > 0 && g.height > 0 && (g.width != w || g.height != h) &&
        (!v->sent_serial || t->base->current.configure_serial == v->sent_serial)) {
        wm_adopt(v, g.width, g.height);                  /* (it took another size than asked: the box follows) */
        v->sent_w = g.width; v->sent_h = g.height;
    }
    view_place_app(v);
}

static void on_map(struct wl_listener *l, void *data) { (void)data; struct view *v = wl_container_of(l, v, map); view_mapped(v); }
static void on_unmap(struct wl_listener *l, void *data) { (void)data; struct view *v = wl_container_of(l, v, unmap); view_unmapped(v); }
static void on_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, destroy);
    struct wl_listener *ls[] = { &v->map, &v->unmap, &v->commit, &v->destroy, &v->request_move, &v->request_resize,
                                 &v->request_maximize, &v->request_fullscreen, &v->request_minimize, &v->request_menu,
                                 &v->set_title, &v->set_app_id };
    for (size_t i = 0; i < sizeof ls / sizeof *ls; i++) wl_list_remove(&ls[i]->link);
    if (v->deco) { wl_list_remove(&v->deco_mode.link); wl_list_remove(&v->deco_destroy.link); }
    view_destroyed(v);
}

static void cursor_units(int *x, int *y) { *x = dunit(P(srv.px)); *y = dunit(P(srv.py)); }
static void on_move(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, request_move);
    int x, y;
    cursor_units(&x, &y);
    if (v->max) wm_toggle_max(v);
    wm_grab(v, 0, x, y);
}
static void on_resize(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, request_resize);
    struct wlr_xdg_toplevel_resize_event *e = data;
    int x, y, m = (e->edges & WLR_EDGE_LEFT ? 1 : 0) | (e->edges & WLR_EDGE_RIGHT ? 2 : 0) |
                  (e->edges & WLR_EDGE_TOP ? 4 : 0) | (e->edges & WLR_EDGE_BOTTOM ? 8 : 0);
    cursor_units(&x, &y);
    if (m) wm_grab(v, m, x, y);
}
static void on_maximize(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, request_maximize);
    if (!v->top->base->initialized) return;
    if (!!v->top->requested.maximized != !!v->max && v->known) wm_toggle_max(v);
    else wlr_xdg_surface_schedule_configure(v->top->base);
}
static void on_fullscreen(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, request_fullscreen);
    if (!v->top->base->initialized) return;
    if (v->mapped && v->known) wm_fullscreen(v, v->top->requested.fullscreen);
    else wlr_xdg_surface_schedule_configure(v->top->base);
}
static void on_minimize(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, request_minimize);
    if (v->mapped) wm_min(v);
    else if (v->top->base->initialized) wlr_xdg_surface_schedule_configure(v->top->base);
}
static void on_menu(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, request_menu);
    int x, y;
    cursor_units(&x, &y);
    menu_window(v, x, y);
}
static void on_title(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, set_title);
    wm_draw(v);
    bar_draw();
}
static void on_app_id(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, set_app_id);
    wm_draw(v);
    bar_draw();
}

static void new_toplevel(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_xdg_toplevel *t = data;
    struct view *v = calloc(1, sizeof *v);
    v->top = t;
    view_tree_init(v);
    t->base->data = v->popups;                           /* (where its popups go) */
    struct { struct wl_listener *l; struct wl_signal *s; wl_notify_func_t f; } ls[] = {
        { &v->map, &t->base->surface->events.map, on_map },
        { &v->unmap, &t->base->surface->events.unmap, on_unmap },
        { &v->commit, &t->base->surface->events.commit, on_commit },
        { &v->destroy, &t->events.destroy, on_destroy },
        { &v->request_move, &t->events.request_move, on_move },
        { &v->request_resize, &t->events.request_resize, on_resize },
        { &v->request_maximize, &t->events.request_maximize, on_maximize },
        { &v->request_fullscreen, &t->events.request_fullscreen, on_fullscreen },
        { &v->request_minimize, &t->events.request_minimize, on_minimize },
        { &v->request_menu, &t->events.request_show_window_menu, on_menu },
        { &v->set_title, &t->events.set_title, on_title },
        { &v->set_app_id, &t->events.set_app_id, on_app_id },
    };
    for (size_t i = 0; i < sizeof ls / sizeof *ls; i++) { ls[i].l->notify = ls[i].f; wl_signal_add(ls[i].s, ls[i].l); }
}

static struct wl_listener kde_new;
void view_init(void)
{
    srv.xdg = wlr_xdg_shell_create(srv.display, 6);
    srv.new_toplevel.notify = new_toplevel;
    wl_signal_add(&srv.xdg->events.new_toplevel, &srv.new_toplevel);
    srv.new_popup.notify = new_popup;
    wl_signal_add(&srv.xdg->events.new_popup, &srv.new_popup);
    struct wlr_xdg_decoration_manager_v1 *dm = wlr_xdg_decoration_manager_v1_create(srv.display);
    srv.new_deco.notify = new_deco;
    wl_signal_add(&dm->events.new_toplevel_decoration, &srv.new_deco);
    kde = wlr_server_decoration_manager_create(srv.display);   /* (GTK 3: no title bar of its own) */
    wlr_server_decoration_manager_set_default_mode(kde, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    kde_new.notify = new_kde;
    wl_signal_add(&kde->events.new_decoration, &kde_new);
}
