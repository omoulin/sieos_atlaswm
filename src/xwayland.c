/*
 * xwayland.c - X11 apps, through Xwayland (started when the first one
 * connects; DISPLAY is set for the apps atlas_wm starts).
 *
 * An X11 window is a window like any other (view.c, wm.c): its tab, its
 * frame, its place, its workspace. It is told where it is on the desk (X
 * apps place their own menus from it). One that asks for no decorations
 * (Motif hints: a GTK app drawing its own title bar) gets no strip, as on
 * Wayland. The override-redirect ones (menus, tooltips, drop-downs) are
 * not windows: they are shown where X puts them, over everything, and get
 * the keys when they want them. One that sets its urgency hint asks for
 * attention (the tray's pill, deskbar.c).
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <wlr/xwayland.h>
#include "atlas.h"

/* An X surface: a window (a view, once shown) or a menu over everything. */
struct xsurf {
    struct view *v;                                     /* (allocated at once; its tree when first shown as a window) */
    struct wlr_xwayland_surface *xs;
    struct wlr_scene_tree *menu;                        /* shown as an override-redirect one */
    int menu_keys;                                      /* ... with the keys */
    int assoc;                                          /* its wl_surface is there (map, unmap listened) */
    struct wl_listener map, unmap, associate, dissociate, destroy, request_configure, request_activate,
                       request_move, request_resize, request_maximize, request_fullscreen, request_minimize,
                       set_title, set_class, set_decorations, set_geometry, set_hints;
};

void xwayland_configure(struct view *v, int lx, int ly, int w, int h)
{
    struct wlr_xwayland_surface *xs = v->xs;
    if (xs->x != lx || xs->y != ly || xs->width != w || xs->height != h) wlr_xwayland_surface_configure(xs, lx, ly, w, h);
}
void xwayland_activate(struct view *v, int on)
{
    wlr_xwayland_surface_activate(v->xs, on);
    if (on) wlr_xwayland_surface_restack(v->xs, NULL, XCB_STACK_MODE_ABOVE);
}

static void keys_to(struct wlr_surface *s)
{
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv.seat);
    if (kb) wlr_seat_keyboard_notify_enter(srv.seat, s, kb->keycodes, kb->num_keycodes, &kb->modifiers);
    else wlr_seat_keyboard_notify_enter(srv.seat, s, NULL, 0, NULL);
}

static void on_map(struct wl_listener *l, void *data)
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, map);
    struct wlr_xwayland_surface *xs = x->xs;
    if (xs->override_redirect) {                        /* a menu, a tooltip: where X says, over everything */
        x->menu = wlr_scene_tree_create(srv.l_xmenu);
        wlr_scene_subsurface_tree_create(x->menu, xs->surface);
        wlr_scene_node_set_position(&x->menu->node, xs->x, xs->y);
        if (wlr_xwayland_or_surface_wants_focus(xs) && !locked()) { keys_to(xs->surface); x->menu_keys = 1; }
        return;
    }
    struct view *v = x->v;
    if (!v->tree) view_tree_init(v);
    v->xsurf = wlr_scene_subsurface_tree_create(v->app, xs->surface);
    if (!v->known) {                                    /* (the size it asked for; a dialog over its parent) */
        int fixed = xs->parent || xs->modal || (xs->size_hints && (xs->size_hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) &&
                    (xs->size_hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) && xs->size_hints->min_width == xs->size_hints->max_width &&
                    xs->size_hints->min_height == xs->size_hints->max_height);
        if (!fixed) wm_place(v);
    }
    if (xs->fullscreen) v->fs = 1;
    view_mapped(v);
}
static void on_unmap(struct wl_listener *l, void *data)
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, unmap);
    if (x->menu) {
        wlr_scene_node_destroy(&x->menu->node);
        x->menu = 0;
        if (x->menu_keys) {                             /* (the keys back to the window in focus) */
            x->menu_keys = 0;
            struct view *f = view_focused();
            if (f && view_surface(f) && !locked() && !layers_keys()) keys_to(view_surface(f));
        }
        return;
    }
    struct view *v = x->v;
    if (!v->tree || !v->mapped) return;
    if (v->xsurf) { wlr_scene_node_destroy(&v->xsurf->node); v->xsurf = 0; }
    view_unmapped(v);
}
static void on_associate(struct wl_listener *l, void *data)   /* (its wl_surface has come) */
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, associate);
    x->assoc = 1;
    x->map.notify = on_map; wl_signal_add(&x->xs->surface->events.map, &x->map);
    x->unmap.notify = on_unmap; wl_signal_add(&x->xs->surface->events.unmap, &x->unmap);
}
static void on_dissociate(struct wl_listener *l, void *data)
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, dissociate);
    if (!x->assoc) return;
    x->assoc = 0;
    wl_list_remove(&x->map.link);
    wl_list_remove(&x->unmap.link);
}
static void on_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, destroy);
    struct wl_listener *ls[] = { &x->associate, &x->dissociate, &x->destroy, &x->request_configure, &x->request_activate,
                                 &x->request_move, &x->request_resize, &x->request_maximize, &x->request_fullscreen,
                                 &x->request_minimize, &x->set_title, &x->set_class, &x->set_decorations, &x->set_geometry, &x->set_hints };
    for (size_t i = 0; i < sizeof ls / sizeof *ls; i++) wl_list_remove(&ls[i]->link);
    if (x->assoc) { wl_list_remove(&x->map.link); wl_list_remove(&x->unmap.link); }
    if (x->menu) wlr_scene_node_destroy(&x->menu->node);
    if (x->v->tree) view_destroyed(x->v);              /* (frees it) */
    else free(x->v);
    free(x);
}

static void on_request_configure(struct wl_listener *l, void *data)
{
    struct xsurf *x = wl_container_of(l, x, request_configure);
    struct wlr_xwayland_surface_configure_event *e = data;
    struct view *v = x->v;
    if (x->xs->override_redirect || !v->mapped || !v->known) {   /* (not shown yet: as it asks) */
        wlr_xwayland_surface_configure(x->xs, e->x, e->y, e->width, e->height);
        return;
    }
    wm_adopt(v, e->width, e->height);                   /* (shown: its size, not its place) */
    view_configure(v);
}
static void on_request_activate(struct wl_listener *l, void *data)
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, request_activate);
    if (x->v->mapped) view_focus(x->v);
}
static void cursor_units(int *ux, int *uy) { *ux = dunit(P(srv.px)); *uy = dunit(P(srv.py)); }
static void on_request_move(struct wl_listener *l, void *data)
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, request_move);
    int ux, uy;
    cursor_units(&ux, &uy);
    if (x->v->max) wm_toggle_max(x->v);
    wm_grab(x->v, 0, ux, uy);
}
static void on_request_resize(struct wl_listener *l, void *data)
{
    struct xsurf *x = wl_container_of(l, x, request_resize);
    struct wlr_xwayland_resize_event *e = data;
    int ux, uy, m = (e->edges & WLR_EDGE_LEFT ? 1 : 0) | (e->edges & WLR_EDGE_RIGHT ? 2 : 0) |
                    (e->edges & WLR_EDGE_TOP ? 4 : 0) | (e->edges & WLR_EDGE_BOTTOM ? 8 : 0);
    cursor_units(&ux, &uy);
    if (m) wm_grab(x->v, m, ux, uy);
}
static void on_request_maximize(struct wl_listener *l, void *data)
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, request_maximize);
    int want = x->xs->maximized_horz || x->xs->maximized_vert;
    if (x->v->known && want != !!x->v->max) wm_toggle_max(x->v);
}
static void on_request_fullscreen(struct wl_listener *l, void *data)
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, request_fullscreen);
    if (x->v->mapped) wm_fullscreen(x->v, x->xs->fullscreen);
    else x->v->fs = x->xs->fullscreen;
}
static void on_request_minimize(struct wl_listener *l, void *data)
{
    struct xsurf *x = wl_container_of(l, x, request_minimize);
    struct wlr_xwayland_minimize_event *e = data;
    if (x->v->mapped && e->minimize) wm_min(x->v);
}
static void on_set_title(struct wl_listener *l, void *data)
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, set_title);
    if (x->v->mapped) { wm_draw(x->v); bar_draw(); }
}
static void on_set_class(struct wl_listener *l, void *data)
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, set_class);
    if (x->v->mapped) { wm_draw(x->v); bar_draw(); }
}
static void on_set_decorations(struct wl_listener *l, void *data)
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, set_decorations);
    if (x->v->tree) view_csd_update(x->v);
}
static void on_set_hints(struct wl_listener *l, void *data)   /* (urgency: attention, unless in focus) */
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, set_hints);
    int urgent = x->xs->hints && (x->xs->hints->flags & XCB_ICCCM_WM_HINT_X_URGENCY);
    if (x->v->mapped) bar_attention(x->v, urgent && x->v != view_focused());
}
static void on_set_geometry(struct wl_listener *l, void *data)   /* (a menu moved) */
{
    (void)data;
    struct xsurf *x = wl_container_of(l, x, set_geometry);
    if (x->menu) wlr_scene_node_set_position(&x->menu->node, x->xs->x, x->xs->y);
}

static void new_surface(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_xwayland_surface *xs = data;
    struct xsurf *x = calloc(1, sizeof *x);
    x->xs = xs;
    x->v = calloc(1, sizeof *x->v);
    x->v->xs = xs;
    xs->data = x;
    struct { struct wl_listener *l; struct wl_signal *s; wl_notify_func_t f; } ls[] = {
        { &x->associate, &xs->events.associate, on_associate }, { &x->dissociate, &xs->events.dissociate, on_dissociate },
        { &x->destroy, &xs->events.destroy, on_destroy }, { &x->request_configure, &xs->events.request_configure, on_request_configure },
        { &x->request_activate, &xs->events.request_activate, on_request_activate },
        { &x->request_move, &xs->events.request_move, on_request_move }, { &x->request_resize, &xs->events.request_resize, on_request_resize },
        { &x->request_maximize, &xs->events.request_maximize, on_request_maximize },
        { &x->request_fullscreen, &xs->events.request_fullscreen, on_request_fullscreen },
        { &x->request_minimize, &xs->events.request_minimize, on_request_minimize },
        { &x->set_title, &xs->events.set_title, on_set_title }, { &x->set_class, &xs->events.set_class, on_set_class },
        { &x->set_decorations, &xs->events.set_decorations, on_set_decorations },
        { &x->set_geometry, &xs->events.set_geometry, on_set_geometry }, { &x->set_hints, &xs->events.set_hints, on_set_hints },
    };
    for (size_t i = 0; i < sizeof ls / sizeof *ls; i++) { ls[i].l->notify = ls[i].f; wl_signal_add(ls[i].s, ls[i].l); }
}

static struct wl_listener new_xs, ready;
static void on_ready(struct wl_listener *l, void *data)   /* (the X server is up: its pointer is ours) */
{
    (void)l; (void)data;
    say("Xwayland ready on DISPLAY=%s", srv.xwayland->display_name);
    wlr_xwayland_set_seat(srv.xwayland, srv.seat);
    cursor_arrow();
    int st;
    uint32_t *px = pixbuf_data(srv.arrow, &st);
    wlr_xwayland_set_cursor(srv.xwayland, (uint8_t *)px, st * 4, srv.arrow->width, srv.arrow->height, 0, 0);
}

void xwayland_init(void)
{
    srv.xwayland = wlr_xwayland_create(srv.display, srv.compositor, true);   /* (lazy: started by the first X app) */
    if (!srv.xwayland) { say("no Xwayland: X11 apps will not run"); unsetenv("DISPLAY"); return; }
    new_xs.notify = new_surface; wl_signal_add(&srv.xwayland->events.new_surface, &new_xs);
    ready.notify = on_ready; wl_signal_add(&srv.xwayland->events.ready, &ready);
    wlr_xwayland_set_seat(srv.xwayland, srv.seat);
    setenv("DISPLAY", srv.xwayland->display_name, 1);
    say("X11 apps: DISPLAY=%s", srv.xwayland->display_name);
}
