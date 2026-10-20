/*
 * palette.c - The palette (Super+Space): SIEOS's sia strip as a command
 * palette. What is typed in a clear glass strip at the desk's top, the
 * results under it on milky glass, in groups, the keys on its last line.
 *
 *   ( Atlas  te|                                            )  the strip
 *   ┌──────────────────────────────────────┐
 *   │ APPS                                 │ nothing typed: the apps at once
 *   │ [>] Terminal                     app │ the chosen one lit amber (Up, Down;
 *   │ WINDOWS, FILES, ACTIONS              │ Enter opens it; nothing typed: once chosen)
 *   │ [ ] test.c                ~/projects │ a file: its folder at its right, home as ~
 *   │ RUN: te                           Tab │ last: what is typed, run by sh (Tab)
 *   │ Up, Down: choose  Enter: open  ...   │
 *   └──────────────────────────────────────┘
 *
 * A name holding the letters typed is found, in the order of its list;
 * those letters are underlined in it. Files are the names under $HOME, a
 * few folders deep (no dot-folders): read in short slices on a timer, a
 * bounded number, so a large home never stalls the compositor; kept a
 * minute.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include "atlas.h"

/* ---- Files: the names under $HOME, breadth first (the shallow ones first). */
#define FMAX 16384                                       /* names kept, at most */
#define FPOOL (1 << 20)                                  /* their paths (from $HOME) */
#define FDEPTH 4                                         /* folders deep */
static struct {
    char *pool;
    int used, n, next, cur, walking;                     /* cur: the folder read (-1: $HOME); next: the one after */
    struct { int off, base, depth, dir; } e[FMAX];
    DIR *d;
    struct wl_event_source *t;
    uint64_t done_ns;
} fs;
static void pal_files_grew(void);

static const char *home(void) { const char *h = getenv("HOME"); return h && *h ? h : "/"; }
static void fs_stop(void)
{
    if (fs.d) closedir(fs.d);
    fs.d = 0; fs.walking = 0; fs.done_ns = now_ns();
}
static int fs_step(void)                                 /* a slice: 2 ms or 400 names -> 1 while there is more */
{
    uint64_t t0 = now_ns();
    char p[PATH_MAX];
    for (int k = 0; k < 400 && now_ns() - t0 < 2000000; k++) {
        if (!fs.d) {                                     /* the next folder */
            while (fs.next < fs.n && !(fs.e[fs.next].dir && fs.e[fs.next].depth < FDEPTH)) fs.next++;
            if (fs.next == fs.n) { fs_stop(); return 0; }
            fs.cur = fs.next++;
            snprintf(p, sizeof p, "%s/%s", home(), fs.pool + fs.e[fs.cur].off);
            fs.d = opendir(p);
            continue;
        }
        struct dirent *de = readdir(fs.d);
        if (!de) { closedir(fs.d); fs.d = 0; continue; }
        if (de->d_name[0] == '.') continue;
        int dir = de->d_type == DT_DIR;
        struct stat st;
        if (de->d_type == DT_UNKNOWN && !fstatat(dirfd(fs.d), de->d_name, &st, AT_SYMLINK_NOFOLLOW)) dir = S_ISDIR(st.st_mode);
        const char *up = fs.cur >= 0 ? fs.pool + fs.e[fs.cur].off : "";
        int n = snprintf(fs.pool + fs.used, FPOOL - fs.used, "%s%s%s", up, *up ? "/" : "", de->d_name);
        if (fs.n == FMAX || n + 1 >= FPOOL - fs.used) { fs_stop(); return 0; }   /* (full: enough) */
        fs.e[fs.n] = (typeof(fs.e[0])){ fs.used, fs.used + n - (int)strlen(de->d_name), fs.cur >= 0 ? fs.e[fs.cur].depth + 1 : 1, dir };
        fs.used += n + 1; fs.n++;
    }
    return 1;
}
static int fs_tick(void *data)
{
    (void)data;
    int more = fs_step();
    pal_files_grew();
    if (more) wl_event_source_timer_update(fs.t, 6);      /* (the rest of the time: the compositor's) */
    return 0;
}
static void fs_walk(void)                                /* again, if a minute old */
{
    if (fs.walking || (fs.done_ns && now_ns() - fs.done_ns < 60000000000ull)) return;
    if (!fs.pool && !(fs.pool = malloc(FPOOL))) return;
    if (!fs.t) fs.t = wl_event_loop_add_timer(srv.loop, fs_tick, 0);
    fs.used = fs.n = fs.next = 0; fs.cur = -1;
    fs.d = opendir(home());
    fs.walking = 1;
    wl_event_source_timer_update(fs.t, 1);
}

/* ---- The results: as SIEOS's strip (atlas.c search, draw_strip). */
static int has(const char *s, const char *q)
{
    for (; *s; s++) {
        int i = 0;
        while (q[i] && ((s[i] | 32) == (q[i] | 32))) i++;
        if (!q[i]) return 1;
    }
    return 0;
}
enum { R_APP = 1, R_WIN, R_FILE, R_ACT, R_RUN };
enum { ACT_SETTINGS, ACT_LOCK, ACT_LOGOUT };
static const char *const grp[] = { "", "APPS", "WINDOWS", "FILES", "ACTIONS", "RUN" };
static const char *const acts[] = { "Settings", "Lock", "Log out" };
#define MAXRES 8
static struct {
    int on, sel, picked, n, x0, y0, x1, y1;              /* sel: the result Enter opens; picked: chosen with the arrows */
    char q[128];
    struct { int kind, i; struct view *v; } r[MAXRES];
    struct chrome ch;
    hits_t hits;
    uint64_t grown;                                      /* (files found while it is open: searched again, now and then) */
} pl;

static void res(int kind, int i, struct view *v) { if (pl.n < MAXRES) pl.r[pl.n++] = (typeof(pl.r[0])){ kind, i, v }; }
static const char *win_name(struct view *v, char *t, size_t cap)
{
    ascii(view_title(v), t, cap);
    return *t ? t : view_app(v);
}
static int view_live(struct view *v)                     /* (still there: windows go while it is open) */
{
    struct view *w;
    wl_list_for_each(w, &srv.views, link) if (w == v) return v->mapped;
    return 0;
}
static void search(void)
{
    const char *q = pl.q;
    char t[96];
    pl.n = 0; pl.sel = 0; pl.picked = 0;
    if (!*q) {                                           /* nothing typed yet: the apps, to pick with the arrows */
        for (int i = 0; i < apps_count() && pl.n < 7; i++) res(R_APP, i, 0);
        return;
    }
    for (int i = 0; i < apps_count() && pl.n < 7; i++) if (has(apps_name(i), q)) res(R_APP, i, 0);
    struct view *v;
    wl_list_for_each_reverse(v, &srv.views, link) if (v->mapped && pl.n < 7 && has(win_name(v, t, sizeof t), q)) res(R_WIN, 0, v);
    for (int i = 0, k = 0; i < fs.n && pl.n < 7 && k < 5; i++) if (has(fs.pool + fs.e[i].base, q)) res(R_FILE, i, 0), k++;
    for (int i = 0; i < 3 && pl.n < 7; i++) if (has(acts[i], q)) res(R_ACT, i, 0);
    res(R_RUN, 0, 0);
}

static void draw(void)
{
    if (!pl.on) { chrome_hide(&pl.ch); return; }
    int x0 = pl.x0, y0 = pl.y0, x1 = pl.x1, y1 = pl.y1, qn = strlen(pl.q);
    dl_reset();
    hits_begin(&pl.hits);
    int rx1 = x0 + 900 < x1 ? x0 + 900 : x1, y = y1 + 6, last = 0, h = 16 + 36;   /* (and the keys, at the bottom) */
    for (int i = 0; i < pl.n; i++) { h += 46 + (pl.r[i].kind != last ? 32 : 0); last = pl.r[i].kind; }
    for (int k = 3; k >= 1; k--) dl_shade(x0 - 3 * k + 6, y + 3 * k + 4, rx1 + 3 * k - 6, y + h + 4 * k, 16 + 3 * k, 3);   /* (it floats over the desk) */
    dl_rect(x0, y0, x1, y1, (y1 - y0) / 2, C_CLEAR);     /* the strip */
    dl_frame(x0, y0, x1, y1, (y1 - y0) / 2, 2, C_GTXT);
    hitbox(x0, y0, x1, y1, H_MENU, -1);
    dl_rect(x0 + 6, y0 + 6, x0 + 42 + tws("Atlas", 2), y1 - 6, (y1 - y0 - 12) / 2, C_ACC);   /* (an amber chip) */
    bold(x0 + 24, y0 + 13, "Atlas", C_INK, x1);
    int qx = x0 + 24 + tws("Atlas", 2) + 32, qo = 0;
    while (qo < qn && text_sw(pl.q + qo, qn - qo, SF(2)) > x1 - 40 - qx) qo++;   /* (too long: its end, where the cursor is) */
    if (qn) dl_text(qx, y0 + 13, pl.q + qo, qn - qo, SF(2), C_GTXT, x0, y0, x1 - 20, y1);
    else label(qx + 18, y0 + 13, "an app, a window, a file, or a command", 2, C_GDIM);   /* (after the cursor) */
    int cw = text_sw(pl.q + qo, qn - qo, SF(2));
    dl_rect(qx + 2 + cw, y0 + 11, qx + 12 + cw, y0 + 35, 0, C_ACC);
    dl_rect(x0, y, rx1, y + h, 16, C_GLASS);             /* the results */
    hitbox(x0, y, rx1, y + h, H_MENU, -1);
    y += 8; last = 0;
    for (int i = 0; i < pl.n; i++) {
        typeof(pl.r[0]) *r = &pl.r[i];
        if (r->kind != last) { dl_text(x0 + 26, y + 10, grp[r->kind], strlen(grp[r->kind]), SSS, C_DIM, 0, 0, SW, SH); y += 32; last = r->kind; }
        if (i == pl.sel) dl_rect(x0 + 10, y, rx1 - 10, y + 42, 10, (C_ACC & 0xffffff) | ALPHA(8));   /* (the one Enter opens) */
        char t[96] = "", w[64] = "";
        int ic = I_APP;
        if (r->kind == R_APP) { snprintf(t, sizeof t, "%s", apps_name(r->i)); snprintf(w, sizeof w, "app"); ic = apps_icon(r->i); }
        else if (r->kind == R_WIN) { win_name(r->v, t, sizeof t); snprintf(w, sizeof w, "workspace %d%s", r->v->ws + 1, r->v->min ? ", hidden" : ""); ic = view_icon(r->v); }
        else if (r->kind == R_FILE) {                    /* a file: its folder at the right, the home one as ~ */
            const char *p = fs.pool + fs.e[r->i].off, *b = fs.pool + fs.e[r->i].base;
            snprintf(t, sizeof t, "%s", b);
            snprintf(w, sizeof w, "~%s%.*s", b > p ? "/" : "", b > p ? (int)(b - p - 1) : 0, p);
            ic = fs.e[r->i].dir ? I_FOLDER : I_FILE;
        }
        else if (r->kind == R_ACT) { snprintf(t, sizeof t, "%s", acts[r->i]); ic = r->i == ACT_SETTINGS ? I_SETTINGS : I_APP; }
        else { snprintf(t, sizeof t, "%s", pl.q); snprintf(w, sizeof w, "Tab"); ic = I_TERM; }
        int ww = text_sw(w, strlen(w), SSS);
        icon(ic, x0 + 22, y + 9, 2);
        dl_text(x0 + 58, y + 10, t, strlen(t), SF(2), C_TXT, x0, y, rx1 - 40 - ww, y + 42);
        if (r->kind != R_RUN)                            /* the letters typed: underlined in the accent */
            for (const char *m = t; *m; m++) {
                size_t k = 0;
                while (pl.q[k] && (m[k] | 32) == (pl.q[k] | 32)) k++;
                if (pl.q[k]) continue;
                int ux = x0 + 58 + text_sw(t, m - t, SF(2));
                dl_rect(ux, y + 34, ux + text_sw(m, k, SF(2)), y + 37, 1, C_ACC);
                break;
            }
        dl_text(rx1 - 26 - ww, y + 12, w, strlen(w), SSS, C_DIM, 0, 0, SW, SH);
        hitbox(x0 + 10, y, rx1 - 10, y + 42, H_MENU, i);
        y += 46;
    }
    static const char keys[] = "Up, Down: choose    Enter: open    Tab: run    Esc: close";
    dl_rect(x0 + 16, y + 4, rx1 - 16, y + 5, 0, C_LINE);
    dl_text(x0 + 26, y + 14, keys, sizeof keys - 1, SSS, C_DIM, x0, y, rx1 - 16, y + 40);
    chrome_draw(&pl.ch, dsc(x0 - 10), dsc(y0 - 4), dsc(x1 + 10), dsc(y1 + 6 + h + 16));
}
static void pal_files_grew(void)                         /* (typed, and files came: searched again, every 100 ms) */
{
    if (!pl.on || !pl.q[0] || pl.picked || (fs.walking && now_ns() - pl.grown < 100000000)) return;
    pl.grown = now_ns();
    search();
    draw();
}

/* ---- Open, close, do. */
int palette_on(void) { return pl.on; }
void palette_open(void)
{
    static int made;
    if (!made) { chrome_init(&pl.ch, srv.l_menu, CH_MENU, 0, &pl.hits); made = 1; }
    menu_close();
    struct output *o = output_primary();                 /* (the strip: over the desk's top, beside the Deskbar) */
    box_t A = o ? desk_area(o) : (box_t){ 16, 16, SW - 16, SH - 16 };
    pl.x0 = A.x0; pl.x1 = A.x1; pl.y0 = A.y0 - TAB_H - 8; pl.y1 = pl.y0 + 48;
    pl.on = 1; pl.q[0] = 0;
    fs_walk();
    search();
    draw();
}
void palette_close(void)
{
    if (!pl.on) return;
    pl.on = 0;
    draw();
}
static void act(int i)
{
    if (i < 0 || i >= pl.n) return;
    typeof(pl.r[0]) r = pl.r[i];
    char cmd[PATH_MAX * 2 + 64], p[PATH_MAX], q[PATH_MAX + 64], self[PATH_MAX];
    palette_close();
    switch (r.kind) {
    case R_APP: spawn(apps_cmd(r.i)); break;
    case R_WIN: if (view_live(r.v)) { wm_show(r.v); view_focus(r.v); } break;
    case R_FILE: {                                       /* a folder in Files (atlas-files), a file in its app */
        snprintf(p, sizeof p, "%s/%s", home(), fs.pool + fs.e[r.i].off);
        size_t n = 0;                                    /* (in single quotes, for sh) */
        q[n++] = '\'';
        for (const char *s = p; *s && n < sizeof q - 6; s++) {
            if (*s == '\'') { memcpy(q + n, "'\\''", 4); n += 4; } else q[n++] = *s;
        }
        q[n++] = '\''; q[n] = 0;
        ssize_t k = readlink("/proc/self/exe", self, sizeof self - 16);
        if (k > 0) { self[k] = 0; strcpy(strrchr(self, '/') + 1, "atlas-files"); }
        if (fs.e[r.i].dir && k > 0 && access(self, X_OK) == 0) snprintf(cmd, sizeof cmd, "'%s' %s", self, q);
        else snprintf(cmd, sizeof cmd, "%s %s", fs.e[r.i].dir ? "atlas-files" : "xdg-open", q);
        spawn(cmd);
        break;
    }
    case R_ACT:
        if (r.i == ACT_SETTINGS) settings_open();
        else if (r.i == ACT_LOCK) lock_now();
        else atlas_quit();
        break;
    case R_RUN: spawn(pl.q); break;
    }
    pointer_refocus();
}
int palette_key(xkb_keysym_t sym, uint32_t ch)           /* every key, while it is open */
{
    int n = strlen(pl.q);
    if (sym == XKB_KEY_Escape) { palette_close(); return 1; }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { if (pl.n && (n || pl.picked)) act(pl.sel); return 1; }   /* (empty: only once the arrows chose) */
    if (sym == XKB_KEY_Down || sym == XKB_KEY_Up) {
        if (pl.n) { pl.sel = (pl.sel + (sym == XKB_KEY_Down ? 1 : pl.n - 1)) % pl.n; pl.picked = 1; }
        draw();
        return 1;
    }
    if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) { for (int i = 0; i < pl.n; i++) if (pl.r[i].kind == R_RUN) act(i); return 1; }
    if (sym == XKB_KEY_BackSpace) { if (!n) return 1; pl.q[n - 1] = 0; }
    else if (ch >= 32 && ch < 127 && n + 1 < (int)sizeof pl.q) { pl.q[n] = (char)ch; pl.q[n + 1] = 0; }
    else return 1;
    search();
    draw();
    return 1;
}
int palette_press(struct chrome *c, int x, int y)        /* 1 if taken; a press elsewhere closes it */
{
    if (!pl.on) return 0;
    if (c != &pl.ch) { palette_close(); return 0; }
    const hit_t *h = hits_at(&pl.hits, x, y);
    if (h && h->arg >= 0) act(h->arg);
    return 1;
}
