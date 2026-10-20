/*
 * settings.c - The Settings window: a window of atlas_wm's own, its inside
 * drawn like the rest of the chrome (from SIEOS's user/atlas/settings.c).
 * Its sections are listed in a sidebar (the chosen one lit), the chosen
 * one's rows on a card beside it, under its title; a narrow window shows
 * every section, one after the other. An amount is a slider (a click
 * along it picks the nearest stop), a long list a dropdown, any other
 * choice a segmented control, its options joined (the chosen one filled).
 * Amber only means chosen. The wheel scrolls. Everything applies at once
 * and is kept in ~/.config/atlas.
 *
 *   Screens    each screen's mode and scale (100% to 200%, fractional
 *              ones included). A new one shows at once and goes back by
 *              itself after 15 s unless kept, so a monitor that cannot show
 *              it is never a dead end. Kept in screens.
 *   Look       the size of the desktop's own parts, 50% to 400%; the
 *              palette: day, night, or auto by the clock (night from 19:00
 *              to 7:00). Kept in look (zoom=, night=), which atlas-files
 *              follows.
 *   Mouse      the pointer's speed, left- or right-handed buttons, the
 *              wheel's direction, tap to click (touchpads). Kept in mouse.
 *   Apps       the apps of the cube's menu, and which of them are docked
 *              (a row on the Deskbar): each one moved up or down, its name
 *              and command edited, removed; one installed added (found by
 *              its name among the .desktop files), or one running (its
 *              command read from the running program). Kept in apps.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <libinput.h>
#include <dirent.h>
#include <math.h>
#include <sys/stat.h>
#include <wlr/backend/libinput.h>
#include <wlr/xwayland.h>
#include "atlas.h"

#define ROW 54                                   /* a row of buttons */
enum { S_BODY, S_ZOOM = 20, S_SPEED = 40, S_HAND = 50, S_WHEEL = 60, S_TAP = 70, S_NIGHT = 80,
       S_KEEP = 90, S_BACK, S_PAGE = 100, S_DROP = 110, S_MODE = 1000, S_SCALE = 2000, S_UP = 4000, S_DOWN = 5000, S_REM = 6000, S_ADD = 7000,
       S_DOCK = 8000, S_EDIT = 9000, S_DONE = 10000, S_CANCEL, S_FIELD = 10100, S_FOUND = 11000 };
static const int zooms[] = { 50, 75, 100, 125, 150, 200, 300, 400 };
#define NZOOM 8
static const double scales[] = { 1, 1.25, 1.5, 1.75, 2 };
#define NSCALE 5
static const double speeds[] = { -0.5, 0, 0.4, 0.8 };   /* (libinput's acceleration) */

static struct view *win;                         /* the window, or 0 */
static struct { int speed, left, natural, tap; } mouse = { 1, 0, 0, 1 };
static int look_mode;                            /* the palette: 0 day, 1 night, 2 auto (by the clock) */

/* ---- Kept settings: small key=value files. */
static FILE *conf_open(const char *name, const char *mode)
{
    char path[512];
    config_path(name, path, sizeof path);
    if (*mode == 'w') {                          /* (its folder first) */
        char dir[512];
        snprintf(dir, sizeof dir, "%s", path);
        for (char *p = dir + 1; (p = strchr(p, '/')); p++) { *p = 0; mkdir(dir, 0755); *p = '/'; }
    }
    return fopen(path, mode);
}
static void look_save(void)
{
    FILE *f = conf_open("look", "w");
    if (!f) return;
    fprintf(f, "zoom=%d\nnight=%d\n", srv.zoom_pct, look_mode);
    fclose(f);
}
static void mouse_save(void)
{
    FILE *f = conf_open("mouse", "w");
    if (!f) return;
    fprintf(f, "speed=%d\nleft=%d\nnatural=%d\ntap=%d\n", mouse.speed, mouse.left, mouse.natural, mouse.tap);
    fclose(f);
}

/* Screens: "NAME WIDTHxHEIGHT@mHz SCALE" lines, applied when one appears. */
#define MAXSCR 8
static struct { char name[32]; int w, h, r; double scale; } kept[MAXSCR];
static int nkept;
static void screens_save(void)
{
    FILE *f = conf_open("screens", "w");
    if (!f) return;
    fputs("# Each screen: name, mode (width x height @ refresh in mHz), scale (written by Settings)\n", f);
    struct output *o;
    nkept = 0;
    wl_list_for_each(o, &srv.outputs, link) {
        struct wlr_output *wo = o->wlr;
        fprintf(f, "%s %dx%d@%d %.2f\n", wo->name, wo->width, wo->height, wo->refresh, wo->scale);
        if (nkept < MAXSCR) {
            snprintf(kept[nkept].name, sizeof kept[0].name, "%s", wo->name);
            kept[nkept].w = wo->width; kept[nkept].h = wo->height; kept[nkept].r = wo->refresh; kept[nkept].scale = wo->scale;
            nkept++;
        }
    }
    fclose(f);
}
void settings_output(struct wlr_output *wo, struct wlr_output_state *st)
{
    for (int i = 0; i < nkept; i++) {
        if (strcmp(kept[i].name, wo->name)) continue;
        struct wlr_output_mode *m, *best = 0;
        wl_list_for_each(m, &wo->modes, link)
            if (m->width == kept[i].w && m->height == kept[i].h && (!best || abs(m->refresh - kept[i].r) < abs(best->refresh - kept[i].r))) best = m;
        if (best) wlr_output_state_set_mode(st, best);
        if (kept[i].scale >= 0.5 && kept[i].scale <= 4) wlr_output_state_set_scale(st, (float)kept[i].scale);
    }
}

void settings_load(void)
{
    char line[256];
    srv.zoom_pct = 100;
    FILE *f = conf_open("look", "r");                    /* (an older look= or web= line: ignored) */
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "zoom=", 5)) srv.zoom_pct = atoi(line + 5);
        if (!strncmp(line, "night=", 6)) look_mode = atoi(line + 6);
    }
    if (f) fclose(f);
    const char *e = getenv("ATLAS_ZOOM");
    if (e) srv.zoom_pct = atoi(e);
    if (srv.zoom_pct < 50 || srv.zoom_pct > 400) srv.zoom_pct = 100;
    if ((e = getenv("ATLAS_LOOK_NIGHT"))) look_mode = atoi(e);
    if (look_mode < 0 || look_mode > 2) look_mode = 0;
    draw_night(look_night(look_mode));
    if ((f = conf_open("mouse", "r"))) {
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "speed=", 6)) mouse.speed = atoi(line + 6) & 3;
            if (!strncmp(line, "left=", 5)) mouse.left = atoi(line + 5) != 0;
            if (!strncmp(line, "natural=", 8)) mouse.natural = atoi(line + 8) != 0;
            if (!strncmp(line, "tap=", 4)) mouse.tap = atoi(line + 4) != 0;
        }
        fclose(f);
    }
    if ((f = conf_open("screens", "r"))) {
        while (fgets(line, sizeof line, f) && nkept < MAXSCR) {
            if (line[0] == '#') continue;
            if (sscanf(line, "%31s %dx%d@%d %lf", kept[nkept].name, &kept[nkept].w, &kept[nkept].h, &kept[nkept].r, &kept[nkept].scale) == 5) nkept++;
        }
        fclose(f);
    }
}

/* ---- The mouse: libinput's settings on each pointer (the buttons and the
 * wheel are swapped by main.c for every kind of pointer). */
int mouse_left(void) { return mouse.left; }
int mouse_natural(void) { return mouse.natural; }
struct ptr { struct wl_list link; struct wlr_input_device *d; struct wl_listener destroy; };
static struct wl_list ptrs = { &ptrs, &ptrs };
static void ptr_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct ptr *p = wl_container_of(l, p, destroy);
    wl_list_remove(&p->destroy.link);
    wl_list_remove(&p->link);
    free(p);
}
static void apply_one(struct wlr_input_device *d)
{
    if (!wlr_input_device_is_libinput(d)) return;
    struct libinput_device *li = wlr_libinput_get_device_handle(d);
    if (libinput_device_config_accel_is_available(li)) libinput_device_config_accel_set_speed(li, speeds[mouse.speed]);
    if (libinput_device_config_tap_get_finger_count(li) > 0)
        libinput_device_config_tap_set_enabled(li, mouse.tap ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
}
void mouse_apply(struct wlr_input_device *d)
{
    if (d) {
        struct ptr *p = calloc(1, sizeof *p);
        p->d = d;
        p->destroy.notify = ptr_destroy;
        wl_signal_add(&d->events.destroy, &p->destroy);
        wl_list_insert(&ptrs, &p->link);
        apply_one(d);
        return;
    }
    struct ptr *p;
    wl_list_for_each(p, &ptrs, link) apply_one(p->d);
}

/* ---- A new mode or scale, kept or not: back by itself after 15 s. */
static struct { int on; struct wlr_output *o; int w, h, r; float scale; struct wl_event_source *t; } back;
static void set_screen(struct wlr_output *o, int w, int h, int r, float scale)
{
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    struct wlr_output_mode *m;
    wl_list_for_each(m, &o->modes, link)
        if (m->width == w && m->height == h && m->refresh == r) { wlr_output_state_set_mode(&st, m); break; }
    wlr_output_state_set_scale(&st, scale);
    if (!wlr_output_commit_state(o, &st)) say("settings: %s refused %dx%d at %.2f", o->name, w, h, scale);
    wlr_output_state_finish(&st);
}
static int still(struct wlr_output *o) { struct output *x; wl_list_for_each(x, &srv.outputs, link) if (x->wlr == o) return 1; return 0; }
static void go_back(void)
{
    if (back.on && still(back.o)) set_screen(back.o, back.w, back.h, back.r, back.scale);
    back.on = 0;
    if (win) wm_draw(win);
}
static int countdown(void *data) { (void)data; say("settings: not kept, back"); go_back(); return 0; }
static void try_screen(struct wlr_output *o, int w, int h, int r, float scale)
{
    if (!back.on) { back.o = o; back.w = o->width; back.h = o->height; back.r = o->refresh; back.scale = o->scale; }
    else if (back.o != o) { go_back(); back.o = o; back.w = o->width; back.h = o->height; back.r = o->refresh; back.scale = o->scale; }
    back.on = 1;
    if (!back.t) back.t = wl_event_loop_add_timer(srv.loop, countdown, 0);
    wl_event_source_timer_update(back.t, 15000);
    set_screen(o, w, h, r, scale);
}

/* ---- Day or night: everything drawn again in the other palette, as for a
 * new size (the desk, the Deskbar, every window's chrome), the windows'
 * grounds coloured again. */
static void look_apply(void)
{
    draw_night(look_night(look_mode));
    struct view *v;
    wl_list_for_each(v, &srv.views, link) if (v->ground) solid_color(v->ground, 0xff000000u | C_PANEL);
    bar_layout();
}
void settings_clock(void)
{
    if (look_night(look_mode) == draw_night_get()) return;
    look_apply();
    look_save();                                 /* (written again: atlas-files looks at the clock too) */
}

/* ---- Drawing. Wide enough: the sections in a sidebar, the chosen one's
 * rows on a card beside it; narrow: every section, one after the other.
 * Rows go from the top (scrolled), only those wholly in view are drawn; a
 * dry pass (nothing drawn: fits() says no) measures the card first. */
#define SBW 200                                  /* the sidebar */
static int X0, Y0, X1, Y1, cy, lx, cx, dry, paged, page, drop = -1;
static int fits(int h) { return !dry && cy >= Y0 + 4 && cy + h <= Y1 - 6; }
static void title(int x, const char *s)          /* (bigger, bold, in the text's colour) */
{
    dl_text(x, cy, s, strlen(s), STS, C_TXT, X0, Y0, X1, Y1);
    dl_text(x + 1, cy, s, strlen(s), STS, C_TXT, X0, Y0, X1, Y1);
}
static void heading(const char *s)               /* a section's title (in the sidebar instead, when there is one) */
{
    if (paged) return;
    cy += 14;
    if (fits(34)) title(lx, s);
    cy += 44;
}
static void group(const char *s)                 /* a heading inside a section: small, dim */
{
    if (fits(26)) dl_text(lx, cy, s, strlen(s), SSS, C_DIM, X0, Y0, X1, Y1);
    cy += 30;
}
static void row_label(const char *s)
{
    if (cx == lx) { if (fits(30)) dl_text(lx, cy, s, strlen(s), SF(2), C_DIM, X0, Y0, X1, Y1); cy += 34; }
    else if (fits(42)) dl_text(lx, cy + 10, s, strlen(s), SF(2), C_DIM, X0, Y0, cx - 12, Y1);
}
static int pill(int x, int y, const char *s, int on, int arg)   /* -> its right edge */
{
    int w = tws(s, 2) + 28;
    dl_rect(x, y, x + w, y + 42, 21, on ? C_ACC : C_FIELD);
    dl_frame(x, y, x + w, y + 42, 21, 1, on ? C_ACC : C_LINE);
    dl_text(x + 14, y + 10, s, strlen(s), SF(2), on ? C_INK : C_TXT, X0, Y0, X1, Y1);
    hitbox(x, y, x + w, y + 42, H_WIN, arg);
    return x + w;
}
static void chevron(int x, int y, int up, uint32_t c)   /* centred at (x, y), 16 wide */
{
    for (int j = 0; j <= 7; j++) {
        int yy = up ? y + 4 - j : y - 4 + j;
        dl_rect(x - 8 + j, yy - 1, x - 5 + j, yy + 2, 0, c);
        dl_rect(x + 5 - j, yy - 1, x + 8 - j, yy + 2, 0, c);
    }
}
/* One choice: its options joined in a segmented control (round at its
 * two ends, a line between options), wrapped onto more rows if narrow. */
#define MAXCH 12
static void segment(const char *const *s, int a, int b, int on, int base, const int *xs, const int *ws)
{
    int x0 = xs[a], x1 = xs[b] + ws[b], y = cy;
    dl_rect(x0, y, x1, y + 42, 21, C_FIELD);
    for (int i = a; i <= b; i++) {
        int x = xs[i], w = ws[i];
        if (i == on) {                           /* the chosen one: filled, round where the control is */
            if (a == b) dl_rect(x, y, x + w, y + 42, 21, C_ACC);
            else if (i == a) { dl_rect(x, y, x + w, y + 42, 21, C_ACC); dl_rect(x + 21, y, x + w, y + 42, 0, C_ACC); }
            else if (i == b) { dl_rect(x, y, x + w, y + 42, 21, C_ACC); dl_rect(x, y, x + w - 21, y + 42, 0, C_ACC); }
            else dl_rect(x, y, x + w, y + 42, 0, C_ACC);
        } else if (i > a && i - 1 != on) dl_rect(x, y + 9, x + 1, y + 33, 0, C_LINE);   /* (between two options) */
        dl_text(x + 14, y + 10, s[i], strlen(s[i]), SF(2), i == on ? C_INK : C_TXT, X0, Y0, X1, Y1);
        hitbox(x, y, x + w, y + 42, H_WIN, base + i);
    }
    dl_frame(x0, y, x1, y + 42, 21, 1, C_LINE);
}
static void choices(const char *name, const char *const *s, int n, int on, int base)
{
    row_label(name);
    int xs[MAXCH], ws[MAXCH], rows[MAXCH], x = cx, r = 0;
    if (n > MAXCH) n = MAXCH;
    for (int i = 0; i < n; i++) {                /* where each option goes */
        ws[i] = tws(s[i], 2) + 28;
        if (x > cx && x + ws[i] > X1 - 24) { x = cx; r++; }
        xs[i] = x; rows[i] = r;
        x += ws[i];
    }
    for (int a = 0, b; a < n; a = b + 1) {       /* each row of them: one control */
        for (b = a; b + 1 < n && rows[b + 1] == rows[a]; b++) ;
        if (fits(42)) segment(s, a, b, on, base, xs, ws);
        if (b + 1 < n) cy += ROW;
    }
    cy += ROW;
}
/* An amount: a slider of n stops, each a click box over its part of the
 * track; the chosen one k (-1: none, no knob), its name at the right. */
static void slider(const char *name, const char *const *s, int n, int k, int base)
{
    row_label(name);
    if (fits(42)) {
        int x0 = cx + 14, x1 = X1 - 60 - tws("Faster", 2), y = cy + 21, half = (x1 - x0) / (2 * (n - 1));
        if (x1 > x0 + 560) x1 = x0 + 560;        /* (not too long) */
        int kx = k < 0 ? x0 : x0 + (x1 - x0) * k / (n - 1);
        dl_rect(x0 - 3, y - 3, x1 + 3, y + 3, 3, C_BAR);
        if (k >= 0) dl_rect(x0 - 3, y - 3, kx + 3, y + 3, 3, C_ACC);
        for (int i = 0; i < n; i++) {            /* (each stop marked under the track) */
            int sx = x0 + (x1 - x0) * i / (n - 1);
            dl_rect(sx - 2, y + 14, sx + 2, y + 18, 2, k >= i ? C_ACC : C_LINE);
            hitbox(sx - half - (i ? 0 : 16), cy, sx + half + 1 + (i < n - 1 ? 0 : 16), cy + 42, H_WIN, base + i);
        }
        if (k >= 0) { dl_rect(kx - 12, y - 12, kx + 12, y + 12, 12, C_FIELD); dl_frame(kx - 12, y - 12, kx + 12, y + 12, 12, 2, C_ACC); }
        if (k >= 0) dl_text(x1 + 30, cy + 10, s[k], strlen(s[k]), SF(2), C_TXT, X0, Y0, X1, Y1);
    }
    cy += ROW;
}
/* A long list: a dropdown showing the chosen one; open (id), its options
 * listed under it, the chosen one marked. */
static void dropdown(const char *name, const char *const *s, int n, int on, int base, int id)
{
    row_label(name);
    int w = 0;
    for (int i = 0; i < n; i++) if (tws(s[i], 2) > w) w = tws(s[i], 2);
    int x1 = cx + w + 72 < X1 - 24 ? cx + w + 72 : X1 - 24, open = drop == id;
    if (fits(42)) {
        dl_rect(cx, cy, x1, cy + 42, 10, C_FIELD);
        dl_frame(cx, cy, x1, cy + 42, 10, 1, C_LINE);
        if (on >= 0) dl_text(cx + 14, cy + 10, s[on], strlen(s[on]), SF(2), C_TXT, cx, Y0, x1 - 40, Y1);
        chevron(x1 - 24, cy + 21, open, C_DIM);
        hitbox(cx, cy, x1, cy + 42, H_WIN, S_DROP + id);
    }
    cy += open ? 46 : ROW;
    if (!open) return;
    for (int i = 0; i < n; i++) {
        if (fits(38)) {
            if (i == on) { dl_rect(cx, cy, x1, cy + 38, 8, C_SEL); dl_rect(cx + 4, cy + 10, cx + 8, cy + 28, 2, C_ACC); }
            dl_text(cx + 18, cy + 8, s[i], strlen(s[i]), SF(2), C_TXT, cx, Y0, x1 - 8, Y1);
            hitbox(cx, cy, x1, cy + 38, H_WIN, base + i);
        }
        cy += 40;
    }
    cy += ROW - 40;
}

/* A screen's modes, each size once (its fastest refresh). */
#define MAXMODE 10
static int modes_of(struct wlr_output *o, struct wlr_output_mode **out)
{
    int n = 0;
    struct wlr_output_mode *m;
    wl_list_for_each(m, &o->modes, link) {
        int k = 0;
        while (k < n && (out[k]->width != m->width || out[k]->height != m->height)) k++;
        if (k < n) { if (m->refresh > out[k]->refresh) out[k] = m; continue; }
        if (n < MAXMODE) out[n++] = m;
    }
    return n;
}

/* The running apps the Deskbar does not list (Add). */
#define MAXADD 8
static struct view *adds[MAXADD];
static int nadd;

/* ---- Typing: an app's name and command, a search among those installed. */
enum { F_NONE, F_NAME, F_CMD, F_SEARCH };
static struct { int app, field; char name[48], cmd[256], search[64]; } ed = { -1, F_NONE, "", "", "" };
static char *field_buf(int f, size_t *cap)
{
    if (f == F_NAME) { *cap = sizeof ed.name; return ed.name; }
    if (f == F_CMD) { *cap = sizeof ed.cmd; return ed.cmd; }
    *cap = sizeof ed.search;
    return ed.search;
}
static void field(const char *name, int f)
{
    row_label(name);
    if (fits(44)) {
        size_t cap;
        char *b = field_buf(f, &cap), a[300];
        ascii(b, a, sizeof a);
        int on = ed.field == f, n = (int)strlen(a), k = f == F_CMD ? S(2) : SF(2), from = 0;   /* (a command: mono, as code) */
        while (from < n && text_sw(a + from, n - from, k) > X1 - 24 - cx - 32) from++;   /* (the end, when long) */
        dl_rect(cx, cy, X1 - 24, cy + 44, 10, C_FIELD);
        dl_frame(cx, cy, X1 - 24, cy + 44, 10, on ? 2 : 1, on ? C_ACC : C_LINE);
        if (n) dl_text(cx + 14, cy + 11, a + from, n - from, k, C_TXT, cx, cy, X1 - 34, cy + 44);
        else if (f == F_SEARCH) dl_text(cx + 14, cy + 11, "Search the apps installed", 25, SF(2), C_DIM, cx, cy, X1 - 34, cy + 44);
        if (on) { int w = text_sw(a + from, n - from, k); dl_rect(cx + 16 + w, cy + 10, cx + 18 + w, cy + 34, 0, C_ACC); }
        hitbox(cx, cy, X1 - 24, cy + 44, H_WIN, S_FIELD + f);
    }
    cy += ROW;
}

/* The apps installed: the .desktop files of the applications folders
 * (the user's, the system's, Flatpak's), the first of each name winning. */
struct dapp { char name[64], cmd[256], id[96], file[96]; int icon; };
static struct dapp *dapps, found[8];
static int ndapps, nfound, dapps_read;
static void read_desktop(const char *path, const char *file)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024], name[64] = "", exec[512] = "", wmclass[96] = "", cats[256] = "";
    int in = 0, hide = 0, term = 0, app = 1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') { in = !strcmp(line, "[Desktop Entry]"); continue; }
        if (!in) continue;
        if (!strncmp(line, "Name=", 5)) snprintf(name, sizeof name, "%s", line + 5);
        else if (!strncmp(line, "Exec=", 5)) snprintf(exec, sizeof exec, "%s", line + 5);
        else if (!strncmp(line, "StartupWMClass=", 15)) snprintf(wmclass, sizeof wmclass, "%s", line + 15);
        else if (!strncmp(line, "Categories=", 11)) snprintf(cats, sizeof cats, "%s", line + 11);
        else if (!strcmp(line, "NoDisplay=true") || !strcmp(line, "Hidden=true")) hide = 1;
        else if (!strcmp(line, "Terminal=true")) term = 1;
        else if (!strncmp(line, "Type=", 5)) app = !strcmp(line + 5, "Application");
    }
    fclose(f);
    if (hide || !app || !name[0] || !exec[0]) return;
    for (int i = 0; i < ndapps; i++) if (!strcmp(dapps[i].file, file)) return;   /* (the first folder's wins) */
    char cmd[256];
    size_t o = 0;
    for (const char *p = exec; *p && o + 1 < sizeof cmd; p++) {   /* (Exec's %f, %U...: gone; %%: %) */
        if (*p == '%' && p[1]) { if (p[1] == '%') cmd[o++] = '%'; p++; continue; }
        cmd[o++] = *p;
    }
    while (o && cmd[o - 1] == ' ') o--;
    cmd[o] = 0;
    dapps = realloc(dapps, (ndapps + 1) * sizeof *dapps);
    struct dapp *d = &dapps[ndapps++];
    snprintf(d->name, sizeof d->name, "%s", name);
    if (term) snprintf(d->cmd, sizeof d->cmd, "%s -e %s", srv.terminal[0] ? srv.terminal : "foot", cmd);
    else snprintf(d->cmd, sizeof d->cmd, "%s", cmd);
    snprintf(d->file, sizeof d->file, "%s", file);
    if (wmclass[0]) snprintf(d->id, sizeof d->id, "%s", wmclass);   /* (the app id: its class, or the file's name) */
    else snprintf(d->id, sizeof d->id, "%.*s", (int)(strlen(file) > 8 ? strlen(file) - 8 : 0), file);
    d->icon = strstr(cats, "WebBrowser") ? I_WEB : strstr(cats, "TerminalEmulator") ? I_TERM : strstr(cats, "FileManager") ? I_FOLDER :
              strstr(cats, "TextEditor") ? I_FILE : strstr(cats, "Email") ? I_MAIL : strstr(cats, "Settings") ? I_SETTINGS :
              icon_for_app(d->id);
}
static void read_dapps(void)
{
    if (dapps_read) return;
    dapps_read = 1;
    char dirs[16][512];
    int nd = 0;
    const char *home = getenv("HOME"), *dh = getenv("XDG_DATA_HOME"), *dd = getenv("XDG_DATA_DIRS");
    if (dh && *dh) snprintf(dirs[nd++], 512, "%s/applications", dh);
    else snprintf(dirs[nd++], 512, "%s/.local/share/applications", home ? home : "");
    snprintf(dirs[nd++], 512, "%s/.local/share/flatpak/exports/share/applications", home ? home : "");
    char list[2048];
    snprintf(list, sizeof list, "%s", dd && *dd ? dd : "/usr/local/share:/usr/share");
    for (char *p = strtok(list, ":"); p && nd < 15; p = strtok(0, ":")) snprintf(dirs[nd++], 512, "%s/applications", p);
    snprintf(dirs[nd++], 512, "/var/lib/flatpak/exports/share/applications");
    for (int k = 0; k < nd; k++) {
        DIR *d = opendir(dirs[k]);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l < 9 || strcmp(e->d_name + l - 8, ".desktop")) continue;
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", dirs[k], e->d_name);
            read_desktop(path, e->d_name);
        }
        if (d) closedir(d);
    }
}
/* Those installed whose name (or command) holds what was typed, not in the list already. */
static void find_apps(void)
{
    nfound = 0;
    if (!ed.search[0]) return;
    read_dapps();
    for (int i = 0; i < ndapps && nfound < 8; i++) {
        struct dapp *d = &dapps[i];
        if (!strcasestr(d->name, ed.search) && !strcasestr(d->cmd, ed.search)) continue;
        int have = apps_has(d->id);
        for (int k = 0; k < apps_count() && !have; k++) have = !strcmp(apps_cmd(k), d->cmd);
        if (!have) found[nfound++] = *d;
    }
}

static void edit_done(int ok)
{
    if (ok && ed.app >= 0 && ed.name[0] && ed.cmd[0]) { apps_edit(ed.app, ed.name, ed.cmd); apps_save(); }
    ed.app = -1;
    if (ed.field == F_NAME || ed.field == F_CMD) ed.field = F_NONE;
}
static void add_found(int i)
{
    if (i < 0 || i >= nfound) return;
    apps_add(found[i].name, icon_name(found[i].icon), found[i].cmd, found[i].id, 0);   /* (in the menu; Dock puts it on the Deskbar) */
    apps_save();
    ed.search[0] = 0;
    ed.field = F_NONE;
}

/* A key, while Settings is in front: to the field being typed in. */
int settings_key(struct view *v, xkb_keysym_t sym, uint32_t ch)
{
    if (ed.field == F_NONE) return 0;
    size_t cap;
    char *b = field_buf(ed.field, &cap);
    size_t n = strlen(b);
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (ed.field == F_SEARCH) { find_apps(); add_found(0); }
        else if (ed.field == F_NAME) ed.field = F_CMD;
        else edit_done(1);
    } else if (sym == XKB_KEY_Escape) {
        if (ed.field == F_SEARCH) { b[0] = 0; ed.field = F_NONE; }
        else edit_done(0);
    } else if (sym == XKB_KEY_Tab && ed.field != F_SEARCH) ed.field = ed.field == F_NAME ? F_CMD : F_NAME;
    else if (sym == XKB_KEY_BackSpace) { while (n && (b[n - 1] & 0xc0) == 0x80) n--; if (n) b[n - 1] = 0; }
    else if (ch >= 32 && ch != 127) {                    /* (a character, UTF-8, at the end) */
        char u[4];
        int l = ch < 0x80 ? 1 : ch < 0x800 ? 2 : ch < 0x10000 ? 3 : 4;
        if (l == 1) u[0] = (char)ch;
        else if (l == 2) { u[0] = (char)(0xc0 | ch >> 6); u[1] = (char)(0x80 | (ch & 63)); }
        else if (l == 3) { u[0] = (char)(0xe0 | ch >> 12); u[1] = (char)(0x80 | (ch >> 6 & 63)); u[2] = (char)(0x80 | (ch & 63)); }
        else { u[0] = (char)(0xf0 | ch >> 18); u[1] = (char)(0x80 | (ch >> 12 & 63)); u[2] = (char)(0x80 | (ch >> 6 & 63)); u[3] = (char)(0x80 | (ch & 63)); }
        if (n + l < cap) { memcpy(b + n, u, l); b[n + l] = 0; }
    } else return 1;                                     /* (other keys: kept from the apps behind) */
    settings_draw(v);
    return 1;
}

static void sec_screens(void)
{
    heading("Screens");
    struct output *o;
    char t[96];
    int k = 0;
    wl_list_for_each(o, &srv.outputs, link) {
        struct wlr_output *wo = o->wlr;
        snprintf(t, sizeof t, "%s  %d x %d", wo->name, wo->width, wo->height);
        group(t);
        struct wlr_output_mode *ms[MAXMODE];
        int n = modes_of(wo, ms), on = -1;
        if (n > 1) {
            static char nb[MAXMODE][24];
            const char *names[MAXMODE];
            for (int i = 0; i < n; i++) {
                snprintf(nb[i], sizeof nb[i], "%d x %d", ms[i]->width, ms[i]->height);
                names[i] = nb[i];
                if (ms[i]->width == wo->width && ms[i]->height == wo->height) on = i;
            }
            dropdown("Mode", names, n, on, S_MODE + k * 100, k);
        }
        static const char *sn[NSCALE] = { "100%", "125%", "150%", "175%", "200%" };
        on = -1;
        for (int i = 0; i < NSCALE; i++) if (fabs(scales[i] - wo->scale) < 0.01) on = i;
        slider("Scale", sn, NSCALE, on, S_SCALE + k * 100);
        if (back.on && back.o == wo) {
            if (fits(30)) dl_text(lx, cy, "Keep it? Else back in 15 s.", 27, SF(2), C_TXT, X0, Y0, X1, Y1);
            cy += 38;
            if (fits(42)) pill(pill(cx, cy, "Keep", 1, S_KEEP) + 10, cy, "Go back", 0, S_BACK);
            cy += ROW;
        }
        k++;
    }
}
static void sec_look(void)
{
    heading("Look");
    static const char *zn[NZOOM] = { "50%", "75%", "100%", "125%", "150%", "200%", "300%", "400%" };
    int zk = -1;
    for (int i = 0; i < NZOOM; i++) if (zooms[i] == srv.zoom_pct) zk = i;
    slider("Size", zn, NZOOM, zk, S_ZOOM);
    static const char *pn[3] = { "Day", "Night", "Auto" };
    choices("Palette", pn, 3, look_mode, S_NIGHT);
}
static void sec_mouse(void)
{
    heading("Mouse");
    static const char *spn[4] = { "Slow", "Normal", "Fast", "Faster" };
    slider("Pointer speed", spn, 4, mouse.speed, S_SPEED);
    static const char *hands[2] = { "Right-handed", "Left-handed" }, *wheels[2] = { "Normal", "Natural" }, *taps[2] = { "Off", "On" };
    choices("Buttons", hands, 2, mouse.left, S_HAND);
    choices("Wheel", wheels, 2, mouse.natural, S_WHEEL);
    choices("Tap to click", taps, 2, mouse.tap, S_TAP);
}
static void sec_apps(void)
{
    char t[96];
    heading("Apps");
    group("All are in the menu; Dock: on the Deskbar too.");
    cy += 4;
    for (int i = 0; i < apps_count(); i++) {             /* each: its name, then Dock, up, down, Edit, Remove (under it when narrow) */
        int x = cx > lx ? cx : lx + 36;
        if (fits(42)) {
            icon(apps_icon(i), lx, cy + 9, 2);
            dl_text(lx + 36, cy + 10, apps_name(i), strlen(apps_name(i)), SF(2), C_TXT, X0, Y0, cx > lx ? x - 12 : X1 - 16, Y1);
        }
        if (cx == lx) cy += 42;
        if (fits(42)) {
            x = pill(x, cy, "Dock", apps_dock(i), S_DOCK + i) + 16;
            for (int k = 0; k < 2; k++, x += 50) {       /* (up, down: a chevron each) */
                dl_rect(x, cy, x + 42, cy + 42, 21, C_FIELD);
                dl_frame(x, cy, x + 42, cy + 42, 21, 1, C_LINE);
                chevron(x + 21, cy + 21, !k, C_TXT);
                hitbox(x, cy, x + 42, cy + 42, H_WIN, (k ? S_DOWN : S_UP) + i);
            }
            x = pill(x + 8, cy, "Edit", ed.app == i, S_EDIT + i) + 8;
            pill(x, cy, "Remove", 0, S_REM + i);
        }
        cy += ROW + (cx == lx ? 6 : 0);
        if (ed.app == i) {                               /* being edited: its name and command */
            field("Name", F_NAME);
            field("Command", F_CMD);
            if (fits(42)) pill(pill(cx, cy, "Done", 1, S_DONE) + 10, cy, "Cancel", 0, S_CANCEL);
            cy += ROW;
        }
    }
    cy += 10;
    field("Add an app", F_SEARCH);
    find_apps();
    for (int i = 0; i < nfound; i++) {
        struct dapp *d = &found[i];
        if (fits(42)) {
            icon(d->icon, cx, cy + 9, 2);
            dl_text(cx + 36, cy + 10, d->name, strlen(d->name), SF(2), C_TXT, X0, Y0, X1 - 130, Y1);
            pill(X1 - 24 - (tws("Add", 2) + 28), cy, "Add", 0, S_FOUND + i);
        }
        cy += ROW - 6;
    }
    if (ed.field == F_SEARCH && ed.search[0] && !nfound) { if (fits(30)) dl_text(cx, cy, "Nothing installed by that name", 30, SSS, C_DIM, X0, Y0, X1, Y1); cy += 30; }
    nadd = 0;
    struct view *w;
    wl_list_for_each(w, &srv.views, link) {
        if (!w->mapped || w->internal || !*view_app(w) || apps_has(view_app(w)) || nadd == MAXADD) continue;
        int dup = 0;
        for (int i = 0; i < nadd; i++) dup |= !strcmp(view_app(adds[i]), view_app(w));
        if (!dup) adds[nadd++] = w;
    }
    if (nadd) {
        cy += 8;
        row_label("Running");
        int x = cx;
        for (int i = 0; i < nadd; i++) {
            snprintf(t, sizeof t, "+ %s", view_app(adds[i]));
            int pw = tws(t, 2) + 28;
            if (x > cx && x + pw > X1 - 24) { x = cx; cy += ROW; }
            if (fits(42)) pill(x, cy, t, 0, S_ADD + i);
            x += pw + 8;
        }
        cy += ROW;
    }
}
enum { P_SCREENS, P_LOOK, P_MOUSE, P_APPS, NPAGE };
static const struct { const char *name; void (*draw)(void); } pages[NPAGE] = {
    { "Screens", sec_screens }, { "Look", sec_look }, { "Mouse", sec_mouse }, { "Apps", sec_apps } };

static int content_h;
void settings_draw(struct view *v)
{
    int cx0, cy0, cx1, cy1;
    wm_client_box(v, &cx0, &cy0, &cx1, &cy1);
    X0 = v->x0 + WM_IN; Y0 = v->y0 + wm_strip(v); X1 = v->x1 - WM_IN; Y1 = v->y1 - WM_IN;
    dl_reset();
    hits_begin(&v->bhits);
    dl_rect(X0 - 2, Y0 - 2, X1 + 2, Y1 + 2, 0, C_PANEL);
    hitbox(X0, Y0, X1, Y1, H_WIN, S_BODY);
    int top = Y0 + 4 - v->scroll;
    paged = v->x1 - v->x0 >= 720;
    if (!paged) {                                        /* narrow: every section, its labels above its rows */
        lx = cx = X0 + 24;
        cy = top;
        for (int i = 0; i < NPAGE; i++) pages[i].draw();
    } else {
        int sx1 = X0 + SBW, y = Y0 + 16;                 /* the sidebar: the sections, the chosen one lit */
        dl_rect(sx1, Y0, sx1 + 1, Y1, 0, C_LINE);
        for (int i = 0; i < NPAGE && y + 40 <= Y1 - 6; i++) {
            if (i == page) {
                dl_rect(X0 + 12, y, sx1 - 12, y + 40, 10, C_TERM);
                dl_rect(X0 + 12, y + 10, X0 + 16, y + 30, 2, C_ACC);
                bold(X0 + 30, y + 9, pages[i].name, C_TXT, sx1 - 12);
            } else label(X0 + 30, y + 9, pages[i].name, 2, C_TXT);
            hitbox(X0 + 12, y, sx1 - 12, y + 40, H_WIN, S_PAGE + i);
            y += 44;
        }
        int kx0 = sx1 + 24, kx1 = X1 - 24;               /* the card: the section's rows */
        lx = kx0 + 22; cx = kx1 - kx0 >= 660 ? lx + 190 : lx;
        int x1 = X1;
        X1 = kx1;
        cy = top + 16;
        if (fits(30)) title(kx0 + 4, pages[page].name);
        cy += 54;
        int c0 = cy;
        dry = 1; pages[page].draw(); dry = 0;
        int ky0 = c0 - 20 > Y0 ? c0 - 20 : Y0, ky1 = cy + 4 < Y1 ? cy + 4 : Y1;
        if (ky1 > ky0) { dl_rect(kx0, ky0, kx1, ky1, 14, C_TERM); dl_frame(kx0, ky0, kx1, ky1, 14, 1, C_LINE); }
        cy = c0;
        pages[page].draw();
        cy += 24;
        X1 = x1;
    }
    content_h = cy + v->scroll - top + 20;
    chrome_draw(&v->body, cx0, cy0, cx1, cy1);
}

void settings_scroll(struct view *v, double d)
{
    int room = (v->y1 - WM_IN) - (v->y0 + wm_strip(v)), max = content_h > room ? content_h - room : 0;
    v->scroll += (int)(d * 3);
    if (v->scroll > max) v->scroll = max;
    if (v->scroll < 0) v->scroll = 0;
    settings_draw(v);
}

/* The command a running app was started with: its process's command line
 * (the Wayland client's, or the X11 window's pid). */
static void command_of(struct view *v, char *out, size_t cap)
{
    pid_t pid = 0;
    if (v->xs) pid = v->xs->pid;
    else if (view_surface(v)) wl_client_get_credentials(wl_resource_get_client(view_surface(v)->resource), &pid, NULL, NULL);
    out[0] = 0;
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    FILE *f = pid > 0 ? fopen(path, "r") : 0;
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) out[o++] = buf[i] ? buf[i] : ' ';
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

void settings_press(struct view *v, int x, int y, int button)
{
    (void)button;
    const hit_t *h = hits_at(&v->bhits, x, y);
    if (!h) return;
    int a = h->arg;
    if (a >= S_FIELD && a < S_FIELD + 4) { ed.field = a - S_FIELD; settings_draw(v); return; }
    if (ed.field == F_SEARCH) ed.field = F_NONE;         /* (a press elsewhere: the search stops taking keys) */
    if (a != S_DROP + drop) drop = -1;                  /* (a press anywhere closes a dropdown) */
    if (a >= S_ZOOM && a < S_ZOOM + NZOOM) { srv.zoom_pct = zooms[a - S_ZOOM]; bar_layout(); look_save(); }
    else if (a >= S_NIGHT && a < S_NIGHT + 3) { look_mode = a - S_NIGHT; say("settings: look %s", look_mode == 0 ? "day" : look_mode == 1 ? "night" : "auto"); look_apply(); look_save(); }
    else if (a >= S_PAGE && a < S_PAGE + NPAGE) { page = a - S_PAGE; v->scroll = 0; }
    else if (a >= S_DROP && a < S_DROP + MAXSCR) drop = drop == a - S_DROP ? -1 : a - S_DROP;
    else if (a >= S_SPEED && a < S_SPEED + 4) { mouse.speed = a - S_SPEED; mouse_apply(0); mouse_save(); }
    else if (a >= S_HAND && a < S_HAND + 2) { mouse.left = a - S_HAND; mouse_save(); }
    else if (a >= S_WHEEL && a < S_WHEEL + 2) { mouse.natural = a - S_WHEEL; mouse_save(); }
    else if (a >= S_TAP && a < S_TAP + 2) { mouse.tap = a - S_TAP; mouse_apply(0); mouse_save(); }
    else if (a == S_KEEP) { back.on = 0; if (back.t) wl_event_source_timer_update(back.t, 0); screens_save(); }
    else if (a == S_BACK) { if (back.t) wl_event_source_timer_update(back.t, 0); go_back(); }
    else if (a >= S_MODE && a < S_SCALE + 1000) {
        int k = (a % 1000) / 100, i = a % 100, n = 0;
        struct output *o, *pick = 0;
        wl_list_for_each(o, &srv.outputs, link) if (n++ == k) pick = o;
        if (!pick) return;
        struct wlr_output *wo = pick->wlr;
        if (a < S_SCALE) {
            struct wlr_output_mode *ms[MAXMODE];
            if (i < modes_of(wo, ms)) try_screen(wo, ms[i]->width, ms[i]->height, ms[i]->refresh, wo->scale);
        } else if (i < NSCALE) try_screen(wo, wo->width, wo->height, wo->refresh, (float)scales[i]);
    }
    else if (a >= S_UP && a < S_UP + 1000) { if (ed.app >= 0) edit_done(0); apps_move(a - S_UP, -1); apps_save(); }
    else if (a >= S_DOWN && a < S_DOWN + 1000) { if (ed.app >= 0) edit_done(0); apps_move(a - S_DOWN, 1); apps_save(); }
    else if (a >= S_REM && a < S_REM + 1000) { if (ed.app >= 0) edit_done(0); apps_remove(a - S_REM); apps_save(); }
    else if (a >= S_DOCK && a < S_DOCK + 1000) { apps_set_dock(a - S_DOCK, !apps_dock(a - S_DOCK)); apps_save(); }
    else if (a >= S_EDIT && a < S_EDIT + 1000) {
        int i = a - S_EDIT;
        if (ed.app == i) edit_done(0);
        else { ed.app = i; snprintf(ed.name, sizeof ed.name, "%s", apps_name(i)); snprintf(ed.cmd, sizeof ed.cmd, "%s", apps_cmd(i)); ed.field = F_NAME; }
    }
    else if (a == S_DONE) edit_done(1);
    else if (a == S_CANCEL) edit_done(0);
    else if (a >= S_FOUND && a < S_FOUND + nfound) add_found(a - S_FOUND);
    else if (a >= S_ADD && a < S_ADD + nadd) {
        struct view *w = adds[a - S_ADD];
        char cmd[512], name[64];
        command_of(w, cmd, sizeof cmd);
        const char *id = view_app(w), *b = strrchr(id, '.');
        snprintf(name, sizeof name, "%s", b && b[1] ? b + 1 : id);
        if (name[0] >= 'a' && name[0] <= 'z') name[0] -= 32;
        if (cmd[0]) { apps_add(name, "", cmd, id, 0); apps_save(); }
    }
    if (win) settings_draw(win);
}

/* ---- The window. */
void settings_open(void)
{
    if (win) { wm_show(win); view_focus(win); return; }
    struct view *v = calloc(1, sizeof *v);
    v->internal = 1;
    view_tree_init(v);
    chrome_init(&v->body, v->tree, CH_VIEW, v, &v->bhits);
    wlr_scene_node_place_above(&v->body.sb->node, &v->ground->node);
    win = v;
    wm_place(v);
    view_mapped(v);
}
void settings_gone(struct view *v)                      /* (closed) */
{
    if (v != win) return;
    win = 0;
    ed.app = -1; ed.field = F_NONE; ed.search[0] = 0; drop = -1;
    view_unmapped(v);
    view_destroyed(v);
}
