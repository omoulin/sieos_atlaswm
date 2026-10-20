/*
 * wl.c - atlas-files' window: a Wayland client of its own, no toolkit.
 *
 * One xdg-shell toplevel, decorated by the compositor (xdg-decoration:
 * Atlas draws its tab and frame). Its picture is drawn with Atlas's
 * display list into shared-memory buffers, at the screen's scale (a
 * fractional one too, with wp-fractional-scale and wp-viewporter), so its
 * text is as sharp as the desktop's. The palette (day or night) and the
 * size of things come from ~/.config/atlas/look, as Atlas's, and follow
 * it when Settings changes it (inotify). The pointer is Atlas's arrow;
 * keys come through xkbcommon, repeated as the compositor says.
 *
 * Nothing is drawn but on a change, at most once a frame.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "files.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct zxdg_decoration_manager_v1 *decos;
static struct wp_fractional_scale_manager_v1 *fracs;
static struct wp_viewporter *viewporter;
static struct wl_surface *surf, *cursor_surf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
static struct wp_viewport *viewport;
static struct wl_pointer *ptr;
static struct wl_keyboard *kbd;
static struct wl_buffer *arrow;
static int running = 1, configured, dirty, frame_pending, focused;
static int W = 960, H = 640, pw, ph;                 /* the window (logical); asked by the compositor */
static double scale = 1;                             /* buffer pixels per logical pixel */
static int zoom_pct = 100;
static double px_, py_;                              /* the pointer (logical) */

static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *xst;
static int rep_rate = 25, rep_delay = 600, rep_fd = -1, tim_fd = -1;
static uint32_t rep_key;
static double wheel;
static int mods(void);

uint64_t win_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
int win_focused(void) { return focused; }
void win_redraw(void) { dirty = 1; }
void win_quit(void) { running = 0; }
void win_title(const char *t) { if (top) xdg_toplevel_set_title(top, t); }

/* ---- Buffers: two, in shared memory, of the window's size in pixels. */
struct buf { struct wl_buffer *b; uint32_t *px; int w, h, busy; size_t len; };
static struct buf bufs[2];
static void buf_release(void *data, struct wl_buffer *b) { (void)b; ((struct buf *)data)->busy = 0; }
static const struct wl_buffer_listener buf_listener = { buf_release };
static int shm_fd(size_t len)
{
    int fd = memfd_create("atlas-files", MFD_CLOEXEC);
    if (fd >= 0 && ftruncate(fd, (off_t)len) < 0) { close(fd); fd = -1; }
    return fd;
}
static struct wl_buffer *make_buffer(int w, int h, uint32_t **px, size_t *len)
{
    *len = (size_t)w * h * 4;
    int fd = shm_fd(*len);
    if (fd < 0) return 0;
    *px = mmap(0, *len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (*px == MAP_FAILED) { close(fd); return 0; }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)*len);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}
static struct buf *next_buffer(int w, int h)
{
    for (int i = 0; i < 2; i++) {
        struct buf *b = &bufs[i];
        if (b->busy) continue;
        if (b->b && (b->w != w || b->h != h)) { wl_buffer_destroy(b->b); munmap(b->px, b->len); b->b = 0; }
        if (!b->b) {
            b->b = make_buffer(w, h, &b->px, &b->len);
            if (!b->b) return 0;
            b->w = w; b->h = h;
            wl_buffer_add_listener(b->b, &buf_listener, b);
        }
        return b;
    }
    return 0;
}

/* ---- Drawing: the picture's units are Atlas's (0.75 screen pixels each
 * at 100%), times the scale. */
static void frame_done(void *data, struct wl_callback *cb, uint32_t t);
static const struct wl_callback_listener frame_listener = { frame_done };
static void layout(void)
{
    pw = (int)(W * scale + 0.5); ph = (int)(H * scale + 0.5);
    draw_size(pw, ph);
    draw_zoom((int)(49152 * scale * zoom_pct / 100 + 0.5));
}
static void draw(void)
{
    if (!configured) return;
    layout();
    struct buf *b = next_buffer(pw, ph);
    if (!b) { dirty = 1; return; }
    dl_reset();
    fm_draw();
    dl_render(b->px, pw, 0, 0, pw, ph, GLASS_SOLID);   /* (its menus' glass: as over the desk's middle) */
    b->busy = 1;
    wl_surface_attach(surf, b->b, 0, 0);
    if (viewport) wp_viewport_set_destination(viewport, W, H);
    else wl_surface_set_buffer_scale(surf, (int)scale);
    wl_surface_damage_buffer(surf, 0, 0, pw, ph);
    struct wl_callback *cb = wl_surface_frame(surf);
    wl_callback_add_listener(cb, &frame_listener, 0);
    frame_pending = 1;
    dirty = 0;
    wl_surface_commit(surf);
}
static void frame_done(void *data, struct wl_callback *cb, uint32_t t) { (void)data; (void)t; wl_callback_destroy(cb); frame_pending = 0; }

static int unit(double l) { return dunit((int)(l * scale + 0.5)); }   /* a logical position, in picture units */

/* ---- xdg-shell. */
static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial) { (void)data; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };
static void xs_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(s, serial);
    configured = 1;
    dirty = 1;
}
static const struct xdg_surface_listener xs_listener = { xs_configure };
static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states)
{
    (void)data; (void)t; (void)states;
    if (w > 0) W = w;
    if (h > 0) H = h;
}
static void top_close(void *data, struct xdg_toplevel *t) { (void)data; (void)t; running = 0; }
static void top_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)data; (void)t; (void)w; (void)h; }
static void top_caps(void *data, struct xdg_toplevel *t, struct wl_array *c) { (void)data; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, top_bounds, top_caps };

static void frac_scale(void *data, struct wp_fractional_scale_v1 *f, uint32_t s)
{
    (void)data; (void)f;
    scale = s / 120.0;
    dirty = 1;
}
static const struct wp_fractional_scale_v1_listener frac_listener = { frac_scale };

/* ---- The pointer. */
static void p_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)s;
    wl_pointer_set_cursor(p, serial, cursor_surf, 0, 0);
    px_ = wl_fixed_to_double(x); py_ = wl_fixed_to_double(y);
    fm_motion(unit(px_), unit(py_));
}
static void p_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) { (void)data; (void)p; (void)serial; (void)s; fm_motion(-1000000, -1000000); }
static void p_motion(void *data, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)p; (void)t;
    px_ = wl_fixed_to_double(x); py_ = wl_fixed_to_double(y);
    fm_motion(unit(px_), unit(py_));
}
static void p_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state)
{
    (void)data; (void)p; (void)serial; (void)t;
    int b = button == BTN_LEFT ? 1 : button == BTN_MIDDLE ? 2 : button == BTN_RIGHT ? 3 : 0;
    if (b) fm_button(unit(px_), unit(py_), b, state == WL_POINTER_BUTTON_STATE_PRESSED);
}
static void p_axis(void *data, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v)
{
    (void)data; (void)p; (void)t;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) wheel += wl_fixed_to_double(v) / 15.0;   /* (15: a wheel's step) */
}
static void p_frame(void *data, struct wl_pointer *p)
{
    (void)data; (void)p;
    if (wheel != 0) { fm_wheel(wheel); wheel = 0; }
}
static void p_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_listener = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_source, p_stop, p_discrete };

/* ---- The keyboard. */
int win_mods(void) { return mods(); }
static int mods(void)
{
    if (!xst) return 0;
    return (xkb_state_mod_name_is_active(xst, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0 ? M_CTRL : 0) |
           (xkb_state_mod_name_is_active(xst, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0 ? M_SHIFT : 0) |
           (xkb_state_mod_name_is_active(xst, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0 ? M_ALT : 0);
}
static void key_do(uint32_t code)
{
    xkb_keysym_t sym = xkb_state_key_get_one_sym(xst, code);
    uint32_t ch = xkb_state_key_get_utf32(xst, code);
    fm_key(sym, ch, mods());
}
static void rep_arm(int ms)
{
    struct itimerspec it = { { 0, 0 }, { ms / 1000, (long)(ms % 1000) * 1000000 } };
    if (ms > 0 && rep_rate > 0) it.it_interval.tv_nsec = 1000000000L / rep_rate;
    timerfd_settime(rep_fd, 0, &it, 0);
}
static void k_keymap(void *data, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size)
{
    (void)data; (void)k;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *m = mmap(0, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return;
    struct xkb_keymap *km = xkb_keymap_new_from_string(xkb, m, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(m, size);
    if (!km) return;
    if (xst) xkb_state_unref(xst);
    if (keymap) xkb_keymap_unref(keymap);
    keymap = km;
    xst = xkb_state_new(km);
}
static void k_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys)
{
    (void)data; (void)k; (void)serial; (void)s; (void)keys;
    focused = 1;
    dirty = 1;
}
static void k_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
    (void)data; (void)k; (void)serial; (void)s;
    focused = 0;
    rep_key = 0;
    rep_arm(0);
    dirty = 1;
}
static void k_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state)
{
    (void)data; (void)k; (void)serial; (void)t;
    if (!xst) return;
    uint32_t code = key + 8;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        key_do(code);
        if (xkb_keymap_key_repeats(keymap, code)) { rep_key = code; rep_arm(rep_delay); }
    } else if (rep_key == code) { rep_key = 0; rep_arm(0); }
}
static void k_mods(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{
    (void)data; (void)k; (void)serial;
    if (xst) xkb_state_update_mask(xst, dep, lat, lock, 0, 0, group);
}
static void k_repeat(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay) { (void)data; (void)k; rep_rate = rate; rep_delay = delay; }
static const struct wl_keyboard_listener kbd_listener = { k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat };

static void seat_caps(void *data, struct wl_seat *s, uint32_t caps)
{
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ptr) { ptr = wl_seat_get_pointer(s); wl_pointer_add_listener(ptr, &ptr_listener, 0); }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !kbd) { kbd = wl_seat_get_keyboard(s); wl_keyboard_add_listener(kbd, &kbd_listener, 0); }
}
static void seat_name(void *data, struct wl_seat *s, const char *n) { (void)data; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void reg_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)data;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, MIN(ver, 4));
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) { wm = wl_registry_bind(r, name, &xdg_wm_base_interface, MIN(ver, 3)); xdg_wm_base_add_listener(wm, &wm_listener, 0); }
    else if (!strcmp(iface, wl_seat_interface.name)) { seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(ver, 5)); wl_seat_add_listener(seat, &seat_listener, 0); }
    else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) decos = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) fracs = wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1);
    else if (!strcmp(iface, wp_viewporter_interface.name)) viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
#undef MIN
}
static void reg_remove(void *data, struct wl_registry *r, uint32_t name) { (void)data; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* Atlas's arrow, 12 x 19, white with a dark outline. */
static const char *arrow_map[19] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X",
    "X........X", "X.........X", "X..........X", "X......XXXXX", "X...X..X", "X..XX..X",
    "X.X  X..X", "XX   X..X", "     X..X", "      XX" };
static void make_cursor(void)
{
    uint32_t *px;
    size_t len;
    arrow = make_buffer(12, 19, &px, &len);
    if (!arrow) return;
    for (int y = 0; y < 19; y++)
        for (int x = 0; x < 12; x++) {
            char c = x < (int)strlen(arrow_map[y]) ? arrow_map[y][x] : ' ';
            px[y * 12 + x] = c == 'X' ? 0xff0a1322u : c == '.' ? 0xffffffffu : 0;
        }
    cursor_surf = wl_compositor_create_surface(comp);
    wl_surface_attach(cursor_surf, arrow, 0, 0);
    wl_surface_damage_buffer(cursor_surf, 0, 0, 12, 19);
    wl_surface_commit(cursor_surf);
}

/* The size and the palette, as Atlas has them (~/.config/atlas/look);
 * its folder watched: read again when the file is written (Settings, or
 * Atlas when auto's palette turns at 7:00 and 19:00). */
static char look_dir[512];
static int look_fd = -1;
static void load_look(void)
{
    char path[600], line[128];
    const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (cfg && *cfg) snprintf(look_dir, sizeof look_dir, "%s/atlas", cfg);
    else snprintf(look_dir, sizeof look_dir, "%s/.config/atlas", home ? home : "");
    snprintf(path, sizeof path, "%s/look", look_dir);
    int m = 0;
    zoom_pct = 100;
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "zoom=", 5)) zoom_pct = atoi(line + 5);
        if (!strncmp(line, "night=", 6)) m = atoi(line + 6);
    }
    if (f) fclose(f);
    const char *e = getenv("ATLAS_ZOOM");
    if (e) zoom_pct = atoi(e);
    if (zoom_pct < 50 || zoom_pct > 400) zoom_pct = 100;
    if ((e = getenv("ATLAS_LOOK_NIGHT"))) m = atoi(e);
    draw_night(look_night(m));
}
static void look_changed(void)
{
    char buf[4096];
    int again = 0;
    ssize_t n;
    while ((n = read(look_fd, buf, sizeof buf)) > 0)
        for (char *p = buf; p < buf + n; ) {
            struct inotify_event *e = (struct inotify_event *)p;
            if (e->len && !strcmp(e->name, "look")) again = 1;
            p += sizeof *e + e->len;
        }
    if (again) { load_look(); dirty = 1; }
}

void win_open(const char *title, const char *app_id)
{
    load_look();
    mkdir(look_dir, 0755);                           /* (watched before Atlas first writes in it) */
    look_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (look_fd >= 0 && inotify_add_watch(look_fd, look_dir, IN_CLOSE_WRITE | IN_MOVED_TO) >= 0) win_fd(look_fd, look_changed);
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "atlas-files: no Wayland display (WAYLAND_DISPLAY)\n"); exit(1); }
    xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, 0);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm) { fprintf(stderr, "atlas-files: the compositor lacks xdg-shell\n"); exit(1); }
    rep_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    tim_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    make_cursor();
    surf = wl_compositor_create_surface(comp);
    if (fracs && viewporter) {
        struct wp_fractional_scale_v1 *f = wp_fractional_scale_manager_v1_get_fractional_scale(fracs, surf);
        wp_fractional_scale_v1_add_listener(f, &frac_listener, 0);
        viewport = wp_viewporter_get_viewport(viewporter, surf);
    }
    xsurf = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xsurf, &xs_listener, 0);
    top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(top, &top_listener, 0);
    xdg_toplevel_set_title(top, title);
    xdg_toplevel_set_app_id(top, app_id);
    xdg_toplevel_set_min_size(top, 360, 240);
    if (decos) {                                     /* (the compositor's tab and frame) */
        struct zxdg_toplevel_decoration_v1 *d = zxdg_decoration_manager_v1_get_toplevel_decoration(decos, top);
        zxdg_toplevel_decoration_v1_set_mode(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(surf);
}

/* ---- The loop: the display, the key repeat, a timer, other fds. */
#define MAXFD 4
static struct { int fd; void (*ready)(void); } extra[MAXFD];
static int nextra;
static void (*tim_fire)(void);
void win_fd(int fd, void (*ready)(void))
{
    for (int i = 0; i < nextra; i++) if (extra[i].ready == ready) { extra[i].fd = fd; return; }
    if (nextra < MAXFD) extra[nextra++] = (typeof(extra[0])){ fd, ready };
}
void win_timer(int ms, void (*fire)(void))
{
    tim_fire = fire;
    struct itimerspec it = { { 0, 0 }, { ms / 1000, (long)(ms % 1000) * 1000000 } };
    timerfd_settime(tim_fd, 0, &it, 0);
}

void win_run(void)
{
    while (running) {
        if (dirty && !frame_pending) draw();
        while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
        struct pollfd p[3 + MAXFD] = { { wl_display_get_fd(dpy), POLLIN, 0 }, { rep_fd, POLLIN, 0 }, { tim_fd, POLLIN, 0 } };
        for (int i = 0; i < nextra; i++) p[3 + i] = (struct pollfd){ extra[i].fd, POLLIN, 0 };
        if (poll(p, 3 + nextra, -1) < 0 && errno != EINTR) { wl_display_cancel_read(dpy); break; }
        if (p[0].revents & POLLIN) wl_display_read_events(dpy);
        else wl_display_cancel_read(dpy);
        if (wl_display_dispatch_pending(dpy) < 0) break;
        uint64_t n;
        if ((p[1].revents & POLLIN) && read(rep_fd, &n, sizeof n) > 0 && rep_key && focused) key_do(rep_key);
        if ((p[2].revents & POLLIN) && read(tim_fd, &n, sizeof n) > 0 && tim_fire) tim_fire();
        for (int i = 0; i < nextra; i++) if (p[3 + i].revents & POLLIN) extra[i].ready();
    }
}
