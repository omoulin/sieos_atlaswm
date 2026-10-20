/*
 * files.c - atlas-files: a folder's contents, in Atlas's look (from
 * SIEOS's Files, user/atlas/files.c, grown into a file manager).
 *
 *   [<] [>] [Up]  / > home > alice > Documents          [New folder]
 *   PLACES     │ NAME                      KIND      MODIFIED       SIZE
 *   Home       │ ▣ notes                   folder    17 Oct 14:52      -
 *   Documents  │ ▤ report.pdf              PDF       12 Oct 09:10  1.2 MB
 *   ...        │ ...
 *   3 items, 1 selected (1.2 MB)
 *
 * Folders first, then files, by name (or kind, date, size: click a
 * column's header; again: reversed); every other row tinted. A click
 * selects (Ctrl: one more, Shift: a range); a double-click (or Enter) opens:
 * a folder in place, a file with its app (xdg-open). The path's parts are
 * links; Ctrl+L types one. Places: the home folders, the drives mounted,
 * the Trash, the whole computer.
 *
 * Files are copied, moved and deleted by cp, mv and rm, one after the
 * other in processes of their own, so a long copy never stops the window;
 * names already taken get " (copy)" or " (2)". Deleting moves to the Trash
 * (freedesktop's, ~/.local/share/Trash: other file managers see it), from
 * where they can be restored; Shift+Delete deletes for good, once asked.
 * The folder is watched (inotify): what changes in it shows at once.
 *
 * Keys: arrows (Shift: extend), Page Up/Down, Home/End, Enter open,
 * Backspace or Alt+Up the folder above, Alt+Left/Right back and forward,
 * F2 rename, Delete to the Trash, Shift+Delete for good, Ctrl+C/X/V copy,
 * cut, paste, Ctrl+A all, Ctrl+H hidden files, Ctrl+L a path,
 * Ctrl+Shift+N a new folder, F5 again, Ctrl+W quit; letters jump to a name.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "files.h"

/* ---- Layout, in the picture's units (Atlas's: 0.75 pixel at 100%). */
#define BAR 58                                   /* the top bar */
#define SIDE_W 230                               /* the places (none in a narrow window) */
static int SIDE = SIDE_W;
#define HEAD 30                                  /* the columns' header */
#define ROW 34
#define STATUS 38
#define KINDW 130
#define DATEW 170
#define SIZEW 110

enum { K_NONE, K_BACK, K_FWD, K_UP, K_NEWDIR, K_EMPTY, K_RESTORE, K_CRUMB, K_PLACE, K_SORT, K_ROW, K_LIST, K_MENU, K_ASK, K_FIELD, K_SIDE };
enum { SORT_NAME, SORT_KIND, SORT_DATE, SORT_SIZE };
enum { A_OPEN = 1, A_RENAME, A_COPY, A_CUT, A_PASTE, A_TRASH, A_DELETE, A_NEWDIR, A_TERMINAL, A_HIDDEN, A_RESTORE, A_EMPTY, A_SEP };

typedef struct { char *name, *key; int dir, exec, link, sel; off_t size; time_t mtime; } ent_t;   /* key: the name as shown (accents folded), for sorting */
static ent_t *ent;
static int nent, capent, cur = -1, anchor = -1, scroll;
static char cwd[PATH_MAX], home[PATH_MAX], trash[PATH_MAX];
static int show_hidden, sort_by = SORT_NAME, sort_rev;
static hits_t hits;
static int mx = -1000000, my = -1000000;          /* the pointer */
static uint64_t last_click;
static int last_row = -1;
static char typed[64];                           /* letters typed: a name to jump to */
static uint64_t typed_t;

static char *hist[64];                           /* the folders before (back) and after (forward) */
static int nback, nfwd;
static char *fwdl[64];

static struct { char **path; int n, cut; } clip;
static struct { int on, x, y, w, n, hover; struct { char label[40]; int act; } e[16]; } mn;
static struct { int on, act; char text[160]; } ask;
static struct { int on, kind, idx; char buf[PATH_MAX]; int curs, s0, s1; } fld;   /* a name typed (rename, a new folder, a path); [s0, s1): chosen */
enum { F_RENAME = 1, F_LOCATION };
static char msg[200];
static int msg_err;

static int ino_fd = -1, ino_wd = -1;

/* ---- Small helpers. */
static void say_msg(int err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say_msg(int err, const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof msg, fmt, a);
    va_end(a);
    msg_err = err;
    win_redraw();
}
static void join(char *out, size_t cap, const char *dir, const char *name)
{
    snprintf(out, cap, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] == '/' ? "" : "/", name);
}
static const char *base(const char *p) { const char *b = strrchr(p, '/'); return b && b[1] ? b + 1 : p; }
static void mkdir_p(const char *path, mode_t mode)
{
    char t[PATH_MAX];
    snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; (p = strchr(p, '/')); p++) { *p = 0; mkdir(t, mode); *p = '/'; }
    mkdir(t, mode);
}
static int in_trash(void) { char f[PATH_MAX]; join(f, sizeof f, trash, "files"); return !strcmp(cwd, f); }
static void text(int x, int y, const char *s, int scale, uint32_t c, int x1)   /* (UTF-8 names: shown in ASCII) */
{
    char a[512];
    ascii(s, a, sizeof a);
    dl_text(x, y, a, strlen(a), scale, c, -(1 << 20), -(1 << 20), x1, 1 << 20);
}
static int cols_to(const char *s, int bytes)       /* how many characters (columns) the first bytes make */
{
    int c = 0;
    for (int i = 0; i < bytes && s[i]; i++) c += (s[i] & 0xc0) != 0x80;
    return c;
}
/* The field's text at (x, y), its chosen part lit, its cursor; from the
 * first column that leaves the cursor within room pixels. */
static void field_text(int x, int y, int h, int room, int x1)
{
    char a[PATH_MAX];
    ascii(fld.buf, a, sizeof a);
    int n = (int)strlen(a), c0 = cols_to(fld.buf, fld.s0), c1 = cols_to(fld.buf, fld.s1), cc = cols_to(fld.buf, fld.curs), from = 0;
    while (from < cc && text_sw(a + from, cc - from, SF(2)) > room) from++;
    if (c1 > c0) dl_rect(x + text_sw(a + from, (c0 > from ? c0 : from) - from, SF(2)), y - 3, x + text_sw(a + from, c1 - from, SF(2)), y + h - 3, 0, C_SEL);
    dl_text(x, y, a + from, n - from, SF(2), C_TXT, -(1 << 20), -(1 << 20), x1, 1 << 20);
    int cxp = x + text_sw(a + from, cc - from, SF(2));
    dl_rect(cxp, y - 1, cxp + 2, y + h - 5, 0, C_ACC);
}
static void size_text(off_t s, char *out, size_t cap)
{
    if (s < 1024) snprintf(out, cap, "%d B", (int)s);
    else if (s < 1024 * 1024) snprintf(out, cap, "%.1f KB", s / 1024.0);
    else if (s < 1024LL * 1024 * 1024) snprintf(out, cap, "%.1f MB", s / 1048576.0);
    else snprintf(out, cap, "%.1f GB", s / 1073741824.0);
}

/* What a file is, by its name: a word and an icon. */
static const char *kind_of(const ent_t *e, int *ic)
{
    *ic = e->dir ? I_FOLDER : e->exec ? I_APP : I_FILE;
    if (e->dir) return "folder";
    const char *x = strrchr(e->name, '.');
    if (!x || x == e->name) return e->exec ? "program" : "file";
    x++;
    static const struct { const char *ext, *kind; int icon; } k[] = {
        { "c", "C source", I_FILE }, { "h", "C header", I_FILE }, { "cpp", "C++ source", I_FILE }, { "py", "Python", I_FILE },
        { "sh", "shell", I_APP }, { "rs", "Rust", I_FILE }, { "go", "Go", I_FILE }, { "js", "JavaScript", I_FILE },
        { "txt", "text", I_FILE }, { "md", "Markdown", I_FILE }, { "conf", "settings", I_SETTINGS }, { "ini", "settings", I_SETTINGS },
        { "toml", "settings", I_SETTINGS }, { "yaml", "settings", I_SETTINGS }, { "yml", "settings", I_SETTINGS }, { "json", "JSON", I_FILE },
        { "html", "web page", I_WEB }, { "htm", "web page", I_WEB }, { "pdf", "PDF", I_FILE }, { "odt", "document", I_FILE },
        { "docx", "document", I_FILE }, { "ods", "spreadsheet", I_FILE }, { "xlsx", "spreadsheet", I_FILE },
        { "png", "image", I_CAM }, { "jpg", "image", I_CAM }, { "jpeg", "image", I_CAM }, { "gif", "image", I_CAM },
        { "webp", "image", I_CAM }, { "svg", "image", I_CAM }, { "ppm", "image", I_CAM }, { "mp4", "video", I_CAM },
        { "mkv", "video", I_CAM }, { "webm", "video", I_CAM }, { "mp3", "audio", I_FILE }, { "flac", "audio", I_FILE },
        { "ogg", "audio", I_FILE }, { "wav", "audio", I_FILE }, { "zip", "archive", I_FILE }, { "tar", "archive", I_FILE },
        { "gz", "archive", I_FILE }, { "xz", "archive", I_FILE }, { "zst", "archive", I_FILE }, { "7z", "archive", I_FILE },
        { "deb", "package", I_APP }, { "desktop", "launcher", I_APP }, { "app", "app", I_APP }, { "eml", "mail", I_MAIL },
        { "iso", "disk image", I_FILE }, { "gguf", "model", I_FILE },
    };
    for (size_t i = 0; i < sizeof k / sizeof *k; i++)
        if (!strcasecmp(x, k[i].ext)) { *ic = k[i].icon; return k[i].kind; }
    return e->exec ? "program" : "file";
}

/* Names in order as people read them: "file2" before "file10", case aside. */
static int natural(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ea = a, *eb = b;
            while (isdigit((unsigned char)*ea)) ea++;
            while (isdigit((unsigned char)*eb)) eb++;
            if (ea - a != eb - b) return (int)(ea - a) - (int)(eb - b);
            int c = strncmp(a, b, ea - a);
            if (c) return c;
            a = ea; b = eb;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
static int cmp(const void *pa, const void *pb)
{
    const ent_t *a = pa, *b = pb;
    if (a->dir != b->dir) return b->dir - a->dir;   /* (folders first, always) */
    int c = 0, i;
    if (sort_by == SORT_KIND) c = strcasecmp(kind_of(a, &i), kind_of(b, &i));
    else if (sort_by == SORT_DATE) c = a->mtime < b->mtime ? -1 : a->mtime > b->mtime;
    else if (sort_by == SORT_SIZE) c = a->size < b->size ? -1 : a->size > b->size;
    if (!c) c = natural(a->key, b->key);
    if (!c) c = strcmp(a->name, b->name);
    return sort_rev ? -c : c;
}

/* ---- The folder. */
static void free_ents(void) { for (int i = 0; i < nent; i++) { free(ent[i].name); free(ent[i].key); } nent = 0; }
static void watch(void)
{
    if (ino_fd < 0) return;
    if (ino_wd >= 0) inotify_rm_watch(ino_fd, ino_wd);
    ino_wd = inotify_add_watch(ino_fd, cwd, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
}
static void set_title(void)
{
    char t[300], a[300];
    ascii(!strcmp(cwd, "/") ? "Computer" : in_trash() ? "Trash" : base(cwd), a, sizeof a);
    snprintf(t, sizeof t, "%s", a);
    win_title(t);
}
/* Read the folder again; the rows chosen stay chosen (by name). */
static int load(void)
{
    DIR *d = opendir(cwd);
    if (!d) { say_msg(1, "Cannot open %s: %s", base(cwd), strerror(errno)); return -1; }
    char **keep = 0;
    int nkeep = 0;
    char *curname = cur >= 0 && cur < nent ? strdup(ent[cur].name) : 0;
    for (int i = 0; i < nent; i++) if (ent[i].sel) { keep = realloc(keep, (nkeep + 1) * sizeof *keep); keep[nkeep++] = strdup(ent[i].name); }
    free_ents();
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *n = de->d_name;
        if (!strcmp(n, ".") || !strcmp(n, "..") || (!show_hidden && n[0] == '.')) continue;
        if (nent == capent) { capent = capent ? capent * 2 : 256; ent = realloc(ent, capent * sizeof *ent); }
        ent_t *e = &ent[nent];
        memset(e, 0, sizeof *e);
        struct stat st, lst;
        if (fstatat(dirfd(d), n, &lst, AT_SYMLINK_NOFOLLOW) < 0) continue;
        e->link = S_ISLNK(lst.st_mode);
        if (fstatat(dirfd(d), n, &st, 0) < 0) st = lst;   /* (a broken link: as itself) */
        e->name = strdup(n);
        char k[NAME_MAX * 2 + 1];
        ascii(n, k, sizeof k);
        e->key = strdup(k);
        e->dir = S_ISDIR(st.st_mode);
        e->exec = !e->dir && (st.st_mode & 0111);
        e->size = st.st_size;
        e->mtime = st.st_mtime;
        nent++;
    }
    closedir(d);
    qsort(ent, nent, sizeof *ent, cmp);
    cur = -1;
    for (int i = 0; i < nent; i++) {
        for (int k = 0; k < nkeep; k++) if (!strcmp(keep[k], ent[i].name)) ent[i].sel = 1;
        if (curname && !strcmp(curname, ent[i].name)) cur = i;
    }
    for (int k = 0; k < nkeep; k++) free(keep[k]);
    free(keep);
    free(curname);
    win_redraw();
    return 0;
}
static void go(const char *path, int remember)
{
    char real[PATH_MAX];
    if (!realpath(path, real)) { say_msg(1, "No folder %s", path); return; }
    DIR *d = opendir(real);
    if (!d) { say_msg(1, "Cannot open %s: %s", base(real), strerror(errno)); return; }
    closedir(d);
    if (remember && strcmp(real, cwd)) {
        if (nback == 64) { free(hist[0]); memmove(hist, hist + 1, 63 * sizeof *hist); nback--; }
        hist[nback++] = strdup(cwd);
        for (int i = 0; i < nfwd; i++) free(fwdl[i]);
        nfwd = 0;
    }
    int same = !strcmp(real, cwd);
    snprintf(cwd, sizeof cwd, "%s", real);
    if (!same) { free_ents(); cur = anchor = -1; scroll = 0; }
    msg[0] = 0;
    load();
    watch();
    set_title();
}
static void back(void)
{
    if (!nback) return;
    if (nfwd < 64) fwdl[nfwd++] = strdup(cwd);
    char *p = hist[--nback];
    go(p, 0);
    free(p);
}
static void forward(void)
{
    if (!nfwd) return;
    if (nback < 64) hist[nback++] = strdup(cwd);
    char *p = fwdl[--nfwd];
    go(p, 0);
    free(p);
}
static void up(void)
{
    if (!strcmp(cwd, "/")) return;
    char p[PATH_MAX], *s;
    snprintf(p, sizeof p, "%s", cwd);
    char was[NAME_MAX + 1];
    snprintf(was, sizeof was, "%s", base(p));
    s = strrchr(p, '/');
    if (s == p) p[1] = 0; else *s = 0;
    go(p, 1);
    for (int i = 0; i < nent; i++) if (!strcmp(ent[i].name, was)) { cur = anchor = i; ent[i].sel = 1; }   /* (the folder we came from: chosen) */
}
static void changed(void)                        /* inotify: the folder changed */
{
    char buf[4096];
    int gone = 0;
    ssize_t n;
    while ((n = read(ino_fd, buf, sizeof buf)) > 0)
        for (char *p = buf; p < buf + n; ) {
            struct inotify_event *e = (struct inotify_event *)p;
            if (e->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) gone = 1;
            p += sizeof *e + e->len;
        }
    if (gone) { say_msg(1, "The folder is gone"); up(); return; }
    load();
}

/* ---- Selection. */
static int nsel(void) { int n = 0; for (int i = 0; i < nent; i++) n += ent[i].sel; return n; }
static void select_only(int i) { for (int k = 0; k < nent; k++) ent[k].sel = k == i; cur = anchor = i; }
static void select_range(int a, int b)
{
    if (a > b) { int t = a; a = b; b = t; }
    for (int k = 0; k < nent; k++) ent[k].sel = k >= a && k <= b;
}
static int list_rows(void) { int n = (SH - STATUS - (BAR + HEAD) - 8) / ROW; return n < 1 ? 1 : n; }
static void show_cur(void)
{
    int n = list_rows();
    if (cur >= 0 && cur < scroll) scroll = cur;
    if (cur >= scroll + n) scroll = cur - n + 1;
    if (scroll > nent - n) scroll = nent - n;
    if (scroll < 0) scroll = 0;
}

/* ---- Running things: apps (on their own), and the file jobs (one after
 * the other: cp, mv, rm), watched by a timer. */
static void spawn(const char *dir, char *const argv[])  /* (twice forked: never a zombie of ours) */
{
    pid_t p = fork();
    if (p == 0) {
        setsid();
        if (fork() == 0) {
            if (dir && chdir(dir) < 0) _exit(126);
            execvp(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0) waitpid(p, NULL, 0);
}
static void open_path(const char *path)
{
    char *argv[] = { "xdg-open", (char *)path, 0 };
    spawn(0, argv);
    say_msg(0, "Opening %s", base(path));
}
static void terminal_here(void)
{
    const char *t = getenv("TERMINAL");
    static const char *terms[] = { "foot", "kgx", "alacritty", "kitty", "konsole", "gnome-terminal", "xterm", 0 };
    char path[PATH_MAX];
    for (int i = 0; !t && terms[i]; i++) {
        for (const char *p = getenv("PATH") ? getenv("PATH") : "/usr/bin"; *p; ) {
            const char *e = strchr(p, ':');
            size_t k = e ? (size_t)(e - p) : strlen(p);
            snprintf(path, sizeof path, "%.*s/%s", (int)k, p, terms[i]);
            if (access(path, X_OK) == 0) { t = terms[i]; break; }
            p += k + (e ? 1 : 0);
        }
    }
    if (!t) { say_msg(1, "No terminal found (set TERMINAL)"); return; }
    char *argv[] = { (char *)t, 0 };
    spawn(cwd, argv);
}

#define MAXJOB 256
static struct job { char **argv; char what[80]; } jobs[MAXJOB];
static int njob, failed, done_n;
static pid_t running_pid;
static void job_add(const char *what, ...)          /* argv..., 0 */
{
    if (njob == MAXJOB) return;
    va_list a;
    va_start(a, what);
    char **v = 0;
    int n = 0;
    for (const char *s; (s = va_arg(a, const char *)); ) { v = realloc(v, (n + 2) * sizeof *v); v[n++] = strdup(s); }
    va_end(a);
    v[n] = 0;
    jobs[njob].argv = v;
    snprintf(jobs[njob].what, sizeof jobs[0].what, "%s", what);
    njob++;
}
static void job_tick(void);
static void job_start(void)
{
    if (running_pid || !njob) return;
    pid_t p = fork();
    if (p == 0) { execvp(jobs[0].argv[0], jobs[0].argv); _exit(127); }
    if (p < 0) { say_msg(1, "Cannot run %s", jobs[0].argv[0]); return; }
    running_pid = p;
    say_msg(0, "%s...", jobs[0].what);
    win_timer(120, job_tick);
}
static void job_tick(void)
{
    int st;
    pid_t r = waitpid(running_pid, &st, WNOHANG);
    if (r == 0) { win_timer(120, job_tick); return; }
    running_pid = 0;
    if (r < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) failed++;
    else done_n++;
    for (char **v = jobs[0].argv; *v; v++) free(*v);
    free(jobs[0].argv);
    memmove(jobs, jobs + 1, --njob * sizeof *jobs);
    if (njob) { job_start(); return; }
    if (failed) say_msg(1, "Done, %d failed", failed);
    else say_msg(0, "Done (%d)", done_n);
    failed = done_n = 0;
    load();
}

/* A free name in dir: name, else "name (copy)" / "name (2)"..., before the extension. */
static void free_name(const char *dir, const char *name, int copy, char *out, size_t cap)
{
    char p[PATH_MAX];
    join(p, sizeof p, dir, name);
    struct stat st;
    if (lstat(p, &st) < 0) { snprintf(out, cap, "%s", p); return; }
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    for (int k = 1; k < 1000; k++) {
        char n[NAME_MAX + 32];
        if (k == 1 && copy) snprintf(n, sizeof n, "%.*s (copy)%s", stem, name, name + stem);
        else snprintf(n, sizeof n, "%.*s (%d)%s", stem, name, k + 1, name + stem);
        join(p, sizeof p, dir, n);
        if (lstat(p, &st) < 0) { snprintf(out, cap, "%s", p); return; }
    }
    snprintf(out, cap, "%s", p);
}

/* ---- What the actions do. */
static void selected_paths(char ***out, int *n)
{
    *out = 0; *n = 0;
    for (int i = 0; i < nent; i++) if (ent[i].sel) {
        char p[PATH_MAX];
        join(p, sizeof p, cwd, ent[i].name);
        *out = realloc(*out, (*n + 1) * sizeof **out);
        (*out)[(*n)++] = strdup(p);
    }
}
static void open_sel(void)
{
    if (cur >= 0 && cur < nent && ent[cur].sel && ent[cur].dir) { char p[PATH_MAX]; join(p, sizeof p, cwd, ent[cur].name); go(p, 1); return; }
    int opened = 0;
    for (int i = 0; i < nent && opened < 10; i++) if (ent[i].sel && !ent[i].dir) { char p[PATH_MAX]; join(p, sizeof p, cwd, ent[i].name); open_path(p); opened++; }
    if (!opened) for (int i = 0; i < nent; i++) if (ent[i].sel && ent[i].dir) { char p[PATH_MAX]; join(p, sizeof p, cwd, ent[i].name); go(p, 1); return; }
}
static void clip_set(int cut)
{
    for (int i = 0; i < clip.n; i++) free(clip.path[i]);
    free(clip.path);
    selected_paths(&clip.path, &clip.n);
    clip.cut = cut;
    if (clip.n) say_msg(0, "%d item%s to %s", clip.n, clip.n > 1 ? "s" : "", cut ? "move" : "copy");
}
static void paste(void)
{
    if (!clip.n) { say_msg(1, "Nothing copied"); return; }
    int queued = 0;
    for (int i = 0; i < clip.n; i++) {
        char dir[PATH_MAX], dst[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", clip.path[i]);
        *strrchr(dir, '/') = 0;
        if (!dir[0]) strcpy(dir, "/");
        if (clip.cut && !strcmp(dir, cwd)) continue;     /* (moved where it is: nothing) */
        size_t l = strlen(clip.path[i]);
        if (!strncmp(cwd, clip.path[i], l) && (cwd[l] == '/' || !cwd[l])) { say_msg(1, "Cannot put a folder inside itself"); continue; }
        free_name(cwd, base(clip.path[i]), !strcmp(dir, cwd), dst, sizeof dst);
        if (clip.cut) job_add("Moving", "mv", "-n", "--", clip.path[i], dst, (char *)0);
        else job_add("Copying", "cp", "-a", "--", clip.path[i], dst, (char *)0);
        queued++;
    }
    if (clip.cut) { for (int i = 0; i < clip.n; i++) free(clip.path[i]); free(clip.path); clip.path = 0; clip.n = 0; }
    if (queued) job_start();
}
/* Into the Trash (the freedesktop way: the file in files/, its old path and
 * the date in info/NAME.trashinfo). */
static void trash_sel(void)
{
    char f[PATH_MAX], info[PATH_MAX];
    join(f, sizeof f, trash, "files");
    join(info, sizeof info, trash, "info");
    mkdir_p(f, 0700);
    mkdir_p(info, 0700);
    int n = 0, slow = 0;
    for (int i = 0; i < nent; i++) {
        if (!ent[i].sel) continue;
        char src[PATH_MAX], dst[PATH_MAX], inf[PATH_MAX + 16];
        join(src, sizeof src, cwd, ent[i].name);
        free_name(f, ent[i].name, 0, dst, sizeof dst);
        snprintf(inf, sizeof inf, "%s/%s.trashinfo", info, base(dst));
        FILE *fi = fopen(inf, "w");
        if (!fi) { say_msg(1, "Cannot write to the Trash"); return; }
        time_t now = time(0);
        struct tm tm;
        localtime_r(&now, &tm);
        char date[32];
        strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
        fprintf(fi, "[Trash Info]\nPath=");
        for (const char *p = src; *p; p++)               /* (the path, percent-encoded as the spec asks) */
            if (isalnum((unsigned char)*p) || strchr("/-_.~", *p)) fputc(*p, fi); else fprintf(fi, "%%%02X", (unsigned char)*p);
        fprintf(fi, "\nDeletionDate=%s\n", date);
        fclose(fi);
        if (rename(src, dst) == 0) n++;
        else if (errno == EXDEV) { job_add("Moving to the Trash", "mv", "--", src, dst, (char *)0); slow++; }
        else { unlink(inf); say_msg(1, "Cannot move %s to the Trash: %s", ent[i].name, strerror(errno)); }
    }
    if (n) say_msg(0, "%d item%s moved to the Trash", n, n > 1 ? "s" : "");
    if (slow) job_start();
    load();
}
static void delete_sel(void)                     /* for good (asked first) */
{
    char **p;
    int n;
    selected_paths(&p, &n);
    for (int i = 0; i < n; i++) {
        job_add("Deleting", "rm", "-rf", "--", p[i], (char *)0);
        if (in_trash()) {                                /* (and its info) */
            char inf[PATH_MAX + 32];
            snprintf(inf, sizeof inf, "%s/info/%s.trashinfo", trash, base(p[i]));
            unlink(inf);
        }
        free(p[i]);
    }
    free(p);
    job_start();
}
static void restore_sel(void)                    /* from the Trash, where each was */
{
    int n = 0;
    for (int i = 0; i < nent; i++) {
        if (!ent[i].sel) continue;
        char inf[PATH_MAX + 32], line[PATH_MAX + 16], to[PATH_MAX] = "", src[PATH_MAX];
        snprintf(inf, sizeof inf, "%s/info/%s.trashinfo", trash, ent[i].name);
        FILE *f = fopen(inf, "r");
        while (f && fgets(line, sizeof line, f))
            if (!strncmp(line, "Path=", 5)) {
                size_t k = 0;
                for (const char *p = line + 5; *p && *p != '\n' && k + 1 < sizeof to; p++)
                    if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) { char h[3] = { p[1], p[2], 0 }; to[k++] = (char)strtol(h, 0, 16); p += 2; }
                    else to[k++] = *p;
                to[k] = 0;
            }
        if (f) fclose(f);
        if (!to[0]) { say_msg(1, "%s: where it was is not known", ent[i].name); continue; }
        char dir[PATH_MAX], dst[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", to);
        *strrchr(dir, '/') = 0;
        if (!dir[0]) strcpy(dir, "/");
        mkdir(dir, 0755);
        free_name(dir, base(to), 0, dst, sizeof dst);
        join(src, sizeof src, cwd, ent[i].name);
        if (rename(src, dst) == 0) { unlink(inf); n++; }
        else if (errno == EXDEV) { job_add("Restoring", "mv", "--", src, dst, (char *)0); unlink(inf); }
        else say_msg(1, "Cannot restore %s: %s", ent[i].name, strerror(errno));
    }
    if (n) say_msg(0, "%d item%s restored", n, n > 1 ? "s" : "");
    job_start();
    load();
}
static void empty_trash(void)
{
    char f[PATH_MAX], info[PATH_MAX];
    join(f, sizeof f, trash, "files");
    join(info, sizeof info, trash, "info");
    const char *dirs[2] = { f, info };
    for (int k = 0; k < 2; k++) {
        DIR *d = opendir(dirs[k]);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char p[PATH_MAX];
            join(p, sizeof p, dirs[k], de->d_name);
            job_add("Emptying the Trash", "rm", "-rf", "--", p, (char *)0);
        }
        if (d) closedir(d);
    }
    job_start();
}
static void ask_for(int act, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ask_for(int act, const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vsnprintf(ask.text, sizeof ask.text, fmt, a);
    va_end(a);
    ask.on = 1; ask.act = act;
    win_redraw();
}

/* The field: a name being typed (rename, a new folder), or a path. */
static void field_start(int kind, int idx, const char *text)
{
    fld.on = 1; fld.kind = kind; fld.idx = idx;
    snprintf(fld.buf, sizeof fld.buf, "%s", text);
    fld.curs = fld.s1 = (int)strlen(fld.buf);
    fld.s0 = 0;
    if (kind == F_RENAME) {                              /* (the name chosen, its extension not) */
        const char *dot = strrchr(fld.buf, '.');
        if (dot && dot != fld.buf && !(idx >= 0 && idx < nent && ent[idx].dir)) fld.curs = fld.s1 = (int)(dot - fld.buf);
    }
    win_redraw();
}
static void field_done(int ok)
{
    if (!fld.on) return;
    fld.on = 0;
    win_redraw();
    if (!ok) return;
    if (fld.kind == F_LOCATION) {
        char p[PATH_MAX];
        if (fld.buf[0] == '~') snprintf(p, sizeof p, "%s%s", home, fld.buf + 1);
        else if (fld.buf[0] != '/') join(p, sizeof p, cwd, fld.buf);
        else snprintf(p, sizeof p, "%s", fld.buf);
        go(p, 1);
        return;
    }
    if (fld.idx < 0 || fld.idx >= nent || !fld.buf[0] || strchr(fld.buf, '/')) { if (strchr(fld.buf, '/')) say_msg(1, "A name cannot hold /"); return; }
    if (!strcmp(fld.buf, ent[fld.idx].name)) return;
    char a[PATH_MAX], b[PATH_MAX];
    join(a, sizeof a, cwd, ent[fld.idx].name);
    join(b, sizeof b, cwd, fld.buf);
    struct stat st;
    if (lstat(b, &st) == 0) { say_msg(1, "%s exists already", fld.buf); return; }
    if (rename(a, b) < 0) { say_msg(1, "Cannot rename: %s", strerror(errno)); return; }
    char *keep = strdup(fld.buf);
    load();
    for (int i = 0; i < nent; i++) if (!strcmp(ent[i].name, keep)) { select_only(i); show_cur(); }
    free(keep);
}
static void new_folder(void)
{
    char p[PATH_MAX];
    free_name(cwd, "New folder", 0, p, sizeof p);
    if (mkdir(p, 0755) < 0) { say_msg(1, "Cannot make a folder here: %s", strerror(errno)); return; }
    load();
    for (int i = 0; i < nent; i++) if (!strcmp(ent[i].name, base(p))) { select_only(i); show_cur(); field_start(F_RENAME, i, ent[i].name); }
}
static void rename_cur(void)
{
    if (cur < 0 || cur >= nent) return;
    select_only(cur);
    show_cur();
    field_start(F_RENAME, cur, ent[cur].name);
}

static void act(int a)
{
    switch (a) {
    case A_OPEN: open_sel(); break;
    case A_RENAME: rename_cur(); break;
    case A_COPY: clip_set(0); break;
    case A_CUT: clip_set(1); break;
    case A_PASTE: paste(); break;
    case A_TRASH: if (nsel()) { if (in_trash()) ask_for(A_DELETE, "Delete %d item%s for good?", nsel(), nsel() > 1 ? "s" : ""); else trash_sel(); } break;
    case A_DELETE: if (nsel()) ask_for(A_DELETE, "Delete %d item%s for good?", nsel(), nsel() > 1 ? "s" : ""); break;
    case A_NEWDIR: new_folder(); break;
    case A_TERMINAL: terminal_here(); break;
    case A_HIDDEN: show_hidden = !show_hidden; load(); say_msg(0, show_hidden ? "Hidden files shown" : "Hidden files hidden"); break;
    case A_RESTORE: restore_sel(); break;
    case A_EMPTY: ask_for(A_EMPTY, "Empty the Trash? Everything in it goes for good."); break;
    }
    win_redraw();
}

/* ---- The menu (right-click): what can be done with the rows chosen, or here. */
static void ment(const char *label, int a)
{
    if (mn.n < 16) { snprintf(mn.e[mn.n].label, sizeof mn.e[0].label, "%s", label); mn.e[mn.n++].act = a; }
}
static void menu_at(int x, int y, int on_row)
{
    mn.n = 0;
    if (on_row) {
        ment("Open", A_OPEN);
        if (in_trash()) ment("Restore", A_RESTORE);
        else { ment("Rename", A_RENAME); ment("Copy", A_COPY); ment("Cut", A_CUT); }
        ment("", A_SEP);
        if (!in_trash()) ment("Move to the Trash", A_TRASH);
        ment("Delete for good", A_DELETE);
    } else {
        if (in_trash()) ment("Empty the Trash", A_EMPTY);
        else { ment("New folder", A_NEWDIR); ment("Paste", A_PASTE); }
        ment("Terminal here", A_TERMINAL);
        ment("", A_SEP);
        ment(show_hidden ? "Hide hidden files" : "Show hidden files", A_HIDDEN);
    }
    int w = 220, h = 16;
    for (int i = 0; i < mn.n; i++) { int k = tws(mn.e[i].label, 2) + 40; if (k > w) w = k; h += mn.e[i].act == A_SEP ? 12 : 36; }
    if (x + w > SW - 8) x = SW - 8 - w;
    if (y + h > SH - 8) y = SH - 8 - h;
    mn.x = x < 8 ? 8 : x; mn.y = y < 8 ? 8 : y; mn.w = w; mn.on = 1; mn.hover = -1;
    win_redraw();
}

/* ---- Places: home folders, drives, the Trash, the computer. */
#define MAXPLACE 20
static struct { char name[64], path[PATH_MAX]; int icon; } places[MAXPLACE];
static int nplace;
static void add_place(const char *name, const char *path, int icon)
{
    struct stat st;
    if (nplace == MAXPLACE || stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) return;
    snprintf(places[nplace].name, sizeof places[0].name, "%s", name);
    snprintf(places[nplace].path, sizeof places[0].path, "%s", path);
    places[nplace++].icon = icon;
}
static void find_places(void)
{
    nplace = 0;
    char p[PATH_MAX];
    add_place("Home", home, I_FOLDER);
    static const char *dirs[] = { "Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) { join(p, sizeof p, home, dirs[i]); add_place(dirs[i], p, I_FOLDER); }
    FILE *f = fopen("/proc/self/mounts", "r");           /* (drives: what is mounted under /media, /run/media, /mnt) */
    char line[1024];
    while (f && fgets(line, sizeof line, f)) {
        char dev[256], mnt[512];
        if (sscanf(line, "%255s %511s", dev, mnt) != 2) continue;
        if (strncmp(mnt, "/media/", 7) && strncmp(mnt, "/run/media/", 11) && strncmp(mnt, "/mnt/", 5)) continue;
        for (char *q = mnt; (q = strstr(q, "\\040")); ) { *q = ' '; memmove(q + 1, q + 4, strlen(q + 4) + 1); }
        add_place(base(mnt), mnt, I_FILE);
    }
    if (f) fclose(f);
    join(p, sizeof p, trash, "files");
    mkdir_p(p, 0700);
    add_place("Trash", p, I_APP);
    add_place("Computer", "/", I_WINDOW);
}

/* ---- Drawing. */
static int pill(int x, int y, const char *s, int on, int kind, int arg, int enabled)   /* -> its right edge */
{
    int w = tws(s, 2) + 28;
    dl_rect(x, y, x + w, y + 42, 21, on ? C_ACC : C_FIELD);
    dl_frame(x, y, x + w, y + 42, 21, 1, on ? C_ACC : C_LINE);
    dl_text(x + 14, y + 10, s, strlen(s), SF(2), on ? C_INK : enabled ? C_TXT : C_LINE, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);
    if (enabled) hitbox(x, y, x + w, y + 42, kind, arg);
    return x + w;
}
static int crumb_x1;
static void draw_bar(void)
{
    int y = 8, x = pill(14, y, "<", 0, K_BACK, 0, nback > 0) + 6;
    x = pill(x, y, ">", 0, K_FWD, 0, nfwd > 0) + 6;
    x = pill(x, y, "Up", 0, K_UP, 0, strcmp(cwd, "/") != 0) + 16;
    int rx = SW - 16;
    if (in_trash()) { rx -= tws("Empty Trash", 2) + 28; pill(rx, y, "Empty Trash", 0, K_EMPTY, 0, nent > 0); }
    else { rx -= tws("New folder", 2) + 28; pill(rx, y, "New folder", 0, K_NEWDIR, 0, access(cwd, W_OK) == 0); }
    crumb_x1 = rx - 16;
    if (fld.on && fld.kind == F_LOCATION) {              /* a path being typed */
        dl_rect(x, y, crumb_x1, y + 42, 10, C_FIELD);
        dl_frame(x, y, crumb_x1, y + 42, 10, 2, C_ACC);
        field_text(x + 14, y + 10, 28, crumb_x1 - x - 28, crumb_x1 - 8);
        hitbox(x, y, crumb_x1, y + 42, K_FIELD, 0);
        return;
    }
    /* the path: each part a link, the last one bold; from the end if it is long */
    const char *parts[128];
    int lens[128], np = 0;
    parts[np] = "/"; lens[np++] = 1;
    for (const char *p = cwd + 1; *p && np < 128; ) {
        const char *e = strchr(p, '/');
        int l = e ? (int)(e - p) : (int)strlen(p);
        parts[np] = p; lens[np++] = l;
        p += l + (e ? 1 : 0);
    }
    static char a[128][41];                              /* (each part as drawn: ASCII, 40 at most) */
    int first = 0, need, ws[128];
    for (int k = 0; k < np; k++) {
        char nm[256], t[256];
        snprintf(nm, sizeof nm, "%.*s", lens[k] > 200 ? 200 : lens[k], parts[k]);
        ascii(nm, t, sizeof t);
        snprintf(a[k], sizeof a[k], "%s", t);
        ws[k] = tws(a[k], 2);
    }
    do {
        need = 0;
        for (int k = first; k < np; k++) need += ws[k] + (k < np - 1 ? 30 : 0);
        if (x + need > crumb_x1 && first < np - 1) first++; else break;
    } while (1);
    for (int k = first; k < np; k++) {
        int l = strlen(a[k]), w = ws[k], last = k == np - 1;
        dl_text(x, y + 10, a[k], l, SF(2), last ? C_TXT : C_DIM, -(1 << 20), -(1 << 20), crumb_x1, 1 << 20);
        if (last) dl_text(x + 1, y + 10, a[k], l, SF(2), C_TXT, -(1 << 20), -(1 << 20), crumb_x1, 1 << 20);
        else hitbox(x - 4, y + 4, x + w + 4, y + 38, K_CRUMB, k);
        x += w;
        if (!last) { dl_text(x + 9, y + 12, ">", 1, SSS, C_LINE, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20); x += 30; }
    }
    hitbox(x, y, crumb_x1, y + 42, K_FIELD, 1);          /* (after the path: type one) */
}
static void draw_side(void)
{
    int y = BAR + 6;
    dl_rect(0, BAR, SIDE, SH - STATUS, 0, C_PANEL);
    dl_rect(SIDE - 1, BAR, SIDE, SH - STATUS, 0, C_LINE);
    hitbox(0, BAR, SIDE, SH - STATUS, K_SIDE, 0);
    dl_text(20, y + 6, "PLACES", 6, SSS, C_DIM, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);
    y += 32;
    for (int i = 0; i < nplace && y + ROW <= SH - STATUS; i++, y += ROW + 2) {
        int on = !strcmp(cwd, places[i].path), hov = mx >= 0 && mx < SIDE && my >= y && my < y + ROW;
        if (on) dl_rect(8, y, SIDE - 8, y + ROW, 8, C_SEL);
        else if (hov) dl_rect(8, y, SIDE - 8, y + ROW, 8, C_BAR);
        icon(places[i].icon, 18, y + 5, 2);
        text(52, y + 7, places[i].name, SF(2), on ? C_TXT : C_DIM, SIDE - 12);
        hitbox(8, y, SIDE - 8, y + ROW, K_PLACE, i);
    }
}
static void draw_list(void)
{
    int x0 = SIDE, x1 = SW, hy = BAR, ly = hy + HEAD, y1 = SH - STATUS;
    int sx = x1 - 24 - SIZEW, dx = sx - DATEW, kx = dx - KINDW;
    int wide = kx > x0 + 300, mid = dx > x0 + 260;       /* (room for the kind, the date) */
    dl_rect(x0, hy, x1, y1, 0, C_TERM);
    hitbox(x0, ly, x1, y1, K_LIST, 0);
    struct { const char *t; int x, key; } cols[4] = { { "NAME", x0 + 52, SORT_NAME }, { "KIND", kx, SORT_KIND }, { "MODIFIED", dx, SORT_DATE }, { "SIZE", x1 - 24 - text_sw("SIZE", 4, SSS), SORT_SIZE } };
    for (int c = 0; c < 4; c++) {
        if ((c == 1 && !wide) || (c == 2 && !mid)) continue;
        int on = sort_by == cols[c].key;
        int w = text_sw(cols[c].t, strlen(cols[c].t), SSS);
        dl_text(cols[c].x, hy + 8, cols[c].t, strlen(cols[c].t), SSS, on ? C_TXT : C_DIM, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);
        if (on) dl_text(cols[c].x + w + 6, hy + 8, sort_rev ? "v" : "^", 1, SSS, C_ACCT, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);
        hitbox(cols[c].x - 8, hy, cols[c].x + w + 24, ly, K_SORT, cols[c].key);
    }
    dl_rect(x0 + 10, ly - 1, x1 - 10, ly, 0, C_LINE);
    int n = list_rows();
    if (scroll > nent - n) scroll = nent - n;
    if (scroll < 0) scroll = 0;
    if (!nent) {                                         /* nothing: say why, in the middle */
        const char *m = access(cwd, R_OK) ? "You may not read this folder" : in_trash() ? "The Trash is empty" : "This folder is empty";
        dl_text((x0 + x1) / 2 - tws(m, 2) / 2, (ly + y1) / 2 - 11, m, strlen(m), SF(2), C_DIM, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);
    }
    time_t now = time(0);
    struct tm tn;
    localtime_r(&now, &tn);
    for (int r = 0; r < n && scroll + r < nent; r++) {
        int i = scroll + r, y = ly + 4 + r * ROW;
        ent_t *e = &ent[i];
        int hov = mx >= x0 && my >= y && my < y + ROW - 2 && !mn.on;
        if (e->sel) dl_rect(x0 + 6, y, x1 - 6, y + ROW - 2, 6, C_SEL);
        else if (hov) dl_rect(x0 + 6, y, x1 - 6, y + ROW - 2, 6, C_BAR);
        else if (r & 1) dl_rect(x0 + 6, y, x1 - 6, y + ROW - 2, 6, C_PANEL);
        if (i == cur && win_focused()) dl_frame(x0 + 6, y, x1 - 6, y + ROW - 2, 6, 1, C_ACC);
        int ic;
        const char *kd = kind_of(e, &ic);
        int cut = 0;
        if (clip.cut) for (int k = 0; k < clip.n && !cut; k++) { char p[PATH_MAX]; join(p, sizeof p, cwd, e->name); cut = !strcmp(p, clip.path[k]); }
        icon(ic, x0 + 16, y + 4, 2);
        int nx1 = (wide ? kx : mid ? dx : sx) - 16;
        if (fld.on && fld.kind == F_RENAME && fld.idx == i) {   /* being renamed: a field in its place */
            dl_rect(x0 + 44, y - 2, nx1, y + ROW, 6, C_FIELD);
            dl_frame(x0 + 44, y - 2, nx1, y + ROW, 6, 2, C_ACC);
            field_text(x0 + 52, y + 6, 28, nx1 - x0 - 72, nx1 - 6);
            hitbox(x0 + 44, y - 2, nx1, y + ROW, K_FIELD, 2);
        } else {
            uint32_t c = cut ? C_DIM : C_TXT;
            text(x0 + 52, y + 6, e->name, SF(2), c, nx1);
            if (e->dir) text(x0 + 53, y + 6, e->name, SF(2), c, nx1);   /* (folders bold) */
            if (e->link) dl_text(x0 + 14, y + 20, "~", 1, SS, C_ACCT, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);   /* (a link) */
        }
        if (wide) dl_text(kx, y + 8, kd, strlen(kd), SSS, C_DIM, -(1 << 20), -(1 << 20), dx - 12, 1 << 20);
        if (mid) {
            struct tm tm;
            char d[32];
            localtime_r(&e->mtime, &tm);
            strftime(d, sizeof d, tm.tm_year == tn.tm_year ? "%d %b %H:%M" : "%d %b %Y", &tm);
            dl_text(dx, y + 8, d, strlen(d), SS, C_DIM, -(1 << 20), -(1 << 20), sx - 12, 1 << 20);
        }
        char sz[24];
        if (e->dir) snprintf(sz, sizeof sz, "-");
        else size_text(e->size, sz, sizeof sz);
        dl_text(x1 - 24 - text_w(strlen(sz), SS), y + 8, sz, strlen(sz), SS, C_DIM, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);   /* (numbers, the date too: mono) */
        hitbox(x0 + 6, y, x1 - 6, y + ROW - 2, K_ROW, i);
    }
    if (nent > n) {                                      /* the scroll bar */
        int h = y1 - ly - 8, th = h * n / nent < 30 ? 30 : h * n / nent, ty = ly + 4 + (h - th) * scroll / (nent - n);
        dl_rect(x1 - 6, ty, x1 - 2, ty + th, 2, C_LINE);
    }
}
static void draw_status(void)
{
    int y = SH - STATUS;
    dl_rect(0, y, SW, SH, 0, C_PANEL);
    dl_rect(0, y, SW, y + 1, 0, C_LINE);
    char t[240];
    if (ask.on) {                                        /* a question: its two answers */
        dl_text(16, y + 10, ask.text, strlen(ask.text), SF(2), C_TXT, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);
        int x = 16 + tws(ask.text, 2) + 20, dw = tws("Delete", 2) + 28, cw = tws("Cancel", 2) + 28;
        dl_rect(x, y + 4, x + dw, y + 34, 15, C_ACC);
        dl_text(x + 14, y + 10, "Delete", 6, SF(2), C_INK, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);
        hitbox(x, y + 4, x + dw, y + 34, K_ASK, 1);
        x += dw + 12;
        dl_frame(x, y + 4, x + cw, y + 34, 15, 1, C_LINE);
        dl_text(x + 14, y + 10, "Cancel", 6, SF(2), C_TXT, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);
        hitbox(x, y + 4, x + cw, y + 34, K_ASK, 0);
        return;
    }
    int s = nsel();
    off_t bytes = 0;
    for (int i = 0; i < nent; i++) if (ent[i].sel && !ent[i].dir) bytes += ent[i].size;
    char b[24];
    size_text(bytes, b, sizeof b);
    if (s) snprintf(t, sizeof t, "%d item%s, %d selected%s%s%s", nent, nent == 1 ? "" : "s", s, bytes ? " (" : "", bytes ? b : "", bytes ? ")" : "");
    else snprintf(t, sizeof t, "%d item%s%s", nent, nent == 1 ? "" : "s", show_hidden ? ", hidden ones shown" : "");
    dl_text(16, y + 10, t, strlen(t), SSS, C_DIM, -(1 << 20), -(1 << 20), SW / 2, 1 << 20);
    if (msg[0]) {
        char a[240];
        ascii(msg, a, sizeof a);
        int w = text_sw(a, strlen(a), SSS);
        dl_text(SW - 16 - w, y + 10, a, strlen(a), SSS, msg_err ? C_ERR : C_ACCT, SW / 2, -(1 << 20), 1 << 20, 1 << 20);
    }
}
static void draw_menu(void)
{
    if (!mn.on) return;
    int h = 16;
    for (int i = 0; i < mn.n; i++) h += mn.e[i].act == A_SEP ? 12 : 36;
    dl_shade(mn.x + 6, mn.y + 6, mn.x + mn.w + 6, mn.y + h + 6, 12, 1);
    dl_rect(mn.x, mn.y, mn.x + mn.w, mn.y + h, 12, C_GLASS);
    hitbox(mn.x, mn.y, mn.x + mn.w, mn.y + h, K_MENU, -1);
    int y = mn.y + 8;
    for (int i = 0; i < mn.n; i++) {
        if (mn.e[i].act == A_SEP) { dl_rect(mn.x + 14, y + 6, mn.x + mn.w - 14, y + 7, 0, C_LINE); y += 12; continue; }
        if (i == mn.hover) dl_rect(mn.x + 6, y, mn.x + mn.w - 6, y + 36, 8, C_BAR);
        uint32_t c = mn.e[i].act == A_DELETE || mn.e[i].act == A_EMPTY ? C_ERR : C_TXT;
        dl_text(mn.x + 20, y + 7, mn.e[i].label, strlen(mn.e[i].label), SF(2), c, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20);
        hitbox(mn.x, y, mn.x + mn.w, y + 36, K_MENU, i);
        y += 36;
    }
}
void fm_draw(void)
{
    hits_begin(&hits);
    find_places();
    dl_rect(0, 0, SW, SH, 0, C_PANEL);                   /* (opaque: the window is ours) */
    SIDE = SW - SIDE_W >= 560 ? SIDE_W : 0;
    draw_bar();
    if (SIDE) draw_side();
    draw_list();
    draw_status();
    draw_menu();
}

/* ---- The pointer. */
void fm_motion(int x, int y)
{
    mx = x; my = y;
    if (mn.on) {
        const hit_t *h = hits_at(&hits, x, y);
        int hv = h && h->kind == K_MENU ? h->arg : -1;
        if (hv != mn.hover) { mn.hover = hv; win_redraw(); }
        return;
    }
    win_redraw();                                        /* (the row under the pointer is lit) */
}
void fm_button(int x, int y, int button, int pressed)
{
    if (!pressed) return;
    const hit_t *h = hits_at(&hits, x, y);
    int kind = h ? h->kind : K_NONE, arg = h ? h->arg : 0;
    if (mn.on) {                                         /* a menu: its entry, or it closes */
        mn.on = 0;
        win_redraw();
        if (kind == K_MENU && arg >= 0 && button == 1) act(mn.e[arg].act);
        return;
    }
    if (ask.on && kind != K_ASK) return;                 /* (a question first) */
    if (fld.on && kind != K_FIELD) field_done(1);        /* (a click elsewhere: the name is taken) */
    switch (kind) {
    case K_BACK: back(); break;
    case K_FWD: forward(); break;
    case K_UP: up(); break;
    case K_NEWDIR: new_folder(); break;
    case K_EMPTY: act(A_EMPTY); break;
    case K_CRUMB: {
        char p[PATH_MAX] = "/";
        int k = 0;
        for (const char *q = cwd + 1; *q && k < arg; ) {
            const char *e = strchr(q, '/');
            int l = e ? (int)(e - q) : (int)strlen(q);
            snprintf(p + strlen(p), sizeof p - strlen(p), "%s%.*s", p[1] ? "/" : "", l, q);
            q += l + (e ? 1 : 0);
            k++;
        }
        go(p, 1);
        break;
    }
    case K_FIELD: if (arg == 1 && !fld.on) field_start(F_LOCATION, -1, cwd); break;
    case K_PLACE: if (button == 1) go(places[arg].path, 1); break;
    case K_SORT:
        if (sort_by == arg) sort_rev = !sort_rev; else { sort_by = arg; sort_rev = 0; }
        load();
        break;
    case K_ROW: {
        uint64_t t = win_ms();
        int ctrl = win_mods() & M_CTRL, shift = win_mods() & M_SHIFT;
        if (button == 3) {
            if (!ent[arg].sel) select_only(arg);
            cur = arg;
            menu_at(x, y, 1);
            break;
        }
        if (button != 1) break;
        if (ctrl) { ent[arg].sel = !ent[arg].sel; cur = anchor = arg; }
        else if (shift && anchor >= 0) { select_range(anchor, arg); cur = arg; }
        else if (arg == last_row && t - last_click < 450) { last_row = -1; select_only(arg); open_sel(); break; }
        else select_only(arg);
        last_row = arg; last_click = t;
        break;
    }
    case K_LIST:
        if (button == 3) { for (int i = 0; i < nent; i++) ent[i].sel = 0; menu_at(x, y, 0); }
        else if (button == 1) { for (int i = 0; i < nent; i++) ent[i].sel = 0; cur = -1; }
        break;
    case K_ASK:
        ask.on = 0;
        if (arg) { if (ask.act == A_DELETE) delete_sel(); else if (ask.act == A_EMPTY) empty_trash(); }
        break;
    }
    win_redraw();
}
void fm_wheel(double steps)
{
    if (mn.on) return;
    scroll += (int)(steps * 3 + (steps > 0 ? 0.5 : -0.5));
    if (scroll < 0) scroll = 0;
    win_redraw();
}

/* ---- Keys. */
static int utf8_len(uint32_t c) { return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }
static int cut_sel(void)                         /* the chosen part out -> 1 if there was one */
{
    if (fld.s0 == fld.s1) return 0;
    int n = (int)strlen(fld.buf);
    memmove(fld.buf + fld.s0, fld.buf + fld.s1, n - fld.s1 + 1);
    fld.curs = fld.s1 = fld.s0;
    return 1;
}
static void field_key(xkb_keysym_t sym, uint32_t ch, int mods)
{
    int n = (int)strlen(fld.buf), had = fld.s0 != fld.s1;
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { field_done(1); return; }
    if (sym == XKB_KEY_Escape) { field_done(0); return; }
    if (sym == XKB_KEY_a && (mods & M_CTRL)) { fld.s0 = 0; fld.curs = fld.s1 = n; win_redraw(); return; }
    if ((sym == XKB_KEY_BackSpace || sym == XKB_KEY_Delete) && cut_sel()) { win_redraw(); return; }
    if (ch >= 32 && ch != 127 && !(mods & M_CTRL)) { cut_sel(); n = (int)strlen(fld.buf); }
    else fld.s0 = fld.s1 = fld.curs;                     /* (moving: nothing chosen) */
    if (had && (sym == XKB_KEY_Left || sym == XKB_KEY_Right)) { win_redraw(); return; }
    if (sym == XKB_KEY_Left) { while (fld.curs > 0 && (fld.buf[--fld.curs] & 0xc0) == 0x80) ; }
    else if (sym == XKB_KEY_Right) { if (fld.curs < n) { fld.curs++; while (fld.curs < n && (fld.buf[fld.curs] & 0xc0) == 0x80) fld.curs++; } }
    else if (sym == XKB_KEY_Home) fld.curs = 0;
    else if (sym == XKB_KEY_End) fld.curs = n;
    else if (sym == XKB_KEY_BackSpace) {
        int a = fld.curs;
        while (a > 0 && (fld.buf[--a] & 0xc0) == 0x80) ;
        if (a < fld.curs) { memmove(fld.buf + a, fld.buf + fld.curs, n - fld.curs + 1); fld.curs = a; }
    } else if (sym == XKB_KEY_Delete) {
        int b = fld.curs;
        if (b < n) { b++; while (b < n && (fld.buf[b] & 0xc0) == 0x80) b++; memmove(fld.buf + fld.curs, fld.buf + b, n - b + 1); }
    } else if (ch >= 32 && ch != 127 && !(mods & M_CTRL)) {   /* a character, UTF-8 */
        char u[4];
        int l = utf8_len(ch);
        if (l == 1) u[0] = (char)ch;
        else if (l == 2) { u[0] = (char)(0xc0 | ch >> 6); u[1] = (char)(0x80 | (ch & 63)); }
        else if (l == 3) { u[0] = (char)(0xe0 | ch >> 12); u[1] = (char)(0x80 | (ch >> 6 & 63)); u[2] = (char)(0x80 | (ch & 63)); }
        else { u[0] = (char)(0xf0 | ch >> 18); u[1] = (char)(0x80 | (ch >> 12 & 63)); u[2] = (char)(0x80 | (ch >> 6 & 63)); u[3] = (char)(0x80 | (ch & 63)); }
        if (n + l < (int)sizeof fld.buf) { memmove(fld.buf + fld.curs + l, fld.buf + fld.curs, n - fld.curs + 1); memcpy(fld.buf + fld.curs, u, l); fld.curs += l; }
    }
    fld.s0 = fld.s1 = fld.curs;
    win_redraw();
}
static void move_cur(int to, int extend)
{
    if (!nent) return;
    if (to < 0) to = 0;
    if (to >= nent) to = nent - 1;
    if (extend && anchor >= 0) { select_range(anchor, to); cur = to; }
    else select_only(to);
    show_cur();
}
void fm_key(xkb_keysym_t sym, uint32_t ch, int mods)
{
    if (sym == XKB_KEY_Shift_L || sym == XKB_KEY_Shift_R || sym == XKB_KEY_Control_L || sym == XKB_KEY_Control_R ||
        sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R || sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R) return;
    if (fld.on) { field_key(sym, ch, mods); return; }
    if (ask.on) {
        if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { ask.on = 0; if (ask.act == A_DELETE) delete_sel(); else empty_trash(); }
        else if (sym == XKB_KEY_Escape) ask.on = 0;
        win_redraw();
        return;
    }
    if (mn.on) {
        int d = sym == XKB_KEY_Down ? 1 : sym == XKB_KEY_Up ? -1 : 0;
        if (d) { int i = mn.hover; for (int k = 0; k < mn.n; k++) { i = (i + d + mn.n) % mn.n; if (mn.e[i].act != A_SEP) break; } mn.hover = i; }
        else if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && mn.hover >= 0) { mn.on = 0; act(mn.e[mn.hover].act); }
        else if (sym == XKB_KEY_Escape) mn.on = 0;
        win_redraw();
        return;
    }
    int ctrl = mods & M_CTRL, shift = mods & M_SHIFT, alt = mods & M_ALT, page = list_rows();
    msg[0] = msg_err ? msg[0] : 0;
    switch (sym) {
    case XKB_KEY_Down: move_cur(cur < 0 ? 0 : cur + 1, shift); break;
    case XKB_KEY_Up: if (alt) up(); else move_cur(cur < 0 ? 0 : cur - 1, shift); break;
    case XKB_KEY_Page_Down: move_cur(cur + page, shift); break;
    case XKB_KEY_Page_Up: move_cur(cur - page, shift); break;
    case XKB_KEY_Home: move_cur(0, shift); break;
    case XKB_KEY_End: move_cur(nent - 1, shift); break;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: open_sel(); break;
    case XKB_KEY_BackSpace: up(); break;
    case XKB_KEY_Left: if (alt) back(); break;
    case XKB_KEY_Right: if (alt) forward(); break;
    case XKB_KEY_F2: rename_cur(); break;
    case XKB_KEY_F5: load(); break;
    case XKB_KEY_Delete: act(shift ? A_DELETE : A_TRASH); break;
    case XKB_KEY_Escape: for (int i = 0; i < nent; i++) ent[i].sel = 0; msg[0] = 0; break;
    case XKB_KEY_Menu: menu_at(SIDE + 80, BAR + HEAD + 40 + (cur - scroll) * ROW, cur >= 0 && nsel()); break;
    default:
        if (ctrl) {
            xkb_keysym_t k = xkb_keysym_to_lower(sym);
            if (k == XKB_KEY_c) act(A_COPY);
            else if (k == XKB_KEY_x) act(A_CUT);
            else if (k == XKB_KEY_v) act(A_PASTE);
            else if (k == XKB_KEY_a) { for (int i = 0; i < nent; i++) ent[i].sel = 1; }
            else if (k == XKB_KEY_h) act(A_HIDDEN);
            else if (k == XKB_KEY_l) field_start(F_LOCATION, -1, cwd);
            else if (k == XKB_KEY_n && shift) new_folder();
            else if (k == XKB_KEY_w || k == XKB_KEY_q) win_quit();
            break;
        }
        if (ch > 32 && ch < 127) {                       /* letters: jump to the first name they start */
            uint64_t t = win_ms();
            size_t l = t - typed_t < 1000 ? strlen(typed) : 0;
            typed_t = t;
            if (l + 1 < sizeof typed) { typed[l] = (char)ch; typed[l + 1] = 0; }
            for (int i = 0; i < nent; i++) if (!strncasecmp(ent[i].name, typed, strlen(typed))) { move_cur(i, 0); break; }
        }
    }
    win_redraw();
}

int main(int argc, char **argv)
{
    const char *h = getenv("HOME");
    snprintf(home, sizeof home, "%s", h && *h ? h : "/");
    const char *data = getenv("XDG_DATA_HOME");
    if (data && *data) snprintf(trash, sizeof trash, "%s/Trash", data);
    else snprintf(trash, sizeof trash, "%s/.local/share/Trash", home);
    signal(SIGPIPE, SIG_IGN);
    ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    win_open("Files", "atlas.files");
    if (ino_fd >= 0) win_fd(ino_fd, changed);
    const char *start = argc > 1 ? argv[1] : home;
    if (!strncmp(start, "file://", 7)) start += 7;
    struct stat st;
    if (stat(start, &st) == 0 && !S_ISDIR(st.st_mode)) {   /* (a file: its folder, it chosen) */
        char d[PATH_MAX];
        snprintf(d, sizeof d, "%s", start);
        char *s = strrchr(d, '/');
        if (s) { *s = 0; go(d[0] ? d : "/", 0); for (int i = 0; i < nent; i++) if (!strcmp(ent[i].name, s + 1)) { select_only(i); show_cur(); } }
        else go(".", 0);
    } else go(start, 0);
    if (!cwd[0]) go(home, 0);
    win_run();
    return 0;
}
