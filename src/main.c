/*
 * main.c - atlas_wm: SIEOS's Atlas desktop as a Wayland compositor.
 *
 *   the backend (DRM on a VT, or a window of another compositor)
 *     │ inputs                                    outputs
 *     ▼                                             ▲
 *   the seat ─── keys, the pointer ──► apps     the glass (glass.c)
 *     │                                             ▲
 *     └─ the chrome (Deskbar, tabs, menus) ──► the scene (wlroots): the
 *        drawn on the CPU (draw.c)                desk, windows, Deskbar, menus
 *
 * Keys (Super is the Windows key): Super+T or Super+Return a terminal,
 * Super+W close the window, Super+Tab the next window, Super+1..4 a
 * workspace, Super+Shift+1..4 send the window there, Super+Up zoom,
 * Super+Down minimize, Super+F full screen, Super+Space the palette
 * (apps, windows, files, actions, a command; palette.c), Super+, Settings,
 * Super+L lock the screen, Super+drag move a window (right button:
 * resize), Super+Shift+E or Ctrl+Alt+Backspace quit, Ctrl+Alt+F1..F12
 * another VT.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>
#include <linux/input-event-codes.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/interfaces/wlr_buffer.h>
#include "atlas.h"

struct server srv;

uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}
void say(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    fputs("atlas: ", stderr);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    va_end(a);
}
void atlas_quit(void) { wl_display_terminate(srv.display); }

/* ---- The animation: a timer every 16 ms, only while something moves (a
 * tab, a ghost, a window fading). */
static int animating;
static int anim_tick(void *data)
{
    (void)data;
    if (wm_tick()) wl_event_source_timer_update(srv.anim, 16);
    else animating = 0;
    return 0;
}
void anim_kick(void) { if (!animating) { animating = 1; wl_event_source_timer_update(srv.anim, 16); } }

/* ---- The clock: once a minute, just after it changes. */
static int clock_tick(void *data)
{
    (void)data;
    settings_clock();
    bar_draw();
    if (!wm_top()) desk_draw();
    lock_redraw();
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    wl_event_source_timer_update(srv.clock, (int)((60 - t.tv_sec % 60) * 1000 - t.tv_nsec / 1000000 + 50));
    return 0;
}

/* ---- The pointer: the desktop's arrow (SIEOS's), 12 x 19, white with a
 * dark outline; an app's own over its surfaces. */
static const char *arrow[19] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X",
    "X........X", "X.........X", "X..........X", "X......XXXXX", "X...X..X", "X..XX..X",
    "X.X  X..X", "XX   X..X", "     X..X", "      XX" };
void cursor_arrow(void)
{
    if (!srv.arrow) {
        srv.arrow = pixbuf_create(12, 19);
        int st;
        uint32_t *px = pixbuf_data(srv.arrow, &st);
        for (int y = 0; y < 19; y++)
            for (int x = 0; arrow[y][x]; x++)
                px[y * st + x] = arrow[y][x] == 'X' ? 0xff0a1322u : arrow[y][x] == '.' ? 0xffffffffu : 0;
    }
    wlr_cursor_set_buffer(srv.cursor, srv.arrow, 0, 0, 1);
    srv.client_cursor = 0;
}

/* What is at the pointer: an app's surface (and where on it), a piece of
 * chrome, a window (its ground). */
static struct wlr_scene_node *at(struct wlr_surface **surf, double *sx, double *sy)
{
    *surf = 0;
    struct wlr_scene_node *n = wlr_scene_node_at(&srv.scene->tree.node, srv.px, srv.py, sx, sy);
    if (n && n->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(n));
        if (ss) *surf = ss->surface;
    }
    return n;
}
static struct view *view_at(struct wlr_scene_node *n)   /* the window a node belongs to */
{
    for (; n; n = n->parent ? &n->parent->node : 0)
        if (n->type == WLR_SCENE_NODE_TREE && n->data) {
            struct view *v;
            wl_list_for_each(v, &srv.views, link) if (v == n->data) return v;
        }
    return 0;
}

static struct { int on; double ox, oy; uint32_t buttons; } held;   /* buttons pressed on an app: it keeps the pointer */
static int in_lock(struct wlr_scene_node *n)            /* a node of the locker's */
{
    for (; n; n = n->parent ? &n->parent->node : 0) if (n == &srv.l_lock->node) return 1;
    return 0;
}
static void motion(uint32_t time)
{
    int ux = dunit(P(srv.px)), uy = dunit(P(srv.py));
    wlr_idle_notifier_v1_notify_activity(srv.idle, srv.seat);
    if (wm_grabbing()) { wm_motion(ux, uy); return; }
    if (held.on && srv.seat->pointer_state.focused_surface) {
        wlr_seat_pointer_notify_motion(srv.seat, time, srv.px - held.ox, srv.py - held.oy);
        return;
    }
    struct wlr_surface *s;
    double sx, sy;
    struct wlr_scene_node *n = at(&s, &sx, &sy);
    if (locked() && !in_lock(n)) s = 0;                  /* (locked: only the locker) */
    if (!locked()) bar_hover(ux, uy);
    if (s) {
        wlr_seat_pointer_notify_enter(srv.seat, s, sx, sy);
        wlr_seat_pointer_notify_motion(srv.seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(srv.seat);
        if (srv.client_cursor) cursor_arrow();
    }
}
void pointer_refocus(void) { motion((uint32_t)(now_ns() / 1000000)); }

static void on_motion(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_motion_event *e = data;
    wlr_cursor_move(srv.cursor, &e->pointer->base, e->delta_x, e->delta_y);
    srv.px = srv.cursor->x; srv.py = srv.cursor->y;
    motion(e->time_msec);
}
static void on_motion_abs(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_motion_absolute_event *e = data;
    wlr_cursor_warp_absolute(srv.cursor, &e->pointer->base, e->x, e->y);
    srv.px = srv.cursor->x; srv.py = srv.cursor->y;
    motion(e->time_msec);
}

static struct { uint64_t t; struct view *v; } last_tab;  /* a double-click on a tab: minimize */
static int super_held(void)
{
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv.seat);
    return kb && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_LOGO);
}
static void press_chrome(struct view *v, int ux, int uy, uint32_t button)
{
    const hit_t *h = hits_at(&v->hits, ux, uy);
    view_focus(v);
    if (!h) return;
    switch (h->kind) {
    case H_WTAB:
        if (button == BTN_RIGHT) { menu_window(v, ux, uy); break; }
        if (button != BTN_LEFT) break;
        if (last_tab.v == v && now_ns() - last_tab.t < 400000000u) { last_tab.v = 0; wm_min(v); break; }
        last_tab.v = v; last_tab.t = now_ns();
        wm_grab(v, 0, ux, uy);
        break;
    case H_WMBTN:
        if (button != BTN_LEFT) break;
        if (h->arg == 1) wm_toggle_max(v);
        else view_close(v);
        break;
    case H_WEDGE:
        if (button == BTN_LEFT) wm_grab(v, h->arg, ux, uy);
        break;
    case H_WIN:
        if (button == BTN_RIGHT && uy < v->y0 + WM_STRIP) menu_window(v, ux, uy);
        break;
    }
}
static void on_button(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_button_event *e = data;
    int ux = dunit(P(srv.px)), uy = dunit(P(srv.py));
    if (mouse_left() && (e->button == BTN_LEFT || e->button == BTN_RIGHT)) e->button ^= BTN_LEFT ^ BTN_RIGHT;   /* (left-handed) */
    wlr_idle_notifier_v1_notify_activity(srv.idle, srv.seat);
    if (e->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (wm_grabbing()) { wm_drop(); pointer_refocus(); return; }
        if (held.buttons & 1u << (e->button & 31)) {
            held.buttons &= ~(1u << (e->button & 31));
            wlr_seat_pointer_notify_button(srv.seat, e->time_msec, e->button, e->state);
            if (!held.buttons) { held.on = 0; pointer_refocus(); }
        }
        return;
    }
    struct wlr_surface *s;
    double sx, sy;
    struct wlr_scene_node *n = at(&s, &sx, &sy);
    if (locked()) {                                      /* (locked: the locker only) */
        if (lock_press(ux, uy)) return;                  /* (or Atlas's own lock screen) */
        if (!s || !in_lock(n)) return;
        lock_refocus();
        held.on = 1; held.ox = srv.px - sx; held.oy = srv.py - sy;
        held.buttons |= 1u << (e->button & 31);
        wlr_seat_pointer_notify_enter(srv.seat, s, sx, sy);
        wlr_seat_pointer_notify_button(srv.seat, e->time_msec, e->button, e->state);
        return;
    }
    struct chrome *c = chrome_at(n);
    if (palette_press(c, ux, uy)) return;               /* (a press elsewhere closes it, and goes on) */
    layers_focus_at(s);                                  /* (a panel that takes the keys; a press elsewhere gives them back) */
    struct view *v = s || (c && c->kind == CH_VIEW) ? view_at(n) : 0;
    if (!v && n && !c) v = view_at(n);                   /* (a window's ground) */
    if (menu_open_now() && !(c && (c->kind == CH_MENU || c->kind == CH_BAR || c->kind == CH_DESK))) menu_close();
    if (c && c->kind != CH_VIEW) { bar_press(c, ux, uy, e->button); return; }
    if (v && super_held() && !v->fs) {                   /* Super+drag: move (left), resize (right) */
        view_focus(v);
        if (e->button == BTN_LEFT) wm_grab(v, 0, ux, uy);
        else if (e->button == BTN_RIGHT) wm_grab(v, (ux < (v->x0 + v->x1) / 2 ? 1 : 2) | (uy < (v->y0 + v->y1) / 2 ? 4 : 8), ux, uy);
        return;
    }
    if (c && v && v->internal && c == &v->body) { view_focus(v); settings_press(v, ux, uy, e->button); return; }
    if (c && v) { press_chrome(v, ux, uy, e->button); return; }
    if (v) view_focus(v);
    if (s) {
        held.on = 1; held.ox = srv.px - sx; held.oy = srv.py - sy;
        held.buttons |= 1u << (e->button & 31);
        wlr_seat_pointer_notify_enter(srv.seat, s, sx, sy);
        wlr_seat_pointer_notify_button(srv.seat, e->time_msec, e->button, e->state);
    }
}
static void on_axis(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_axis_event *e = data;
    wlr_idle_notifier_v1_notify_activity(srv.idle, srv.seat);
    if (mouse_natural()) { e->delta = -e->delta; e->delta_discrete = -e->delta_discrete; }
    if (!locked()) {                                     /* (over Settings' inside: it scrolls) */
        struct wlr_surface *s;
        double sx, sy;
        struct chrome *c = chrome_at(at(&s, &sx, &sy));
        if (c && c->kind == CH_VIEW && c == &((struct view *)c->owner)->body) {
            if (e->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) settings_scroll(c->owner, e->delta);
            return;
        }
    }
    wlr_seat_pointer_notify_axis(srv.seat, e->time_msec, e->orientation, e->delta, e->delta_discrete, e->source, e->relative_direction);
}
static void on_frame(struct wl_listener *l, void *data) { (void)l; (void)data; wlr_seat_pointer_notify_frame(srv.seat); }

static void on_request_cursor(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_seat_pointer_request_set_cursor_event *e = data;
    if (srv.seat->pointer_state.focused_client != e->seat_client) return;
    wlr_cursor_set_surface(srv.cursor, e->surface, e->hotspot_x, e->hotspot_y);
    srv.client_cursor = 1;
}
static void on_selection(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_seat_request_set_selection_event *e = data;
    wlr_seat_set_selection(srv.seat, e->source, e->serial);
}
static void on_primary(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_seat_request_set_primary_selection_event *e = data;
    wlr_seat_set_primary_selection(srv.seat, e->source, e->serial);
}

/* ---- Keyboards. */
struct keyboard { struct wl_list link; struct wlr_keyboard *kb; struct wl_listener mods, key, destroy; };

static int binding(uint32_t mods, xkb_keysym_t sym)   /* -> 1 if it was ours */
{
    if ((mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) == (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) {
        if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
            if (srv.session) wlr_session_change_vt(srv.session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
            return 1;
        }
        if (sym == XKB_KEY_BackSpace) { atlas_quit(); return 1; }
    }
    if (!(mods & WLR_MODIFIER_LOGO)) return 0;
    int shift = mods & WLR_MODIFIER_SHIFT;
    struct view *f = view_focused();
    switch (sym) {
    case XKB_KEY_Return: case XKB_KEY_t: spawn_terminal(); break;
    case XKB_KEY_w: case XKB_KEY_q: view_close(f); break;
    case XKB_KEY_Tab: wm_cycle(); break;
    case XKB_KEY_1: case XKB_KEY_2: case XKB_KEY_3: case XKB_KEY_4:
        if (shift) wm_send(f, (int)(sym - XKB_KEY_1)); else wm_switch((int)(sym - XKB_KEY_1));
        break;
    case XKB_KEY_Up: wm_toggle_max(f); break;
    case XKB_KEY_Down: wm_min(f); break;
    case XKB_KEY_f: if (f) wm_fullscreen(f, !f->fs); break;
    case XKB_KEY_space: if (palette_on()) palette_close(); else palette_open(); break;
    case XKB_KEY_comma: settings_open(); break;
    case XKB_KEY_l: lock_now(); break;
    case XKB_KEY_e: if (shift) atlas_quit(); else return 0; break;
    default: return 0;
    }
    return 1;
}
static void on_key(struct wl_listener *l, void *data)
{
    struct keyboard *k = wl_container_of(l, k, key);
    struct wlr_keyboard_key_event *e = data;
    uint32_t code = e->keycode + 8, mods = wlr_keyboard_get_modifiers(k->kb);
    const xkb_keysym_t *syms;
    xkb_layout_index_t lay = xkb_state_key_get_layout(k->kb->xkb_state, code);
    int n = xkb_keymap_key_get_syms_by_level(k->kb->keymap, code, lay, 0, &syms);   /* (unshifted: Super+Shift+1 is 1) */
    int done = 0;
    wlr_idle_notifier_v1_notify_activity(srv.idle, srv.seat);
    if (e->state == WL_KEYBOARD_KEY_STATE_PRESSED && locked()) {   /* (locked: only another console) */
        const xkb_keysym_t *ts;
        int tn = xkb_state_key_get_syms(k->kb->xkb_state, code, &ts);
        for (int i = 0; i < tn; i++) if (ts[i] >= XKB_KEY_XF86Switch_VT_1 && ts[i] <= XKB_KEY_XF86Switch_VT_12) done = binding(mods, ts[i]);
        if (!done) done = lock_key(xkb_state_key_get_one_sym(k->kb->xkb_state, code), xkb_state_key_get_utf32(k->kb->xkb_state, code));
    } else if (e->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        const xkb_keysym_t *ts, *up;
        int tn = xkb_state_key_get_syms(k->kb->xkb_state, code, &ts);   /* (VT keys come shifted by Ctrl+Alt) */
        for (int i = 0; i < tn && !done; i++) if (ts[i] >= XKB_KEY_XF86Switch_VT_1 && ts[i] <= XKB_KEY_XF86Switch_VT_12) done = binding(mods, ts[i]);
        for (int i = 0; i < n && !done; i++) done = binding(mods, syms[i]);
        int un = xkb_keymap_key_get_syms_by_level(k->kb->keymap, code, lay, 1, &up);   /* (AZERTY: the digits are shifted) */
        for (int i = 0; i < un && !done; i++) if (up[i] >= XKB_KEY_1 && up[i] <= XKB_KEY_4) done = binding(mods, up[i]);
        if (!done && palette_on())
            done = palette_key(xkb_state_key_get_one_sym(k->kb->xkb_state, code), xkb_state_key_get_utf32(k->kb->xkb_state, code));
        if (!done && menu_open_now()) {
            for (int i = 0; i < n; i++) menu_key(syms[i]);
            done = 1;
        }
    }
    if (!done && e->state == WL_KEYBOARD_KEY_STATE_PRESSED && view_focused() && view_focused()->internal && !locked())   /* (Settings' fields) */
        done = settings_key(view_focused(), xkb_state_key_get_one_sym(k->kb->xkb_state, code), xkb_state_key_get_utf32(k->kb->xkb_state, code));
    if (done) return;
    wlr_seat_set_keyboard(srv.seat, k->kb);
    wlr_seat_keyboard_notify_key(srv.seat, e->time_msec, e->keycode, e->state);
}
static void on_mods(struct wl_listener *l, void *data)
{
    (void)data;
    struct keyboard *k = wl_container_of(l, k, mods);
    wlr_seat_set_keyboard(srv.seat, k->kb);
    wlr_seat_keyboard_notify_modifiers(srv.seat, &k->kb->modifiers);
    lock_redraw();                                       /* (Caps Lock, said on the lock screen) */
}
static void on_kb_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct keyboard *k = wl_container_of(l, k, destroy);
    wl_list_remove(&k->mods.link); wl_list_remove(&k->key.link); wl_list_remove(&k->destroy.link); wl_list_remove(&k->link);
    free(k);
}
/* The layout: XKB_DEFAULT_LAYOUT... if set, else the system's (Debian's /etc/default/keyboard). */
static char kb_conf[4][64];
static struct xkb_rule_names kb_names(void)
{
    struct xkb_rule_names r = { 0 };
    if (getenv("XKB_DEFAULT_LAYOUT")) return r;
    static const char *keys[4] = { "XKBMODEL=", "XKBLAYOUT=", "XKBVARIANT=", "XKBOPTIONS=" };
    FILE *f = fopen("/etc/default/keyboard", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f))
        for (int i = 0; i < 4; i++)
            if (!strncmp(line, keys[i], strlen(keys[i]))) {
                char *v = line + strlen(keys[i]);
                v += *v == '"';
                v[strcspn(v, "\"\n")] = 0;
                snprintf(kb_conf[i], sizeof kb_conf[i], "%s", v);
            }
    if (f) fclose(f);
    r.model = kb_conf[0][0] ? kb_conf[0] : NULL;
    r.layout = kb_conf[1][0] ? kb_conf[1] : NULL;
    r.variant = kb_conf[2][0] ? kb_conf[2] : NULL;
    r.options = kb_conf[3][0] ? kb_conf[3] : NULL;
    return r;
}
static void add_keyboard(struct wlr_keyboard *kb)
{
    struct keyboard *k = calloc(1, sizeof *k);
    k->kb = kb;
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = kb_names();
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(kb, km);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    wlr_keyboard_set_repeat_info(kb, 25, 600);
    k->mods.notify = on_mods; wl_signal_add(&kb->events.modifiers, &k->mods);
    k->key.notify = on_key; wl_signal_add(&kb->events.key, &k->key);
    k->destroy.notify = on_kb_destroy; wl_signal_add(&kb->base.events.destroy, &k->destroy);
    wlr_seat_set_keyboard(srv.seat, kb);
    wl_list_insert(&srv.keyboards, &k->link);
}
static void update_caps(void)
{
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&srv.keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(srv.seat, caps);
}
static void on_new_input(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_input_device *d = data;
    if (d->type == WLR_INPUT_DEVICE_KEYBOARD) add_keyboard(wlr_keyboard_from_input_device(d));
    else if (d->type == WLR_INPUT_DEVICE_POINTER) { wlr_cursor_attach_input_device(srv.cursor, d); mouse_apply(d); }
    update_caps();
}
static struct wl_listener new_vptr, new_vkb, activate;
static void on_new_vptr(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_virtual_pointer_v1_new_pointer_event *e = data;
    wlr_cursor_attach_input_device(srv.cursor, &e->new_pointer->pointer.base);
}
static void on_new_vkb(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_virtual_keyboard_v1 *k = data;
    add_keyboard(&k->keyboard);
    update_caps();
}
/* An app asks for one of its windows in front: it comes if the token was
 * made by the app that has the keys (a link clicked, a launcher); else the
 * window only asks for attention (the tray's pill). */
static void on_activate(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_xdg_activation_v1_request_activate_event *e = data;
    struct wlr_xdg_toplevel *t = wlr_xdg_toplevel_try_from_wlr_surface(e->surface);
    struct view *v = t ? view_of(t) : 0;
    if (!v || !v->mapped || v == view_focused()) return;
    struct wlr_surface *ks = srv.seat->keyboard_state.focused_surface, *ts = e->token->surface;
    if (ks && ts && wl_resource_get_client(ks->resource) == wl_resource_get_client(ts->resource)) { wm_show(v); view_focus(v); }
    else bar_attention(v, 1);
}

/* ---- Tests (ATLAS_TEST=FIFO): lines that drive the pointer as a hand
 * would ("move X Y" in layout pixels, "down B", "up B": 1 left, 2 middle,
 * 3 right) and "dump": every window's box to the log. */
static int test_fd = -1;
static char test_buf[512];
static size_t test_n;
static void test_button(int b, int pressed)
{
    struct wlr_pointer_button_event e = { .time_msec = (uint32_t)(now_ns() / 1000000),
        .button = b == 3 ? BTN_RIGHT : b == 2 ? BTN_MIDDLE : BTN_LEFT,
        .state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED };
    wl_signal_emit_mutable(&srv.cursor->events.button, &e);
}
static void test_line(char *l)
{
    double x, y;
    int b;
    if (sscanf(l, "move %lf %lf", &x, &y) == 2) {
        wlr_cursor_warp_closest(srv.cursor, NULL, x, y);
        srv.px = srv.cursor->x; srv.py = srv.cursor->y;
        motion((uint32_t)(now_ns() / 1000000));
    } else if (sscanf(l, "down %d", &b) == 1) test_button(b, 1);
    else if (sscanf(l, "up %d", &b) == 1) test_button(b, 0);
    else if (sscanf(l, "at %lf %lf", &x, &y) == 2) {   /* what is there: a surface, a chrome and its box, a window */
        double ox = srv.px, oy = srv.py, sx, sy;
        srv.px = x; srv.py = y;
        struct wlr_surface *s;
        struct wlr_scene_node *n = at(&s, &sx, &sy);
        struct chrome *c = chrome_at(n);
        const hit_t *h = c && c->hits ? hits_at(c->hits, dunit(P(x)), dunit(P(y))) : 0;
        struct view *v = view_at(n);
        say("test: at %.0f %.0f: %s chrome %d hit %d.%d window %s", x, y, s ? "surface" : n ? "node" : "nothing",
            c ? c->kind : 0, h ? h->kind : 0, h ? h->arg : 0, v ? view_title(v) : "-");
        srv.px = ox; srv.py = oy;
    }
    else if (sscanf(l, "wheel %lf", &x) == 1) {          /* (the wheel: steps of 15, down if positive) */
        struct wlr_pointer_axis_event e = { .time_msec = (uint32_t)(now_ns() / 1000000), .orientation = WL_POINTER_AXIS_VERTICAL_SCROLL,
                                            .delta = x * 15, .delta_discrete = (int32_t)(x * 120), .source = WL_POINTER_AXIS_SOURCE_WHEEL };
        wl_signal_emit_mutable(&srv.cursor->events.axis, &e);
    }
    else if (!strncmp(l, "dump", 4)) {
        struct view *v;
        int i = 0;
        wl_list_for_each(v, &srv.views, link)
            say("test: window %d %s ws %d%s%s%s box %d %d %d %d focus %d", i++, view_title(v), v->ws + 1, v->min ? " min" : "",
                v->max ? " zoomed" : "", v->fs ? " full" : "", v->x0, v->y0, v->x1, v->y1, v == view_focused());
        say("test: workspace %d", wm_workspace() + 1);
    }
    wlr_seat_pointer_notify_frame(srv.seat);           /* (as a device ends each event) */
}
static int test_read(int fd, uint32_t mask, void *data)
{
    (void)mask; (void)data;
    ssize_t n = read(fd, test_buf + test_n, sizeof test_buf - 1 - test_n);
    if (n <= 0) return 0;
    test_n += n;
    char *nl;
    while ((nl = memchr(test_buf, '\n', test_n))) {
        *nl = 0;
        test_line(test_buf);
        test_n -= nl + 1 - test_buf;
        memmove(test_buf, nl + 1, test_n);
    }
    if (test_n == sizeof test_buf - 1) test_n = 0;
    return 0;
}

static int on_sigterm(int sig, void *data) { (void)sig; (void)data; atlas_quit(); return 0; }
static int on_sigusr1(int sig, void *data) { (void)sig; (void)data; lock_now(); return 0; }   /* (swayidle: before-sleep, timeout) */

static void autostart(void)
{
    char path[512], line[1024];
    const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (cfg && *cfg) snprintf(path, sizeof path, "%s/atlas/autostart", cfg);
    else snprintf(path, sizeof path, "%s/.config/atlas/autostart", home ? home : "");
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && line[0] != '#') spawn(line);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *startup = 0;
    int c;
    while ((c = getopt(argc, argv, "s:hv")) != -1) {
        if (c == 's') startup = optarg;
        else if (c == 'v') { puts("atlas_wm 0.1"); return 0; }
        else { printf("usage: %s [-s command]\n  SIEOS's Atlas desktop as a Wayland compositor (README.md)\n", argv[0]); return c == 'h' ? 0 : 1; }
    }
    wlr_log_init(getenv("ATLAS_DEBUG") ? WLR_DEBUG : WLR_ERROR, NULL);
    setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 0);  /* (glass.c reads the scene's frames: never an app's buffer) */
    settings_load();
    srv.scale = 1;
    wl_list_init(&srv.views);
    wl_list_init(&srv.keyboards);

    srv.display = wl_display_create();
    srv.loop = wl_display_get_event_loop(srv.display);
    srv.backend = wlr_backend_autocreate(srv.loop, &srv.session);
    if (!srv.backend) { say("no backend"); return 1; }
    srv.renderer = wlr_renderer_autocreate(srv.backend);
    if (!srv.renderer) { say("no renderer"); return 1; }
    wlr_renderer_init_wl_shm(srv.renderer, srv.display);
    srv.allocator = wlr_allocator_autocreate(srv.backend, srv.renderer);
    if (!srv.allocator) { say("no allocator"); return 1; }

    srv.compositor = wlr_compositor_create(srv.display, 6, srv.renderer);
    wlr_subcompositor_create(srv.display);
    wlr_data_device_manager_create(srv.display);
    wlr_data_control_manager_v1_create(srv.display);
    wlr_primary_selection_v1_device_manager_create(srv.display);
    wlr_viewporter_create(srv.display);
    wlr_presentation_create(srv.display, srv.backend);
    wlr_single_pixel_buffer_manager_v1_create(srv.display);
    wlr_fractional_scale_manager_v1_create(srv.display, 1);
    wlr_screencopy_manager_v1_create(srv.display);
    struct wlr_xdg_activation_v1 *xa = wlr_xdg_activation_v1_create(srv.display);
    activate.notify = on_activate;
    wl_signal_add(&xa->events.request_activate, &activate);

    srv.scene = wlr_scene_create();
    if (wlr_renderer_get_texture_formats(srv.renderer, WLR_BUFFER_CAP_DMABUF)) {
        struct wlr_linux_dmabuf_v1 *dm = wlr_linux_dmabuf_v1_create_with_renderer(srv.display, 4, srv.renderer);
        if (dm) wlr_scene_set_linux_dmabuf_v1(srv.scene, dm);
    }
    struct wlr_scene_tree **layers[] = { &srv.l_desk, &srv.l_bg, &srv.l_bottom, &srv.l_wins, &srv.l_bar, &srv.l_top,
                                         &srv.l_fs, &srv.l_overlay, &srv.l_menu, &srv.l_xmenu, &srv.l_lock };
    for (size_t i = 0; i < sizeof layers / sizeof *layers; i++) *layers[i] = wlr_scene_tree_create(&srv.scene->tree);
    lock_init();
    output_init();
    wlr_xdg_output_manager_v1_create(srv.display, srv.layout);
    view_init();
    toplevels_init();
    layers_init();
    bar_init();

    srv.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(srv.cursor, srv.layout);
    cursor_arrow();
    srv.cursor_motion.notify = on_motion; wl_signal_add(&srv.cursor->events.motion, &srv.cursor_motion);
    srv.cursor_motion_abs.notify = on_motion_abs; wl_signal_add(&srv.cursor->events.motion_absolute, &srv.cursor_motion_abs);
    srv.cursor_button.notify = on_button; wl_signal_add(&srv.cursor->events.button, &srv.cursor_button);
    srv.cursor_axis.notify = on_axis; wl_signal_add(&srv.cursor->events.axis, &srv.cursor_axis);
    srv.cursor_frame.notify = on_frame; wl_signal_add(&srv.cursor->events.frame, &srv.cursor_frame);
    srv.new_input.notify = on_new_input; wl_signal_add(&srv.backend->events.new_input, &srv.new_input);
    srv.seat = wlr_seat_create(srv.display, "seat0");
    update_caps();                                       /* (the pointer is always there) */
    srv.request_cursor.notify = on_request_cursor; wl_signal_add(&srv.seat->events.request_set_cursor, &srv.request_cursor);
    srv.request_set_selection.notify = on_selection; wl_signal_add(&srv.seat->events.request_set_selection, &srv.request_set_selection);
    srv.request_set_primary.notify = on_primary; wl_signal_add(&srv.seat->events.request_set_primary_selection, &srv.request_set_primary);
    xwayland_init();
    if (getenv("ATLAS_TEST")) {                          /* (tests drive it: wlrctl, wtype) */
        struct wlr_virtual_pointer_manager_v1 *vp = wlr_virtual_pointer_manager_v1_create(srv.display);
        new_vptr.notify = on_new_vptr; wl_signal_add(&vp->events.new_virtual_pointer, &new_vptr);
        struct wlr_virtual_keyboard_manager_v1 *vk = wlr_virtual_keyboard_manager_v1_create(srv.display);
        new_vkb.notify = on_new_vkb; wl_signal_add(&vk->events.new_virtual_keyboard, &new_vkb);
        test_fd = open(getenv("ATLAS_TEST"), O_RDWR | O_NONBLOCK | O_CLOEXEC);   /* (RDWR: never at end of file) */
        if (test_fd >= 0) wl_event_loop_add_fd(srv.loop, test_fd, WL_EVENT_READABLE, test_read, 0);
    }

    srv.anim = wl_event_loop_add_timer(srv.loop, anim_tick, 0);
    srv.clock = wl_event_loop_add_timer(srv.loop, clock_tick, 0);
    wl_event_loop_add_signal(srv.loop, SIGTERM, on_sigterm, 0);
    wl_event_loop_add_signal(srv.loop, SIGINT, on_sigterm, 0);
    wl_event_loop_add_signal(srv.loop, SIGUSR1, on_sigusr1, 0);

    const char *sock = wl_display_add_socket_auto(srv.display);
    if (!sock) { say("no socket"); return 1; }
    if (!wlr_backend_start(srv.backend)) { say("the backend did not start"); return 1; }
    setenv("WAYLAND_DISPLAY", sock, 1);
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    setenv("XDG_CURRENT_DESKTOP", "atlas", 1);
    setenv("MOZ_ENABLE_WAYLAND", "1", 1);
    setenv("QT_QPA_PLATFORM", "wayland;xcb", 0);
    setenv("ELECTRON_OZONE_PLATFORM_HINT", "auto", 0);
    say("on WAYLAND_DISPLAY=%s, %d%%", sock, srv.zoom_pct);
    clock_tick(0);
    autostart();
    if (startup) {                                       /* (-s foot: the Deskbar's terminal, as Super+T starts it) */
        size_t n = strlen(startup);
        const char *t = srv.terminal, *b = strrchr(t, '/');
        b = b && (size_t)(b - t) < strcspn(t, " ") ? b + 1 : t;
        if (!strchr(startup, ' ') && !strncmp(b, startup, n) && (b[n] == ' ' || !b[n])) spawn_terminal();
        else if (!strchr(startup, ' ') && term_preset(startup)) spawn(term_preset(startup));   /* (another terminal: still Term) */
        else spawn(startup);
    }
    wl_display_run(srv.display);

    wl_display_destroy_clients(srv.display);
    wl_display_destroy(srv.display);
    return 0;
}
