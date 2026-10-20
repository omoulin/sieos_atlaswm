/*
 * toplevels.c - The windows, as taskbars and docks see them: the window
 * list of wlr-foreign-toplevel-management (waybar's wlr/taskbar, sfwbar,
 * nwg-panel...) and of ext-foreign-toplevel-list.
 *
 * Each shown window (minimized ones too) is a handle with its title, its
 * app, its screen and its state (in focus, zoomed, minimized, full
 * screen); a taskbar's click brings it in front (its workspace with it),
 * minimizes, zooms, makes it full screen or closes it, as the Deskbar
 * does. The handles follow the windows each time the Deskbar is drawn.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include "atlas.h"

static struct wlr_foreign_toplevel_manager_v1 *mgr;
static struct wlr_ext_foreign_toplevel_list_v1 *list;
enum { T_ACT = 1, T_MAX = 2, T_MIN = 4, T_FS = 8 };

static void on_activate(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, ftl_activate);
    if (!locked()) { wm_show(v); view_focus(v); }
}
static void on_maximize(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, ftl_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *e = data;
    if (!!e->maximized != !!v->max) wm_toggle_max(v);
}
static void on_minimize(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, ftl_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *e = data;
    if (e->minimized) wm_min(v);
    else if (!locked()) { wm_show(v); view_focus(v); }
}
static void on_fullscreen(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, ftl_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *e = data;
    wm_fullscreen(v, e->fullscreen);
}
static void on_close(struct wl_listener *l, void *data) { (void)data; struct view *v = wl_container_of(l, v, ftl_close); view_close(v); }
static void on_destroy(struct wl_listener *l, void *data)   /* (wlroots ends it: the display goes) */
{
    (void)data;
    struct view *v = wl_container_of(l, v, ftl_destroy);
    struct wl_listener *ls[] = { &v->ftl_activate, &v->ftl_maximize, &v->ftl_minimize, &v->ftl_fullscreen, &v->ftl_close, &v->ftl_destroy };
    for (size_t i = 0; i < sizeof ls / sizeof *ls; i++) wl_list_remove(&ls[i]->link);
    v->ftl = 0;
}

void toplevels_gone(struct view *v)
{
    if (v->ftl) { struct wlr_foreign_toplevel_handle_v1 *h = v->ftl; on_destroy(&v->ftl_destroy, 0); wlr_foreign_toplevel_handle_v1_destroy(h); }
    if (v->eftl) { wlr_ext_foreign_toplevel_handle_v1_destroy(v->eftl); v->eftl = 0; }
    v->ftl_out = 0;
    v->ftl_state = 0;
}

static int same(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static void update(struct view *v)
{
    if (!v->mapped) { toplevels_gone(v); return; }
    const char *t = view_title(v), *a = view_app(v);
    if (!v->ftl) {
        v->ftl = wlr_foreign_toplevel_handle_v1_create(mgr);
        struct { struct wl_listener *l; struct wl_signal *s; wl_notify_func_t f; } ls[] = {
            { &v->ftl_activate, &v->ftl->events.request_activate, on_activate },
            { &v->ftl_maximize, &v->ftl->events.request_maximize, on_maximize },
            { &v->ftl_minimize, &v->ftl->events.request_minimize, on_minimize },
            { &v->ftl_fullscreen, &v->ftl->events.request_fullscreen, on_fullscreen },
            { &v->ftl_close, &v->ftl->events.request_close, on_close },
            { &v->ftl_destroy, &v->ftl->events.destroy, on_destroy },
        };
        for (size_t i = 0; i < sizeof ls / sizeof *ls; i++) { ls[i].l->notify = ls[i].f; wl_signal_add(ls[i].s, ls[i].l); }
        v->ftl_state = -1;
    }
    if (!same(v->ftl->title, t)) wlr_foreign_toplevel_handle_v1_set_title(v->ftl, t);
    if (!same(v->ftl->app_id, a)) wlr_foreign_toplevel_handle_v1_set_app_id(v->ftl, a);
    int st = (view_focused() == v ? T_ACT : 0) | (v->max ? T_MAX : 0) | (v->min ? T_MIN : 0) | (v->fs ? T_FS : 0);
    if (st != v->ftl_state) {
        wlr_foreign_toplevel_handle_v1_set_activated(v->ftl, st & T_ACT);
        wlr_foreign_toplevel_handle_v1_set_maximized(v->ftl, st & T_MAX);
        wlr_foreign_toplevel_handle_v1_set_minimized(v->ftl, st & T_MIN);
        wlr_foreign_toplevel_handle_v1_set_fullscreen(v->ftl, st & T_FS);
        v->ftl_state = st;
    }
    struct output *o = output_of_box((box_t){ v->x0, v->y0 - TAB_H / 2, v->x0 + 1, v->y0 - TAB_H / 2 + 1 });   /* (its tab's screen) */
    struct wlr_output *wo = o ? o->wlr : 0;
    if (wo != v->ftl_out) {
        if (v->ftl_out) wlr_foreign_toplevel_handle_v1_output_leave(v->ftl, v->ftl_out);
        if (wo) wlr_foreign_toplevel_handle_v1_output_enter(v->ftl, wo);
        v->ftl_out = wo;
    }
    struct wlr_ext_foreign_toplevel_handle_v1_state es = { .title = t, .app_id = a };
    if (!v->eftl) v->eftl = wlr_ext_foreign_toplevel_handle_v1_create(list, &es);
    else if (!same(v->eftl->title, t) || !same(v->eftl->app_id, a)) wlr_ext_foreign_toplevel_handle_v1_update_state(v->eftl, &es);
}

void toplevels_update(void)
{
    struct view *v;
    wl_list_for_each(v, &srv.views, link) update(v);
}

void toplevels_init(void)
{
    mgr = wlr_foreign_toplevel_manager_v1_create(srv.display);
    list = wlr_ext_foreign_toplevel_list_v1_create(srv.display, 1);
}
