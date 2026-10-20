/*
 * deskbar.c - The Deskbar, the desk and the menus (from SIEOS's atlas.c).
 *
 *   ┌─ (cube) Atlas    v ─┐ the menu: the apps, every window, Lock, Log out
 *   │ Sat 17 Oct   11:38  │ the tray: the date (or "2 new"), the battery, the clock
 *   │ Terminal         ●  │ the apps (running: bold, a lit dot, how many);
 *   │ Files               │ a click brings the app's window back, or starts it;
 *   │ Web                 │ right-click: New ..., and its windows
 *   │ [1] [2] [3] [4]     │ the workspaces, each a small map of its windows
 *   └─────────────────────┘
 *
 * The apps are ~/.config/atlas/apps (name | icon | command | app_id |
 * dock lines; Settings writes it), or those found on this computer. All
 * are in the cube's menu; those docked also have a row on the Deskbar.
 * Apps running that are not docked get a row of their own while they run.
 * Windows that ask for attention (an activation they were not given, X11's
 * urgency) are counted in an amber pill in the tray, in the date's place; a
 * click on it brings the latest. The Deskbar is clear glass, white text on
 * it; menus are milky glass. The
 * desk is a deep navy lit by blue glows; a workspace with no window shows
 * the time, large and dim, at its bottom right.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "atlas.h"

static int M = 16, DBW, DBX, DBY;        /* margin; the Deskbar's width, left and top (the first screen's top right, under panels) */
#define DB_MENU 52                       /* the Deskbar: its menu row, tray, app rows, workspaces */
#define DB_TRAY 40
#define DB_ROW 40
#define DB_WS 48
#define MAXAPP 24
#define MAXROW 32

static struct app { char name[48], cmd[256], id[96]; int icon, dock; } apps[MAXAPP];   /* dock: a row on the Deskbar (else the menu only) */
static int napps;
static struct row { int app; char id[96], name[64]; int icon, n, hidden; struct view *last; } rows[MAXROW];
static int nrows, hover_row = -1, hover_menu_row;
static struct chrome bar;
static hits_t bar_hits;
static int bar_y1;

/* ---- The menus: one at a time; entries with an icon, separators. */
enum { A_SEP = 1, A_RUN, A_WINDOW, A_LOGOUT, A_MIN, A_ZOOM, A_FULL, A_SENDWS, A_CLOSE, A_SETTINGS, A_LOCK };
#define MAXMENT 40
#define MENH 36
#define MENSEP 12
static struct {
    int on, x, y, w, n, hover, from_bar;     /* from_bar: the cube's (its row stays lit) */
    struct { char label[64], cmd[256]; int act, icon, arg; struct view *v; } e[MAXMENT];
} mn;
static struct chrome menu;
static hits_t menu_hits;

/* ---- The apps. */
static int on_path(const char *cmd)                      /* the command's first word is a program here */
{
    char w[128];
    size_t n = 0;
    while (cmd[n] && cmd[n] != ' ' && n < sizeof w - 1) { w[n] = cmd[n]; n++; }
    w[n] = 0;
    if (strchr(w, '/')) return access(w, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    char d[512];
    for (const char *p = path; *p;) {
        const char *e = strchr(p, ':');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        snprintf(d, sizeof d, "%.*s/%s", (int)k, p, w);
        if (access(d, X_OK) == 0) return 1;
        p += k + (e ? 1 : 0);
    }
    return 0;
}
static void add_app(const char *name, const char *icon_name, const char *cmd, const char *id, int dock)
{
    if (napps == MAXAPP) return;
    struct app *a = &apps[napps++];
    a->dock = dock;
    snprintf(a->name, sizeof a->name, "%s", name);
    snprintf(a->cmd, sizeof a->cmd, "%s", cmd);
    snprintf(a->id, sizeof a->id, "%s", id ? id : "");
    a->icon = icon_by_name(icon_name);
    if (a->icon < 0) a->icon = icon_for_app(a->id[0] ? a->id : cmd);
}
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}
static int load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line), *fld[5] = { 0 };
        if (!*p || *p == '#') continue;
        int k = 0;
        for (char *s = p; k < 5; k++) { fld[k] = s; char *bar = strchr(s, '|'); if (!bar) { k++; break; } *bar = 0; s = bar + 1; }
        for (int i = 0; i < 5; i++) if (fld[i]) fld[i] = trim(fld[i]);
        if (fld[0] && fld[2] && *fld[0] && *fld[2]) add_app(fld[0], fld[1] ? fld[1] : "", fld[2], fld[3], !fld[4] || strcmp(fld[4], "menu"));
    }
    fclose(f);
    return 1;
}
/* Those found here: the first of each kind. */
#define TERM_FOOT "foot -T Term --app-id=Term -o colors.background=1b254a -o colors.alpha=0 -o colors.foreground=c9d1ec -o 'font=DejaVu Sans Mono:size=12'"
static const struct { const char *name, *icon, *cmd, *id; } finds[][6] = {
    /* (terminals named Term, see-through over Atlas's smoked glass (wm.c), its light text, in
     * Atlas's font at 12 points: through their own options, their settings files untouched) */
    { { "Terminal", "terminal", TERM_FOOT, "Term" },
      { "Terminal", "terminal", "alacritty -T Term --class Term -o 'colors.primary.background=\"#1b254a\"' -o 'colors.primary.foreground=\"#c9d1ec\"' -o window.opacity=0 -o 'font.normal.family=\"DejaVu Sans Mono\"' -o font.size=12", "Term" },
      { "Terminal", "terminal", "kitty -T Term --class Term -o background=#1b254a -o background_opacity=0 -o foreground=#c9d1ec -o 'font_family=DejaVu Sans Mono' -o font_size=12", "Term" }, { "Terminal", "terminal", "kgx", "org.gnome.Console" },
      { "Terminal", "terminal", "konsole", "org.kde.konsole" }, { "Terminal", "terminal", "gnome-terminal --wait", "org.gnome.Terminal" } },
    { { "Files", "folder", "atlas-files", "atlas.files" }, { "Files", "folder", "nautilus --new-window", "org.gnome.Nautilus" },
      { "Files", "folder", "thunar", "thunar" }, { "Files", "folder", "pcmanfm-qt", "pcmanfm-qt" }, { "Files", "folder", "dolphin", "org.kde.dolphin" }, { 0 } },
    { { "Text editor", "editor", "gnome-text-editor --new-window", "org.gnome.TextEditor" }, { "Text editor", "editor", "gedit", "org.gnome.gedit" },
      { "Text editor", "editor", "mousepad", "org.xfce.mousepad" }, { "Text editor", "editor", "kate", "org.kde.kate" }, { 0 }, { 0 } },
    { { "Web", "web", "firefox", "firefox" }, { "Web", "web", "firefox-esr", "firefox-esr" }, { "Web", "web", "chromium", "chromium" },
      { "Web", "web", "epiphany", "org.gnome.Epiphany" }, { 0 }, { 0 } },
};
const char *config_path(const char *name, char *buf, size_t cap)   /* ~/.config/atlas/NAME */
{
    const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (cfg && *cfg) snprintf(buf, cap, "%s/atlas/%s", cfg, name);
    else snprintf(buf, cap, "%s/.config/atlas/%s", home ? home : "", name);
    return buf;
}

static void find_terminal(void)
{
    srv.terminal[0] = 0;
    for (int i = 0; i < napps; i++) if (apps[i].icon == I_TERM) { snprintf(srv.terminal, sizeof srv.terminal, "%s", apps[i].cmd); break; }
    const char *t = getenv("TERMINAL");
    if (!srv.terminal[0] && t && *t) snprintf(srv.terminal, sizeof srv.terminal, "%s", t);
}
void bar_reload(void)
{
    napps = 0;
    char path[512];
    config_path("apps", path, sizeof path);
    if (!load_file(path))
        for (size_t k = 0; k < sizeof finds / sizeof *finds; k++)
            for (int i = 0; i < 6 && finds[k][i].name; i++) {
                char beside[PATH_MAX];                   /* (atlas-files not installed: the one built next to atlas_wm) */
                ssize_t n = readlink("/proc/self/exe", beside, sizeof beside - 32);
                if (n > 0) { beside[n] = 0; snprintf(strrchr(beside, '/') + 1, 32, "%s", finds[k][i].cmd); }
                if (on_path(finds[k][i].cmd)) { add_app(finds[k][i].name, finds[k][i].icon, finds[k][i].cmd, finds[k][i].id, 1); break; }
                if (n > 0 && !strchr(finds[k][i].cmd, ' ') && access(beside, X_OK) == 0) { add_app(finds[k][i].name, finds[k][i].icon, beside, finds[k][i].id, 1); break; }
            }
    find_terminal();
}

static int app_runs_as(const struct app *a, const char *id);

/* ---- The list, changed from Settings (then kept: apps_save). */
int apps_count(void) { return napps; }
const char *apps_name(int i) { return i >= 0 && i < napps ? apps[i].name : ""; }
int apps_icon(int i) { return i >= 0 && i < napps ? apps[i].icon : I_APP; }
void apps_move(int i, int d)
{
    int j = i + d;
    if (i < 0 || i >= napps || j < 0 || j >= napps) return;
    struct app t = apps[i]; apps[i] = apps[j]; apps[j] = t;
}
void apps_remove(int i)
{
    if (i < 0 || i >= napps) return;
    memmove(apps + i, apps + i + 1, (napps - i - 1) * sizeof *apps);
    napps--;
}
void apps_add(const char *name, const char *icon_name, const char *cmd, const char *id, int dock) { add_app(name, icon_name, cmd, id, dock); }
int  apps_dock(int i) { return i >= 0 && i < napps && apps[i].dock; }
void apps_set_dock(int i, int on) { if (i >= 0 && i < napps) apps[i].dock = on; }
const char *apps_cmd(int i) { return i >= 0 && i < napps ? apps[i].cmd : ""; }
void apps_edit(int i, const char *name, const char *cmd)
{
    if (i < 0 || i >= napps) return;
    snprintf(apps[i].name, sizeof apps[i].name, "%s", name);
    snprintf(apps[i].cmd, sizeof apps[i].cmd, "%s", cmd);
}
int apps_has(const char *id)
{
    for (int i = 0; i < napps; i++) if (app_runs_as(&apps[i], id)) return 1;
    return 0;
}
void apps_save(void)                                     /* ~/.config/atlas/apps, the Deskbar again */
{
    char path[512], dir[512];
    config_path("apps", path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0;
    char *p = dir;
    while ((p = strchr(p + 1, '/'))) { *p = 0; mkdir(dir, 0755); *p = '/'; }
    mkdir(dir, 0755);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs("# The apps: name | icon | command | app_id | dock (a row on the Deskbar) or menu (the menu only); written by Settings\n", f);
        for (int i = 0; i < napps; i++)
            fprintf(f, "%s | %s | %s | %s | %s\n", apps[i].name, icon_name(apps[i].icon), apps[i].cmd, apps[i].id, apps[i].dock ? "dock" : "menu");
        fclose(f);
    }
    find_terminal();
    bar_draw();
}

void spawn(const char *cmd)
{
    if (!cmd || !*cmd) return;
    say("run: %s", cmd);
    pid_t p = fork();                                    /* (twice: the app is init's child, never a zombie of ours) */
    if (p == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        setsid();
        if (fork() == 0) {
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0) waitpid(p, NULL, 0);
}
void spawn_terminal(void) { spawn(srv.terminal[0] ? srv.terminal : TERM_FOOT); }
const char *term_preset(const char *name)
{
    size_t n = strlen(name);
    for (int i = 0; i < 6 && finds[0][i].name; i++)
        if (!strncmp(finds[0][i].cmd, name, n) && finds[0][i].cmd[n] == ' ') return finds[0][i].cmd;
    return 0;
}

/* ---- Rows: the apps, then each other app running. */
static int app_runs_as(const struct app *a, const char *id)
{
    if (!*id) return 0;
    if (a->id[0]) return !strcasecmp(a->id, id);
    char w[96];
    size_t n = 0;
    while (a->cmd[n] && a->cmd[n] != ' ' && n < sizeof w - 1) { w[n] = a->cmd[n]; n++; }
    w[n] = 0;
    const char *b = strrchr(w, '/');
    return !strcasecmp(b ? b + 1 : w, id);
}
static void app_name(const char *id, char *out, size_t cap)   /* "org.gnome.Nautilus" -> "Nautilus" */
{
    const char *b = strrchr(id, '.');
    b = b && b[1] ? b + 1 : id;
    snprintf(out, cap, "%.*s", (int)(cap - 1), *b ? b : "Window");
    if (out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
}
static void make_rows(void)
{
    nrows = 0;
    for (int i = 0; i < napps && nrows < MAXROW; i++) {
        if (!apps[i].dock) continue;
        struct row *r = &rows[nrows++];
        *r = (struct row){ .app = i, .icon = apps[i].icon };
        snprintf(r->name, sizeof r->name, "%.47s", apps[i].name);
        snprintf(r->id, sizeof r->id, "%.95s", apps[i].id);
    }
    struct view *v;
    wl_list_for_each(v, &srv.views, link) {
        if (!v->mapped) continue;
        const char *id = view_app(v);
        int k = -1;
        for (int i = 0; i < nrows && k < 0; i++)
            if (rows[i].app >= 0 ? app_runs_as(&apps[rows[i].app], id) : !strcmp(rows[i].id, id)) k = i;
        if (k < 0 && nrows < MAXROW) {                   /* (running: a row while it runs; an app of the menu's as itself) */
            int a = -1;
            for (int i = 0; i < napps && a < 0; i++) if (!apps[i].dock && app_runs_as(&apps[i], id)) a = i;
            k = nrows++;
            rows[k] = (struct row){ .app = a, .icon = a >= 0 ? apps[a].icon : view_icon(v) };
            snprintf(rows[k].id, sizeof rows[k].id, "%s", a >= 0 ? apps[a].id : id);
            if (a >= 0) snprintf(rows[k].name, sizeof rows[k].name, "%.47s", apps[a].name);
            else app_name(id, rows[k].name, sizeof rows[k].name);
        }
        if (k < 0) continue;
        rows[k].n++;
        rows[k].hidden += v->min;
        if (!rows[k].last || v->focus_ns >= rows[k].last->focus_ns) rows[k].last = v;
    }
}
static int row_has(int k, struct view *v)
{
    const char *id = view_app(v);
    return rows[k].app >= 0 ? app_runs_as(&apps[rows[k].app], id) : !strcmp(rows[k].id, id);
}

/* Where v's app's row is (a window's ghost flies to and from it: wm.c);
 * the menu's row if it has none. */
box_t bar_row_box(struct view *v)
{
    make_rows();
    int x0 = DBX + 6, x1 = DBX + DBW - 6, y = DBY + DB_MENU + DB_TRAY + 4;
    for (int k = 0; k < nrows; k++) if (row_has(k, v)) return (box_t){ x0, y + k * DB_ROW, x1, y + k * DB_ROW + DB_ROW - 2 };
    return (box_t){ x0, DBY + 6, x1, DBY + DB_MENU - 4 };
}

/* ---- Attention: the windows asking for it (shown: no longer). */
void bar_attention(struct view *v, int on)
{
    if (!v || !!on == !!v->attn_ns) return;
    v->attn_ns = on ? now_ns() : 0;
    if (on) say("window asks for attention: %s", view_title(v));
    bar_draw();
}
static struct view *asking(int *n)                       /* the latest asking, and how many */
{
    struct view *v, *last = 0;
    *n = 0;
    wl_list_for_each(v, &srv.views, link)
        if (v->mapped && v->attn_ns) { ++*n; if (!last || v->attn_ns > last->attn_ns) last = v; }
    return last;
}

/* ---- The tray: the date, the battery, the clock. */
static void clock_text(char *ck, size_t cap, const char *fmt)
{
    time_t t = time(0);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ck, cap, fmt, &tm);
}
static int battery(int *charging)                       /* -> percent, or -1: none */
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return -1;
    struct dirent *e;
    int pc = -1;
    while ((e = readdir(d)) && pc < 0) {
        if (strncmp(e->d_name, "BAT", 3)) continue;
        char p[300], b[32];
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(b, sizeof b, f)) pc = atoi(b);
        fclose(f);
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
        *charging = 0;
        if ((f = fopen(p, "r"))) { if (fgets(b, sizeof b, f)) *charging = !strncmp(b, "Charging", 8); fclose(f); }
    }
    closedir(d);
    return pc;
}

/* ---- Drawing the Deskbar. */
static int db_h(void) { return DB_MENU + DB_TRAY + nrows * DB_ROW + DB_WS + 10; }
void bar_draw(void)
{
    if (!output_primary()) return;
    make_rows();
    int x0 = DBX, y0 = DBY, x1 = DBX + DBW, y1 = DBY + db_h();
    bar_y1 = y1;
    dl_reset();
    hits_begin(&bar_hits);
    dl_rect(x0, y0, x1, y1, 16, C_CLEAR);
    hitbox(x0, y0, x1, y1, H_DBBG, 0);
    int lit = (mn.on && mn.from_bar) || hover_row == -2; /* the menu row: the cube, Atlas */
    if (lit) dl_rect(x0 + 6, y0 + 6, x1 - 6, y0 + DB_MENU - 4, 10, C_WASH);
    logo(x0 + 16, y0 + 10, 2);
    bold(x0 + 60, y0 + 15, "Atlas", C_GTXT, x1);
    label(x1 - 34, y0 + 15, "v", 2, C_GDIM);
    hitbox(x0 + 6, y0 + 6, x1 - 6, y0 + DB_MENU - 4, H_DBMENU, 0);
    int ty = y0 + DB_MENU;                               /* the tray */
    dl_rect(x0 + 8, ty + 3, x1 - 8, ty + DB_TRAY - 3, 10, C_CELL);
    char ck[16], day[32];
    clock_text(ck, sizeof ck, "%H:%M");
    clock_text(day, sizeof day, "%a %d %b");
    int chg = 0, pc = battery(&chg), cx = x1 - 16 - text_sw(ck, strlen(ck), STS);
    int na;
    if (asking(&na)) {                                   /* windows asking: an amber pill, in the date's place */
        char t[16];
        snprintf(t, sizeof t, "%d new", na);
        int k = strlen(t), pw = text_sw(t, k, SSS) + 24;
        dl_rect(x0 + 14, ty + 8, x0 + 14 + pw, ty + DB_TRAY - 8, 12, C_ACC);
        dl_text(x0 + 26, ty + 11, t, k, SSS, C_INK, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);
        dl_text(x0 + 27, ty + 11, t, k, SSS, C_INK, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);   /* (bold) */
        hitbox(x0 + 10, ty + 4, x0 + 18 + pw, ty + DB_TRAY - 4, H_DBBG, 1);   /* (the pill: arg 1) */
    } else dl_text(x0 + 20, ty + 9, day, strlen(day), SF(2), C_GTXT, -(1 << 20), -(1 << 20), pc >= 0 ? cx - 44 : cx - 12, 1 << 20);
    dl_text(cx, ty + 6, ck, strlen(ck), STS, C_GTXT, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);   /* (title size) */
    if (pc >= 0) {                                       /* the battery: its outline, filled as charged */
        int bx = cx - 38, by = ty + 14;
        uint32_t c = chg ? C_ACC : pc < 15 ? C_ERR : C_GTXT;
        dl_frame(bx, by, bx + 22, by + 12, 2, 1, c);
        dl_rect(bx + 22, by + 4, bx + 25, by + 8, 0, c);
        dl_rect(bx + 2, by + 2, bx + 2 + 18 * (pc > 100 ? 100 : pc) / 100, by + 10, 0, c);
    }
    int ry = ty + DB_TRAY;                               /* the apps */
    for (int k = 0; k < nrows; k++) {
        int y = ry + 4 + k * DB_ROW, run = rows[k].n > 0;
        if (hover_row == k) dl_rect(x0 + 6, y, x1 - 6, y + DB_ROW - 2, 10, C_WASH);
        icon_ink(rows[k].icon, x0 + 14, y + 7, 2, 1);
        if (run) {                                       /* running: bold, a lit dot (and how many) */
            bold(x0 + 50, y + 8, rows[k].name, C_GTXT, x1 - 50);
            if (rows[k].hidden == rows[k].n) dl_frame(x1 - 30, y + 14, x1 - 18, y + 26, 6, 2, C_ACC);   /* (all hidden: a ring) */
            else dl_rect(x1 - 30, y + 14, x1 - 18, y + 26, 6, C_ACC);
            if (rows[k].n > 1) {
                char c[12];
                snprintf(c, sizeof c, "%d", rows[k].n);
                dl_text(x1 - 38 - text_sw(c, strlen(c), SSS), y + 11, c, strlen(c), SSS, C_GDIM, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);
            }
        } else dl_text(x0 + 50, y + 8, rows[k].name, strlen(rows[k].name), SF(2), C_GDIM, -(1 << 20), -(1 << 20), x1 - 50, 1 << 20);
        hitbox(x0 + 6, y, x1 - 6, y + DB_ROW - 2, H_DBAPP, k);
    }
    int wy = ry + 8 + nrows * DB_ROW, cw = (DBW - 20 - 3 * 8) / WM_NWS;   /* the workspaces */
    for (int k = 0; k < WM_NWS; k++) {
        int cx0 = x0 + 10 + k * (cw + 8), on = k == wm_workspace();
        char d[2] = { (char)('1' + k), 0 };
        dl_rect(cx0, wy + 6, cx0 + cw, wy + 40, 8, on ? C_ACC : C_CELL);   /* a cell: its windows, small (the current one amber) */
        wm_minimap(k, cx0 + 22, wy + 11, cx0 + cw - 5, wy + 35, on ? C_DIM : C_LINE, C_TXT);
        dl_text(cx0 + 6, wy + 9, d, 1, SSS, on ? C_INK : C_GTXT, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);
        hitbox(cx0, wy + 6, cx0 + cw, wy + 40, H_DBWS, k);
    }
    chrome_draw(&bar, dsc(x0), dsc(y0), dsc(x1), dsc(y1));
    toplevels_update();                                  /* (taskbars see the same) */
}

static box_t pic(struct wlr_box b);

/* ---- The desk. */
#define RC_K 10                                          /* (the clock's text scale) */
static const char rest_hint[] = "Super+T a terminal   Super+Space search   the Deskbar: everything";
void desk_draw(void)
{
    struct output *o, *pri = output_primary();
    wl_list_for_each(o, &srv.outputs, link) {
        struct wlr_box b;
        wlr_output_layout_get_box(srv.layout, o->wlr, &b);
        if (wlr_box_empty(&b)) continue;
        dl_reset();
        box_t S = desk_screen(o);
        dl_desk(S.x0, S.y0, S.x1, S.y1);
        if (o == pri && !wm_top()) {                     /* an empty workspace: the time, and what to press */
            char ck[16];
            clock_text(ck, sizeof ck, "%H:%M");
            box_t U = o->usable.width > 0 ? pic(o->usable) : desk_screen(o);
            int x1 = U.x1 - 72, y1 = U.y1 - 56, y0 = y1 - CH * RC_K - 44;
            label(x1 - tw(strlen(ck), RC_K), y0, ck, RC_K, C_LINE);
            dl_text(x1 - text_sw(rest_hint, sizeof rest_hint - 1, SSS), y1 - 22, rest_hint, sizeof rest_hint - 1, SSS, C_DIM, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);
        }
        int px0 = P(b.x), py0 = P(b.y);
        chrome_draw(&o->desk, px0, py0, px0 + P(b.width), py0 + P(b.height));
    }
}

/* ---- The layout. The picture's scale comes from the first screen; the
 * Deskbar is at its top right, under any panel there. */
static box_t pic(struct wlr_box b) { return (box_t){ dunit(P(b.x)), dunit(P(b.y)), dunit(P(b.x + b.width)), dunit(P(b.y + b.height)) }; }
box_t desk_screen(struct output *o)
{
    struct wlr_box b;
    wlr_output_layout_get_box(srv.layout, o->wlr, &b);
    return pic(b);
}
box_t desk_area(struct output *o)                        /* (under panels; the Deskbar's column; room for the tabs) */
{
    box_t u = o->usable.width > 0 ? pic(o->usable) : desk_screen(o);
    box_t a = { u.x0 + M, u.y0 + M + TAB_H + 8, u.x1 - M, u.y1 - M };
    if (o == output_primary()) a.x1 = DBX - M;
    return a;
}
void bar_layout(void)
{
    struct output *o = output_primary();
    if (!o) return;
    int w, h;
    wlr_output_transformed_resolution(o->wlr, &w, &h);
    int oldz = draw_zoom_get();
    srv.scale = o->wlr->scale > 0 ? o->wlr->scale : 1;
    draw_size(w, h);
    draw_zoom((int)(49152 * srv.scale * srv.zoom_pct / 100 + 0.5));
    if (draw_zoom_get() != oldz) wm_rezoom(oldz);       /* (windows keep their size on the screen)*/
    box_t u = o->usable.width > 0 ? pic(o->usable) : desk_screen(o);
    DBW = u.x1 - u.x0 >= 1600 ? 300 : 260;
    DBX = u.x1 - M - DBW;
    DBY = u.y0 + M;
    wm_relayout();
    desk_draw();
    bar_draw();
    lock_redraw();
    if (mn.on) menu_close();
}

void bar_init(void)
{
    chrome_init(&bar, srv.l_bar, CH_BAR, 0, &bar_hits);
    chrome_init(&menu, srv.l_menu, CH_MENU, 0, &menu_hits);
    bar_reload();
}

/* ---- Menus. */
static int menu_h(void) { int h = 16; for (int i = 0; i < mn.n; i++) h += mn.e[i].act == A_SEP ? MENSEP : MENH; return h; }
static void menu_draw(void)
{
    if (!mn.on) { chrome_hide(&menu); return; }
    int h = menu_h(), y = mn.y + 8;
    dl_reset();
    hits_begin(&menu_hits);
    dl_shade(mn.x + 6, mn.y + 6, mn.x + mn.w + 6, mn.y + h + 6, 12, 1);
    dl_rect(mn.x, mn.y, mn.x + mn.w, mn.y + h, 12, C_GLASS);
    hitbox(mn.x, mn.y, mn.x + mn.w, mn.y + h, H_MENU, -1);
    for (int i = 0; i < mn.n; i++) {
        if (mn.e[i].act == A_SEP) { dl_rect(mn.x + 14, y + MENSEP / 2, mn.x + mn.w - 14, y + MENSEP / 2 + 1, 0, C_LINE); y += MENSEP; continue; }
        if (i == mn.hover) dl_rect(mn.x + 6, y, mn.x + mn.w - 6, y + MENH, 8, C_BAR);
        if (mn.e[i].icon >= 0) icon(mn.e[i].icon, mn.x + 16, y + 6, 2);
        uint32_t c = mn.e[i].act == A_CLOSE || mn.e[i].act == A_LOGOUT ? C_ERR : C_TXT;
        dl_text(mn.x + 54, y + 7, mn.e[i].label, strlen(mn.e[i].label), SF(2), c, mn.x, y, mn.x + mn.w - 14, y + MENH);
        hitbox(mn.x, y, mn.x + mn.w, y + MENH, H_MENU, i);
        y += MENH;
    }
    chrome_draw(&menu, dsc(mn.x), dsc(mn.y), dsc(mn.x + mn.w + 7), dsc(mn.y + h + 7));
}
static void ment(int act, int icon_, const char *label_, const char *cmd, struct view *v, int arg)
{
    if (mn.n == MAXMENT) return;
    typeof(mn.e[0]) *e = &mn.e[mn.n++];
    e->act = act; e->icon = icon_; e->v = v; e->arg = arg;
    snprintf(e->label, sizeof e->label, "%s", label_ ? label_ : "");
    snprintf(e->cmd, sizeof e->cmd, "%s", cmd ? cmd : "");
}
static void sep(void) { if (mn.n && mn.e[mn.n - 1].act != A_SEP) ment(A_SEP, -1, 0, 0, 0, 0); }
static void window_entries(int (*pick)(int, struct view *), int k)
{
    struct view *v;
    wl_list_for_each_reverse(v, &srv.views, link) {
        if (!v->mapped || (pick && !pick(k, v))) continue;
        char t[64], ascii[64];
        size_t n = 0;
        for (const char *s = view_title(v); *s && n < sizeof ascii - 1; s++) ascii[n++] = (unsigned char)*s < 0x80 ? *s : '?';
        ascii[n] = 0;
        snprintf(t, sizeof t, "%s%s", n ? ascii : "(untitled)", v->min ? " (hidden)" : "");
        ment(A_WINDOW, view_icon(v), t, 0, v, 0);
    }
}
static void menu_show(int x, int y)
{
    struct output *o = output_of_box((box_t){ x, y, x + 1, y + 1 });   /* (kept on its screen) */
    box_t S = o ? desk_screen(o) : (box_t){ 0, 0, SW, SH };
    int w = 260;
    for (int i = 0; i < mn.n; i++) { int k = 54 + tws(mn.e[i].label, 2) + 30; if (k > w) w = k; }
    if (w > (S.x1 - S.x0) / 2) w = (S.x1 - S.x0) / 2;
    mn.w = w;
    int h = menu_h();
    if (x + w > S.x1 - 8) x = S.x1 - 8 - w;
    if (y + h > S.y1 - 8) y = S.y1 - 8 - h;
    if (x < S.x0 + 8) x = S.x0 + 8;
    if (y < S.y0 + 8) y = S.y0 + 8;
    mn.x = x; mn.y = y; mn.on = 1; mn.hover = -1;
    menu_draw();
    bar_draw();
}
void menu_close(void)
{
    palette_close();                                     /* (the lock closes both) */
    if (!mn.on) return;
    mn.on = 0; mn.from_bar = 0;
    menu_draw();
    bar_draw();
}
int menu_open_now(void) { return mn.on; }
void menu_forget(struct view *v)
{
    for (int i = 0; i < mn.n && mn.on; i++) if (mn.e[i].v == v) menu_close();
}

static void apps_entries(void) { for (int i = 0; i < napps; i++) ment(A_RUN, apps[i].icon, apps[i].name, apps[i].cmd, 0, 0); }
void menu_main(void)                                     /* the cube's: the apps, every window, Lock, Log out */
{
    mn.n = 0;
    apps_entries();
    ment(A_SETTINGS, I_SETTINGS, "Settings", 0, 0, 0);
    sep();
    window_entries(0, 0);
    sep();
    ment(A_LOCK, -1, "Lock", 0, 0, 0);
    ment(A_LOGOUT, -1, "Log out", 0, 0, 0);
    mn.from_bar = 1;
    menu_show(DBX, DBY + DB_MENU);
}
static void menu_desk(int x, int y)
{
    mn.n = 0;
    apps_entries();
    ment(A_SETTINGS, I_SETTINGS, "Settings", 0, 0, 0);
    sep();
    ment(A_LOCK, -1, "Lock", 0, 0, 0);
    ment(A_LOGOUT, -1, "Log out", 0, 0, 0);
    mn.from_bar = 0;
    menu_show(x, y);
}
static void menu_row(int k, int x, int y)                /* an app's: New ..., its windows */
{
    mn.n = 0;
    if (rows[k].app >= 0) {
        char t[64];
        snprintf(t, sizeof t, "New %s", rows[k].name);
        ment(A_RUN, rows[k].icon, t, apps[rows[k].app].cmd, 0, 0);
        sep();
    }
    window_entries(row_has, k);
    mn.from_bar = 0;
    menu_show(x, y);
}
void menu_window(struct view *v, int x, int y)           /* a window's: minimize, zoom, a workspace, close */
{
    if (!v || !v->mapped) return;
    mn.n = 0;
    ment(A_MIN, -1, "Minimize", 0, v, 0);
    ment(A_ZOOM, -1, v->max ? "Unzoom" : "Zoom", 0, v, 0);
    ment(A_FULL, -1, v->fs ? "Leave full screen" : "Full screen", 0, v, 0);
    sep();
    for (int k = 0; k < WM_NWS; k++) {
        if (k == v->ws) continue;
        char t[40];
        snprintf(t, sizeof t, "Send to workspace %d", k + 1);
        ment(A_SENDWS, -1, t, 0, v, k);
    }
    sep();
    ment(A_CLOSE, -1, "Close", 0, v, 0);
    mn.from_bar = 0;
    menu_show(x, y);
}

static void menu_do(int i)
{
    if (i < 0 || i >= mn.n || mn.e[i].act == A_SEP) return;
    typeof(mn.e[0]) e = mn.e[i];
    menu_close();
    switch (e.act) {
    case A_RUN: spawn(e.cmd); break;
    case A_WINDOW: wm_show(e.v); view_focus(e.v); break;
    case A_LOGOUT: atlas_quit(); break;
    case A_LOCK: lock_now(); break;
    case A_MIN: wm_min(e.v); break;
    case A_ZOOM: wm_toggle_max(e.v); break;
    case A_FULL: wm_fullscreen(e.v, !e.v->fs); break;
    case A_SENDWS: wm_send(e.v, e.arg); break;
    case A_CLOSE: view_close(e.v); break;
    case A_SETTINGS: settings_open(); break;
    }
    pointer_refocus();
}
void menu_key(xkb_keysym_t sym)
{
    if (!mn.on) return;
    int d = sym == XKB_KEY_Down ? 1 : sym == XKB_KEY_Up ? -1 : 0;
    if (d) {
        int i = mn.hover;
        for (int k = 0; k < mn.n; k++) {
            i = (i + d + mn.n) % mn.n;
            if (i < 0) i = 0;
            if (mn.e[i].act != A_SEP) break;
        }
        mn.hover = i;
        menu_draw();
    } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) menu_do(mn.hover);
    else if (sym == XKB_KEY_Escape) menu_close();
}

/* ---- The pointer over the Deskbar and the menus: what it lights. */
void bar_hover(int x, int y)
{
    int r = -1, m = -1;
    if (x >= DBX && x < DBX + DBW && y >= DBY && y < bar_y1) {
        const hit_t *h = hits_at(&bar_hits, x, y);
        r = h && h->kind == H_DBAPP ? h->arg : h && h->kind == H_DBMENU ? -2 : -1;
    }
    if (mn.on) {
        const hit_t *h = hits_at(&menu_hits, x, y);
        m = h && h->kind == H_MENU && h->arg >= 0 ? h->arg : -1;
        if (m != mn.hover && (m >= 0 || hover_menu_row)) { mn.hover = m; menu_draw(); }
        hover_menu_row = m >= 0;
    }
    if (r != hover_row) { hover_row = r; bar_draw(); }
}

/* A press on the Deskbar, the desk or a menu. -> 1 if taken. */
int bar_press(struct chrome *c, int x, int y, int button)
{
    int right = button == 0x111;                         /* (BTN_RIGHT) */
    if (c && c->kind == CH_MENU) {
        const hit_t *h = hits_at(&menu_hits, x, y);
        if (h && h->arg >= 0) menu_do(h->arg);
        return 1;
    }
    if (mn.on) {                                         /* (a press elsewhere closes the menu) */
        int was_bar = mn.from_bar;
        const hit_t *h = c && c->kind == CH_BAR ? hits_at(&bar_hits, x, y) : 0;
        menu_close();
        if (was_bar && h && h->kind == H_DBMENU) return 1;   /* (the cube again: only closed) */
        if (!c || c->kind != CH_BAR) return 1;
    }
    if (c && c->kind == CH_BAR) {
        const hit_t *h = hits_at(&bar_hits, x, y);
        if (!h) return 1;
        int na;
        struct view *a = asking(&na);
        if (h->kind == H_DBMENU) menu_main();
        else if (h->kind == H_DBBG && h->arg == 1 && a) { wm_show(a); view_focus(a); }   /* (the pill: the latest asking) */
        else if (h->kind == H_DBWS) wm_switch(h->arg);
        else if (h->kind == H_DBAPP && h->arg < nrows) {
            int k = h->arg;
            if (right) menu_row(k, x - 200, y);
            else if (rows[k].last) {                     /* running: its last window, in front */
                struct view *v = rows[k].last;
                if (v == view_focused() && wm_shown(v) && rows[k].n > 1) {   /* (again: the next of its windows) */
                    struct view *w, *pick = 0;
                    wl_list_for_each(w, &srv.views, link) if (w != v && w->mapped && row_has(k, w)) { pick = w; break; }
                    if (pick) v = pick;
                }
                wm_show(v);
                view_focus(v);
            } else if (rows[k].app >= 0) spawn(apps[rows[k].app].cmd);
        }
        return 1;
    }
    if (c && c->kind == CH_DESK) {
        if (right) menu_desk(x, y);
        return 1;
    }
    return 0;
}
