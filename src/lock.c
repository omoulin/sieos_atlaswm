/*
 * lock.c - Screen lockers (ext-session-lock-v1: swaylock, gtklock...),
 * and idling (ext-idle-notify-v1, for swayidle; idle-inhibit, for video
 * players).
 *
 * Locked, every screen is black under the locker's surfaces, over
 * everything; only the locker gets the pointer and the keys, and no key of
 * ours works but Ctrl+Alt+Fn (another console). If the locker dies without
 * unlocking, the screens stay black and locked: start another locker
 * (from another console: WAYLAND_DISPLAY=... swaylock) to get in.
 *
 * Atlas's own lock screen (Super+L, Lock in the menus, SIGUSR1: swayidle's
 * before-sleep) is SIEOS's: the desk, the time large in its middle, the date
 * under it, then a card with your initial in a circle, your name, the
 * password and an arrow to go in. The session goes on behind it (apps run,
 * nothing of them shown, no input reaches them); PAM checks the password in
 * a child process (the "atlas" service if /etc/pam.d has it, else "login"),
 * so a slow check never stops the screens.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <fcntl.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <security/pam_appl.h>
#include "atlas.h"

static struct { struct wlr_session_lock_v1 *lock; int on; struct wl_listener new_surface, unlock, destroy; } L_;
struct lsurf { struct wlr_session_lock_surface_v1 *s; struct wlr_scene_tree *tree; struct wl_listener map, destroy; };

/* Atlas's own: the password typed so far, the line under it, the check running. */
static struct { int on; char user[64], pw[256], msg[80]; pid_t pid; struct wl_event_source *src, *shake; uint64_t shake_t0; } A;
static hits_t lock_hits;

int locked(void) { return L_.on || A.on; }

static void blank(struct output *o, int on)              /* a screen's black, under the locker */
{
    struct wlr_box b;
    wlr_output_layout_get_box(srv.layout, o->wlr, &b);
    if (!o->lockbg) o->lockbg = solid_create(srv.l_lock, 0xff000000u, true);
    wlr_scene_node_lower_to_bottom(&o->lockbg->node);
    if (on && !wlr_box_empty(&b)) solid_place(o->lockbg, P(b.x), P(b.y), P(b.x + b.width), P(b.y + b.height));
    else wlr_scene_node_set_enabled(&o->lockbg->node, false);
}
void lock_output(struct output *o) { blank(o, L_.on); }

static void keys_to(struct wlr_surface *s)
{
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv.seat);
    if (kb) wlr_seat_keyboard_notify_enter(srv.seat, s, kb->keycodes, kb->num_keycodes, &kb->modifiers);
    else wlr_seat_keyboard_notify_enter(srv.seat, s, NULL, 0, NULL);
}
void lock_refocus(void)                                  /* the keys: to the locker on the pointer's screen, or any */
{
    if (!L_.on || !L_.lock) return;
    struct output *o = output_at(srv.px, srv.py);
    struct wlr_session_lock_surface_v1 *s, *pick = 0;
    wl_list_for_each(s, &L_.lock->surfaces, link) {
        if (!s->surface->mapped) continue;
        if (!pick || (o && s->output == o->wlr)) pick = s;
    }
    if (pick) keys_to(pick->surface);
}

static void ls_map(struct wl_listener *l, void *data) { (void)l; (void)data; lock_refocus(); pointer_refocus(); }
static void ls_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct lsurf *x = wl_container_of(l, x, destroy);
    wl_list_remove(&x->map.link);
    wl_list_remove(&x->destroy.link);
    wlr_scene_node_destroy(&x->tree->node);
    free(x);
    lock_refocus();
}
static void on_new_surface(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_session_lock_surface_v1 *s = data;
    struct wlr_box b;
    wlr_output_layout_get_box(srv.layout, s->output, &b);
    struct lsurf *x = calloc(1, sizeof *x);
    x->s = s;
    x->tree = wlr_scene_subsurface_tree_create(srv.l_lock, s->surface);
    wlr_scene_node_set_position(&x->tree->node, b.x, b.y);
    wlr_session_lock_surface_v1_configure(s, b.width, b.height);
    x->map.notify = ls_map; wl_signal_add(&s->surface->events.map, &x->map);
    x->destroy.notify = ls_destroy; wl_signal_add(&s->events.destroy, &x->destroy);
}
static void forget(void)
{
    wl_list_remove(&L_.new_surface.link);
    wl_list_remove(&L_.unlock.link);
    wl_list_remove(&L_.destroy.link);
    L_.lock = 0;
}
static void on_unlock(struct wl_listener *l, void *data)
{
    (void)l; (void)data;
    forget();
    L_.on = 0;
    struct output *o;
    wl_list_for_each(o, &srv.outputs, link) blank(o, 0);
    say("unlocked");
    struct view *v = view_focused();
    if (v && view_surface(v)) keys_to(view_surface(v));
    else wlr_seat_keyboard_notify_clear_focus(srv.seat);
    pointer_refocus();
}
static void on_lock_destroy(struct wl_listener *l, void *data)   /* (the locker died: still locked) */
{
    (void)l; (void)data;
    forget();
    say("the locker is gone: still locked");
}
static void on_new_lock(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_session_lock_v1 *lock = data;
    if (L_.lock || A.on) { wlr_session_lock_v1_destroy(lock); return; }   /* (one locker at a time; Atlas's own is one) */
    L_.lock = lock;
    L_.on = 1;
    L_.new_surface.notify = on_new_surface; wl_signal_add(&lock->events.new_surface, &L_.new_surface);
    L_.unlock.notify = on_unlock; wl_signal_add(&lock->events.unlock, &L_.unlock);
    L_.destroy.notify = on_lock_destroy; wl_signal_add(&lock->events.destroy, &L_.destroy);
    menu_close();
    if (wm_grabbing()) wm_drop();
    struct output *o;
    wl_list_for_each(o, &srv.outputs, link) blank(o, 1);
    wlr_seat_keyboard_notify_clear_focus(srv.seat);
    wlr_seat_pointer_clear_focus(srv.seat);
    wlr_session_lock_v1_send_locked(lock);
    say("locked");
}

/* ---- Atlas's own lock screen. */
static void lock_draw(void)
{
    struct output *o, *pri = output_primary();
    wl_list_for_each(o, &srv.outputs, link) {
        struct wlr_box b;
        wlr_output_layout_get_box(srv.layout, o->wlr, &b);
        if (!A.on || wlr_box_empty(&b)) { chrome_hide(&o->lockc); continue; }
        dl_reset();
        box_t S = desk_screen(o);
        dl_desk(S.x0, S.y0, S.x1, S.y1);
        if (o == pri) {                                  /* the time, the date, the card */
            hits_begin(&lock_hits);
            char ck[16], dt[48];
            time_t t = time(0);
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(ck, sizeof ck, "%H:%M", &tm);
            strftime(dt, sizeof dt, "%A %-d %B", &tm);
            int mx = (S.x0 + S.x1) / 2, my = (S.y0 + S.y1) / 2;
            int cy = my - 330 > S.y0 + 24 ? my - 330 : S.y0 + 24;
            label(mx - tw(strlen(ck), 14) / 2, cy, ck, 14, C_GTXT);
            label(mx - tws(dt, 3) / 2, cy + CH * 14 + 24, dt, 3, C_GTXT);
            uint64_t dt_ns = A.shake_t0 ? now_ns() - A.shake_t0 : 0;   /* refused: the card shakes, 320 ms, dying away */
            double f = dt_ns / 320e6;
            mx += A.shake_t0 && f < 1 ? (int)(14 * sin(f * 6 * M_PI) * (1 - f)) : 0;
            int x0 = mx - 260, x1 = mx + 260, y0 = cy + CH * 14 + 96, y1 = y0 + 300;
            dl_rect(x0, y0, x1, y1, 24, C_CLEAR & ~FROST);   /* (clear, not frosted: glass here would show the windows behind) */
            int ay = y0 + 64;                            /* the initial in a circle */
            char in[2] = { A.user[0] >= 'a' && A.user[0] <= 'z' ? A.user[0] - 32 : A.user[0], 0 };
            dl_rect(mx - 40, ay - 40, mx + 40, ay + 40, 40, C_ACC);
            label(mx - tws(in, 3) / 2, ay - 17, in, 3, C_INK);
            label(mx - tws(in, 3) / 2 + 1, ay - 17, in, 3, C_INK);
            bold(mx - tws(A.user, 2) / 2, y0 + 124, A.user, C_GTXT, x1);
            int fx0 = x0 + 50, fx1 = x1 - 112, py = y0 + 168;   /* the password, the arrow at its right */
            char st[64];
            int n = 0;
            for (const char *p = A.pw; *p && n < (int)sizeof st - 1; p++) if ((*p & 0xc0) != 0x80) st[n++] = '*';
            dl_rect(fx0, py, fx1, py + 50, 10, C_FIELD);
            dl_frame(fx0, py, fx1, py + 50, 10, 2, C_ACC);
            if (n) dl_text(fx0 + 20, py + 14, st, n, SF(2), C_TXT, fx0, py, fx1 - 10, py + 50);
            else label(fx0 + 28, py + 14, "Password", 2, C_DIM);
            int cw = n ? text_sw(st, n, SF(2)) : 0;
            if (!A.pid) dl_rect(fx0 + 21 + cw, py + 12, fx0 + 24 + cw, py + 38, 0, C_ACC);   /* (the caret) */
            dl_rect(x1 - 100, py, x1 - 50, py + 50, 25, C_ACC);   /* the arrow: go in */
            dl_rect(x1 - 88, py + 23, x1 - 64, py + 27, 0, C_INK);
            for (int k = 0; k < 9; k++) {
                dl_rect(x1 - 72 + k, py + 16 + k, x1 - 69 + k, py + 18 + k, 0, C_INK);
                dl_rect(x1 - 72 + k, py + 32 - k, x1 - 69 + k, py + 34 - k, 0, C_INK);
            }
            hitbox(x1 - 100, py, x1 - 50, py + 50, H_LOCKGO, 0);
            struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv.seat);
            int caps = kb && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_CAPS);
            const char *hint = A.msg[0] ? A.msg : caps ? "Caps Lock is on" : "Locked: your password opens it";
            label(mx - tws(hint, 2) / 2, y1 - 54, hint, 2, A.msg[0] || caps ? C_ACC : C_GDIM);
            if (A.msg[0] && caps) label(mx - tws("Caps Lock is on", 2) / 2, y1 - 30, "Caps Lock is on", 2, C_ACC);   /* (on clear glass: amber reads, red does not) */
        }
        int px0 = P(b.x), py0 = P(b.y);
        chrome_draw(&o->lockc, px0, py0, px0 + P(b.width), py0 + P(b.height));
    }
}
void lock_redraw(void) { if (A.on) lock_draw(); }

void lock_now(void)
{
    if (locked()) return;
    struct passwd *pw = getpwuid(getuid());
    const char *u = pw ? pw->pw_name : getenv("USER");
    snprintf(A.user, sizeof A.user, "%s", u ? u : "?");
    explicit_bzero(A.pw, sizeof A.pw);
    A.msg[0] = 0;
    A.on = 1;
    menu_close();
    if (wm_grabbing()) wm_drop();
    wlr_seat_keyboard_notify_clear_focus(srv.seat);
    wlr_seat_pointer_clear_focus(srv.seat);
    cursor_arrow();
    lock_draw();
    say("locked: %s", A.user);
}
static void unlock(void)
{
    A.on = 0;
    explicit_bzero(A.pw, sizeof A.pw);
    lock_draw();                                         /* (hidden) */
    say("unlocked");
    struct view *v = view_focused();
    if (v && view_surface(v)) keys_to(view_surface(v));
    pointer_refocus();
}

/* The check: a child asks PAM and says 1 (yes), 0 (no) or 2 (PAM could
 * not tell) on a pipe. */
static int shake_tick(void *data)
{
    (void)data;
    if (now_ns() - A.shake_t0 < 320000000u) wl_event_source_timer_update(A.shake, 16);
    else A.shake_t0 = 0;
    lock_draw();
    return 0;
}

static int conv(int n, const struct pam_message **m, struct pam_response **r, void *pw)
{
    struct pam_response *a = calloc(n, sizeof *a);
    if (!a) return PAM_BUF_ERR;
    for (int i = 0; i < n; i++)
        if (m[i]->msg_style == PAM_PROMPT_ECHO_OFF || m[i]->msg_style == PAM_PROMPT_ECHO_ON) a[i].resp = strdup(pw);
    *r = a;
    return PAM_SUCCESS;
}
static char check(const char *user, const char *pw)
{
    const char *svc = !access("/etc/pam.d/atlas", F_OK) || !access("/usr/lib/pam.d/atlas", F_OK) ? "atlas" : "login";
    struct pam_conv c = { conv, (void *)pw };
    pam_handle_t *h;
    if (pam_start(svc, user, &c, &h) != PAM_SUCCESS) return 2;
    int r = pam_authenticate(h, 0);
    pam_end(h, r);
    return r == PAM_SUCCESS ? 1 : r == PAM_AUTH_ERR || r == PAM_USER_UNKNOWN || r == PAM_MAXTRIES ? 0 : 2;
}
static int checked(int fd, uint32_t mask, void *data)
{
    (void)mask; (void)data;
    char r = 2;
    if (read(fd, &r, 1) != 1) r = 2;
    wl_event_source_remove(A.src);
    A.src = 0;
    close(fd);
    waitpid(A.pid, NULL, 0);
    A.pid = 0;
    if (r == 1) { unlock(); return 0; }
    snprintf(A.msg, sizeof A.msg, "%s", r ? "The password could not be checked (PAM)" : "Wrong password.");
    say("unlock %s: %s", A.user, r ? "PAM failed" : "refused");
    if (!A.shake) A.shake = wl_event_loop_add_timer(srv.loop, shake_tick, 0);
    A.shake_t0 = now_ns();
    shake_tick(0);
    return 0;
}
static void submit(void)
{
    int p[2];
    if (A.pid || !A.pw[0] || pipe2(p, O_CLOEXEC) < 0) return;
    pid_t k = fork();
    if (k == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGCHLD, SIG_DFL);                        /* (pam_unix waits for its helper) */
        char r = check(A.user, A.pw);
        if (write(p[1], &r, 1) != 1) _exit(1);
        _exit(0);
    }
    close(p[1]);
    explicit_bzero(A.pw, sizeof A.pw);
    if (k < 0) { close(p[0]); return; }
    A.pid = k;
    A.src = wl_event_loop_add_fd(srv.loop, p[0], WL_EVENT_READABLE, checked, 0);
    snprintf(A.msg, sizeof A.msg, "Checking...");
    lock_draw();
}
int lock_key(xkb_keysym_t sym, uint32_t ch)
{
    if (!A.on) return 0;
    if (A.pid) return 1;                                 /* (being checked) */
    size_t n = strlen(A.pw);
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { submit(); return 1; }
    if (sym == XKB_KEY_BackSpace) { while (n && (A.pw[n - 1] & 0xc0) == 0x80) n--; if (n) n--; A.pw[n] = 0; }
    else if (sym == XKB_KEY_Escape) explicit_bzero(A.pw, sizeof A.pw);
    else if (ch >= 32 && ch != 127) {                    /* (the character, in UTF-8) */
        char u[4];
        int k = ch < 0x80 ? 1 : ch < 0x800 ? 2 : ch < 0x10000 ? 3 : 4;
        if (k == 1) u[0] = ch;
        else {
            for (int i = k - 1; i > 0; i--, ch >>= 6) u[i] = 0x80 | (ch & 0x3f);
            u[0] = (0xf00 >> k) | ch;
        }
        if (n + k >= sizeof A.pw) return 1;
        memcpy(A.pw + n, u, k);
        A.pw[n + k] = 0;
    } else return 1;
    A.msg[0] = 0;
    lock_draw();
    return 1;
}
int lock_press(int x, int y)
{
    if (!A.on) return 0;
    const hit_t *h = hits_at(&lock_hits, x, y);
    if (h && h->kind == H_LOCKGO) submit();
    return 1;
}

/* ---- Idling: inhibited while an inhibitor (a video playing) lives. */
static int inhibitors;
static struct wl_listener new_lock, new_inhibitor, mgr_destroy;
struct inhib { struct wl_listener destroy; };
static void inhib_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct inhib *i = wl_container_of(l, i, destroy);
    wl_list_remove(&i->destroy.link);
    free(i);
    wlr_idle_notifier_v1_set_inhibited(srv.idle, --inhibitors > 0);
}
static void on_new_inhibitor(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_idle_inhibitor_v1 *in = data;
    struct inhib *i = calloc(1, sizeof *i);
    i->destroy.notify = inhib_destroy;
    wl_signal_add(&in->events.destroy, &i->destroy);
    wlr_idle_notifier_v1_set_inhibited(srv.idle, ++inhibitors > 0);
}

static void on_mgr_destroy(struct wl_listener *l, void *data)   /* (the end: wlroots wants us gone first) */
{
    (void)l; (void)data;
    wl_list_remove(&new_lock.link);
    wl_list_remove(&mgr_destroy.link);
}

void lock_init(void)
{
    struct wlr_session_lock_manager_v1 *m = wlr_session_lock_manager_v1_create(srv.display);
    new_lock.notify = on_new_lock;
    wl_signal_add(&m->events.new_lock, &new_lock);
    mgr_destroy.notify = on_mgr_destroy;
    wl_signal_add(&m->events.destroy, &mgr_destroy);
    srv.idle = wlr_idle_notifier_v1_create(srv.display);
    struct wlr_idle_inhibit_manager_v1 *im = wlr_idle_inhibit_v1_create(srv.display);
    new_inhibitor.notify = on_new_inhibitor;
    wl_signal_add(&im->events.new_inhibitor, &new_inhibitor);
}
