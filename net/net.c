/*
 * net.c - atlas-net: the network, in Atlas's look; NetworkManager's nmcli
 * does the work (README.md, Network). Laid out as Settings: the pages in
 * a sidebar (the chosen one lit), its rows on a card beside it.
 *
 *   Wi-Fi        on or off; the networks in range, the strongest first (a
 *                click chooses one: its passphrase, Connect, Disconnect,
 *                Forget); Scan; a hidden network by its name.
 *   Connections  the profiles NetworkManager keeps: each one's name,
 *                whether it connects by itself, its IPv4 (automatic,
 *                manual: address, gateway; DNS servers) and IPv6;
 *                Save, Connect, Disconnect, Delete.
 *   Devices      each network device: its state, addresses, gateway, DNS;
 *                Connect, Disconnect; networking on or off; the hostname.
 *
 * nmcli runs in processes of its own: questions (what is there) at once,
 * as they answer fast; actions (connecting, scanning) one at a time in the
 * background, the window never waiting; the status bar says how they
 * went, in nmcli's words. "nmcli monitor" tells when something changed,
 * then all is asked again (and every 10 s, for the signals).
 * A passphrase never goes on a command line (where ps shows it): a new
 * network's profile is added without it, then brought up with it in a
 * passwd-file, a memfd. (A known network whose passphrase changed:
 * Forget, then Connect.)
 *
 * Keys: Up/Down choose, Enter connects (in a field: what it is for), Tab
 * the next field, Delete forgets, F5 scans, Ctrl+Page Up/Down the pages,
 * Ctrl+W quit.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include "../files/files.h"

/* ---- Layout, in the picture's units (as Settings'). */
#define ROW 54
#define SBW 200                                  /* the sidebar */
#define STATUS 38
#define NROW 46                                  /* a network's row */

enum { K_NONE, K_PAGE, K_RADIO, K_SCAN, K_AP, K_CONNECT, K_DISCONNECT, K_FORGET, K_FIELD, K_SHOW, K_HIDDEN,
       K_CON, K_AUTO, K_V4, K_V6, K_SAVE, K_UP, K_DOWN, K_DELETE, K_DEVUP, K_DEVDOWN, K_NETWORKING, K_HOST, K_ASK };
enum { P_WIFI, P_CONS, P_DEVS, NPAGE };
static const char *const page_names[NPAGE] = { "Wi-Fi", "Connections", "Devices" };
enum { F_PASS, F_HSSID, F_HPASS, F_NAME, F_ADDR, F_GW, F_DNS, F_HOST, NF };
enum { A_FORGET = 1, A_DELETE, A_NETOFF };

/* ---- What NetworkManager has, as last asked. */
#define MAXAP 64
#define MAXCON 64
#define MAXDEV 16
typedef struct { char ssid[96], sec[48]; int signal, inuse; } ap_t;
typedef struct { char name[128], uuid[40], type[40], dev[32]; int active; } con_t;
typedef struct { char name[32], type[24], state[48], con[128], hw[24], ip4[160], gw4[48], dns[160], ip6[200]; } netdev_t;
static ap_t aps[MAXAP];
static con_t cons[MAXCON];
static netdev_t devs[MAXDEV];
static int naps, ncons, ndevs;
static int nm_ok = 1, nmcli_ok = 1, wifi_on, wifi_hw = 1, networking = 1;
static char nm_state[48], connectivity[32], hostname[128], wifi_dev[32];
static struct { char uuid[40], ssid[96]; } ssids[MAXCON];   /* each Wi-Fi profile's network (asked once) */
static int nssids;

/* ---- The window's state. */
static int page = P_WIFI, scroll[NPAGE], content_h, focus = -1, show_pass, hidden_open, scanning;
static char fbuf[NF][256];
static int fcur[NF];
static char sel_ssid[96], sel_con[40];                   /* the network, the profile chosen (by name, by uuid) */
static struct { char name[128]; int autoc, v4, v6; char addr[256], gw[64], dns[256]; int ip; } orig, ed;   /* the profile as read, as edited */
static struct { int on, act; char text[200], arg[128]; } ask;
static char msg[240];
static int msg_err;
static hits_t hits;
static int mx = -1000000, my = -1000000;

static const char *const v4_names[] = { "Automatic", "Manual", "Shared", "Off" };
static const char *const v4_vals[] = { "auto", "manual", "shared", "disabled" };
static const char *const v6_names[] = { "Automatic", "Link-local", "Off" };
static const char *const v6_vals[] = { "auto", "link-local", "disabled" };

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
static void copy(char *d, size_t cap, const char *s) { snprintf(d, cap, "%s", s ? s : ""); }
#define COPY(d, s) copy(d, sizeof d, s)

/* ---- nmcli. Its terse output: fields split at ':', "\:" and "\\" within. */
static int split(char *line, char **f, int max)
{
    int n = 0;
    char *w = line;
    f[n++] = w;
    for (char *r = line; *r && *r != '\n'; r++) {
        if (*r == '\\' && r[1]) { *w++ = *++r; continue; }
        if (*r == ':' && n < max) { *w++ = 0; f[n++] = w; continue; }
        *w++ = *r;
    }
    *w = 0;
    for (int i = n; i < max; i++) f[i] = w;              /* (missing ones: empty) */
    return n;
}
/* The child's side: C.UTF-8 (nmcli's words untranslated, names as they are),
 * its output and errors to fd out, a secret as fd 3. */
static void child(int out, int secret, char *const argv[])
{
    dup2(out, 1); dup2(out, 2);
    if (secret >= 0) { dup2(secret, 3); fcntl(3, F_SETFD, 0); }   /* (after: out may have been 3) */
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0) dup2(null, 0);
    setenv("LC_ALL", "C.UTF-8", 1);
    execvp(argv[0], argv);
    dprintf(2, "Error: cannot run %s: %s\n", argv[0], strerror(errno));
    _exit(127);
}
/* A question: nmcli's answer in out -> 0 if it said yes. */
static int ask_nm(char *out, size_t cap, ...)
{
    char *argv[32];
    int n = 0;
    va_list a;
    va_start(a, cap);
    argv[n++] = "nmcli";
    for (char *s; n < 31 && (s = va_arg(a, char *)); ) argv[n++] = s;
    va_end(a);
    argv[n] = 0;
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) child(p[1], -1, argv);
    close(p[1]);
    size_t k = 0;
    ssize_t r;
    while ((r = read(p[0], out + k, cap - 1 - k)) > 0 || (r < 0 && errno == EINTR)) if (r > 0) k += r;
    out[k] = 0;
    close(p[0]);
    int st;
    if (pid < 0 || waitpid(pid, &st, 0) < 0) return -1;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) nmcli_ok = 0;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}
/* nmcli's last words (an error: without "Error: "). */
static const char *last_line(const char *out)
{
    static char l[200];
    l[0] = 0;
    for (const char *p = out; *p; ) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n) snprintf(l, sizeof l, "%.*s", (int)n, p);
        p += n + (e ? 1 : 0);
    }
    char *s = l;
    if (!strncmp(s, "Error: ", 7)) s += 7;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '.' || s[n - 1] == ' ')) s[--n] = 0;
    return s;
}

/* An action: one at a time, in the background; done(ok, output) after. */
static struct { pid_t pid; int fd; char out[8192]; size_t n; void (*done)(int, const char *); char what[160]; } job = { 0, -1 };
static void refresh(void);
static void job_ready(void)
{
    ssize_t r = read(job.fd, job.out + job.n, sizeof job.out - 1 - job.n);
    if (r > 0) { job.n += r; if (job.n < sizeof job.out - 1) return; }
    else if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
    close(job.fd);
    job.fd = -1;
    win_fd(-1, job_ready);
    job.out[job.n] = 0;
    int st = 0;
    waitpid(job.pid, &st, 0);
    job.pid = 0;
    void (*done)(int, const char *) = job.done;
    char out[sizeof job.out];
    memcpy(out, job.out, job.n + 1);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) nmcli_ok = 0;
    if (done) done(ok, out);
    else if (ok) say_msg(0, "Done");
    else say_msg(1, "%s", last_line(out));
    refresh();
}
static int busy(void) { if (job.pid) say_msg(1, "Wait: %s", job.what); return job.pid != 0; }
static int run_argv(const char *what, void (*done)(int, const char *), int secret, char *const *args)   /* nmcli's args, 0 */
{
    if (job.pid) { say_msg(1, "Wait: %s", job.what); if (secret >= 0) close(secret); return -1; }
    char *argv[40];
    int n = 0;
    argv[n++] = "nmcli";
    while (n < 39 && *args) argv[n++] = *args++;
    argv[n] = 0;
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) { if (secret >= 0) close(secret); return -1; }
    pid_t pid = fork();
    if (pid == 0) child(p[1], secret, argv);
    close(p[1]);
    if (secret >= 0) close(secret);
    if (pid < 0) { close(p[0]); say_msg(1, "Cannot run nmcli"); return -1; }
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    job.pid = pid; job.fd = p[0]; job.n = 0; job.done = done;
    COPY(job.what, what);
    win_fd(job.fd, job_ready);
    say_msg(0, "%s...", what);
    return 0;
}
static int run(const char *what, void (*done)(int, const char *), int secret, ...)   /* nmcli's args..., 0 */
{
    char *args[40];
    int n = 0;
    va_list a;
    va_start(a, secret);
    for (char *s; n < 39 && (s = va_arg(a, char *)); ) args[n++] = s;
    va_end(a);
    args[n] = 0;
    return run_argv(what, done, secret, args);
}
/* A secret for passwd-file (nmcli reads it as /dev/fd/3): only in memory. */
static int secret_fd(const char *key, const char *value)
{
    int fd = memfd_create("atlas-net", MFD_CLOEXEC);
    if (fd < 0) return -1;
    dprintf(fd, "%s:%s\n", key, value);
    lseek(fd, 0, SEEK_SET);
    return fd;
}

/* "nmcli monitor": a line whenever something changed; all asked again soon after. */
static int mon_fd = -1;
static void monitor_ready(void)
{
    char b[4096];
    ssize_t r = read(mon_fd, b, sizeof b);
    if (r > 0) { win_timer(300, refresh); return; }
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
    close(mon_fd);                                       /* (it ended: the timer goes on) */
    mon_fd = -1;
    win_fd(-1, monitor_ready);
}
static void monitor_start(void)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);               /* (gone with the window) */
        char *argv[] = { "nmcli", "monitor", 0 };
        child(p[1], -1, argv);
    }
    close(p[1]);
    if (pid < 0) { close(p[0]); return; }
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    mon_fd = p[0];
    win_fd(mon_fd, monitor_ready);
}

/* ---- Asking what there is. */
static char big[65536];
static void read_aps(const char *out)                     /* IN-USE:SIGNAL:SECURITY:SSID, one network once */
{
    char line[512];
    naps = 0;
    for (const char *p = out; *p; ) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), p);
        p += n + (e ? 1 : 0);
        char *f[4];
        if (split(line, f, 4) < 4 || !f[3][0]) continue;   /* (hidden ones: by their name, below) */
        int inuse = f[0][0] == '*', sig = atoi(f[1]), k = 0;
        while (k < naps && strcmp(aps[k].ssid, f[3])) k++;
        if (k < naps) {                                  /* (another access point of it) */
            if (sig > aps[k].signal) aps[k].signal = sig;
            aps[k].inuse |= inuse;
            continue;
        }
        if (naps == MAXAP) continue;
        ap_t *a = &aps[naps++];
        COPY(a->ssid, f[3]); COPY(a->sec, f[2]);
        a->signal = sig; a->inuse = inuse;
    }
    for (int i = 1; i < naps; i++)                       /* the one in use first, then the strongest */
        for (int j = i; j > 0; j--) {
            ap_t *x = &aps[j - 1], *y = &aps[j];
            if (y->inuse > x->inuse || (y->inuse == x->inuse && y->signal > x->signal)) { ap_t t = *x; *x = *y; *y = t; }
            else break;
        }
}
static const char *ssid_of(const char *uuid)             /* a Wi-Fi profile's network */
{
    for (int i = 0; i < nssids; i++) if (!strcmp(ssids[i].uuid, uuid)) return ssids[i].ssid;
    char out[512], *f[1];
    if (ask_nm(out, sizeof out, "-t", "-g", "802-11-wireless.ssid", "connection", "show", uuid, (char *)0) < 0) out[0] = 0;   /* (kept too: not asked again) */
    split(out, f, 1);
    if (nssids == MAXCON) nssids = 0;
    COPY(ssids[nssids].uuid, uuid); COPY(ssids[nssids].ssid, f[0]);
    return ssids[nssids++].ssid;
}
static const char *saved(const char *ssid)               /* the profile of a network, or 0 */
{
    for (int i = 0; i < ncons; i++)                      /* (the active one first) */
        if (cons[i].active && !strcmp(cons[i].type, "802-11-wireless") && !strcmp(ssid_of(cons[i].uuid), ssid)) return cons[i].uuid;
    for (int i = 0; i < ncons; i++)
        if (!strcmp(cons[i].type, "802-11-wireless") && !strcmp(ssid_of(cons[i].uuid), ssid)) return cons[i].uuid;
    return 0;
}
static con_t *con_by(const char *uuid) { for (int i = 0; i < ncons; i++) if (!strcmp(cons[i].uuid, uuid)) return &cons[i]; return 0; }
static netdev_t *dev_by(const char *name) { for (int i = 0; i < ndevs; i++) if (!strcmp(devs[i].name, name)) return &devs[i]; return 0; }

static void load_con(void);
static void refresh(void)
{
    char *f[8], line[1024];
    win_timer(10000, refresh);
    /* the whole: STATE:CONNECTIVITY:NETWORKING:WIFI-HW:WIFI */
    nm_ok = ask_nm(big, sizeof big, "-t", "-f", "STATE,CONNECTIVITY,NETWORKING,WIFI-HW,WIFI", "general", (char *)0) == 0;
    if (!nm_ok) { naps = ncons = ndevs = 0; win_redraw(); return; }
    COPY(line, big);
    split(line, f, 5);
    COPY(nm_state, f[0]); COPY(connectivity, f[1]);
    networking = !strcmp(f[2], "enabled"); wifi_hw = !strcmp(f[3], "enabled"); wifi_on = !strcmp(f[4], "enabled");
    if (ask_nm(big, sizeof big, "general", "hostname", (char *)0) == 0) { big[strcspn(big, "\n")] = 0; COPY(hostname, big); }
    /* the devices: DEVICE:TYPE:STATE:CONNECTION, then each one's addresses */
    ndevs = 0;
    wifi_dev[0] = 0;
    if (ask_nm(big, sizeof big, "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status", (char *)0) == 0)
        for (char *p = big, *e; *p && ndevs < MAXDEV; p = e + 1) {
            e = strchr(p, '\n');
            if (!e) e = p + strlen(p) - 1;
            snprintf(line, sizeof line, "%.*s", (int)(e - p + 1), p);
            split(line, f, 4);
            if (!strcmp(f[1], "loopback") || !strcmp(f[1], "wifi-p2p") || !f[0][0]) { if (!e[1]) break; continue; }
            netdev_t *d = &devs[ndevs++];
            memset(d, 0, sizeof *d);
            COPY(d->name, f[0]); COPY(d->type, f[1]); COPY(d->state, f[2]); COPY(d->con, f[3]);
            if (!wifi_dev[0] && !strcmp(d->type, "wifi")) COPY(wifi_dev, d->name);
            if (!e[1]) break;
        }
    if (ask_nm(big, sizeof big, "-t", "-f", "GENERAL.DEVICE,GENERAL.HWADDR,IP4.ADDRESS,IP4.GATEWAY,IP4.DNS,IP6.ADDRESS", "device", "show", (char *)0) == 0) {
        netdev_t *d = 0;
        for (char *p = big, *e; *p; p = e + 1) {         /* (key:value, the value as it is) */
            e = strchr(p, '\n');
            if (!e) e = p + strlen(p);
            snprintf(line, sizeof line, "%.*s", (int)(e - p), p);
            char *v = strchr(line, ':');
            if (v) {
                *v++ = 0;
                char *list = 0;
                size_t cap = 0;
                if (!strcmp(line, "GENERAL.DEVICE")) d = dev_by(v);
                else if (d && !strcmp(line, "GENERAL.HWADDR")) COPY(d->hw, v);
                else if (d && !strcmp(line, "IP4.GATEWAY")) COPY(d->gw4, v);
                else if (d && !strncmp(line, "IP4.ADDRESS", 11)) { list = d->ip4; cap = sizeof d->ip4; }
                else if (d && !strncmp(line, "IP4.DNS", 7)) { list = d->dns; cap = sizeof d->dns; }
                else if (d && !strncmp(line, "IP6.ADDRESS", 11)) { list = d->ip6; cap = sizeof d->ip6; }
                if (list && *v) snprintf(list + strlen(list), cap - strlen(list), "%s%s", *list ? ", " : "", v);
            }
            if (!*e) break;
        }
    }
    /* the profiles: NAME:UUID:TYPE:DEVICE:ACTIVE */
    ncons = 0;
    if (ask_nm(big, sizeof big, "-t", "-f", "NAME,UUID,TYPE,DEVICE,ACTIVE", "connection", "show", (char *)0) == 0)
        for (char *p = big, *e; *p && ncons < MAXCON; p = e + 1) {
            e = strchr(p, '\n');
            if (!e) e = p + strlen(p) - 1;
            snprintf(line, sizeof line, "%.*s", (int)(e - p + 1), p);
            split(line, f, 5);
            if (f[1][0] && strcmp(f[2], "loopback")) {
                con_t *c = &cons[ncons++];
                COPY(c->name, f[0]); COPY(c->uuid, f[1]); COPY(c->type, f[2]); COPY(c->dev, f[3]);
                c->active = !strcmp(f[4], "yes");
            }
            if (!e[1]) break;
        }
    /* the networks in range (as last scanned: asking does not scan) */
    naps = 0;
    if (wifi_dev[0] && wifi_on && !scanning &&
        ask_nm(big, sizeof big, "-t", "-f", "IN-USE,SIGNAL,SECURITY,SSID", "device", "wifi", "list", "--rescan", "no", (char *)0) == 0)
        read_aps(big);
    for (int i = 0; i < ncons; i++) if (!strcmp(cons[i].type, "802-11-wireless")) ssid_of(cons[i].uuid);   /* (asked here, not while drawing) */
    if (sel_con[0] && !con_by(sel_con)) sel_con[0] = 0;  /* (deleted) */
    win_redraw();
}

/* ---- The profile chosen: read into the fields (once chosen, not at each refresh). */
static int idx_of(const char *const *vals, int n, const char *v) { for (int i = 0; i < n; i++) if (!strcmp(vals[i], v)) return i; return -1; }
static int has_ip(const char *type)
{
    static const char *const t[] = { "802-3-ethernet", "802-11-wireless", "bond", "bridge", "vlan", "team", "wireguard", "vpn", "tun", "gsm", "cdma", 0 };
    for (int i = 0; t[i]; i++) if (!strcmp(type, t[i])) return 1;
    return 0;
}
static void load_con(void)
{
    con_t *c = con_by(sel_con);
    memset(&orig, 0, sizeof orig);
    if (!c) return;
    char line[1024];
    if (ask_nm(big, sizeof big, "-t", "-f", "connection.id,connection.autoconnect,ipv4.method,ipv4.addresses,ipv4.gateway,ipv4.dns,ipv6.method",
               "connection", "show", sel_con, (char *)0) == 0)
        for (char *p = big, *e; *p; p = e + 1) {
            e = strchr(p, '\n');
            if (!e) e = p + strlen(p);
            snprintf(line, sizeof line, "%.*s", (int)(e - p), p);
            char *v = strchr(line, ':');
            if (v) {
                *v++ = 0;
                if (!strcmp(line, "connection.id")) COPY(orig.name, v);
                else if (!strcmp(line, "connection.autoconnect")) orig.autoc = !strcmp(v, "yes");
                else if (!strcmp(line, "ipv4.method")) orig.v4 = idx_of(v4_vals, 4, v);
                else if (!strcmp(line, "ipv4.addresses")) COPY(orig.addr, v);
                else if (!strcmp(line, "ipv4.gateway")) COPY(orig.gw, v);
                else if (!strcmp(line, "ipv4.dns")) COPY(orig.dns, v);
                else if (!strcmp(line, "ipv6.method")) orig.v6 = idx_of(v6_vals, 3, v);
            }
            if (!*e) break;
        }
    orig.ip = has_ip(c->type);
    ed = orig;
    COPY(fbuf[F_NAME], ed.name); COPY(fbuf[F_ADDR], ed.addr); COPY(fbuf[F_GW], ed.gw); COPY(fbuf[F_DNS], ed.dns);
    for (int f = F_NAME; f <= F_DNS; f++) fcur[f] = (int)strlen(fbuf[f]);
}

/* ---- The actions. */
static void wipe(int f) { explicit_bzero(fbuf[f], sizeof fbuf[f]); fcur[f] = 0; }
static ap_t *sel_ap(void) { for (int i = 0; i < naps; i++) if (!strcmp(aps[i].ssid, sel_ssid)) return &aps[i]; return 0; }
static int needs_pass(const char *sec) { return sec[0] && strcmp(sec, "--") && !strstr(sec, "OWE"); }

static char pending_ssid[96], pending_uuid[40];
static int pending_hidden;
static void up_done(int ok, const char *out)             /* a new network: in, or its profile taken away again */
{
    if (ok) { say_msg(0, "Connected to %s", pending_ssid); wipe(F_PASS); wipe(F_HPASS); hidden_open = 0; return; }
    char o[64];
    ask_nm(o, sizeof o, "connection", "delete", pending_uuid, (char *)0);
    say_msg(1, "Could not connect to %s: %s", pending_ssid, last_line(out));
}
static void add_done(int ok, const char *out)
{
    const char *a = strrchr(out, '('), *b = a ? strchr(a, ')') : 0;   /* "Connection 'x' (UUID) successfully added." */
    if (!ok || !a || !b || b - a > 40) { say_msg(1, "Could not add %s: %s", pending_ssid, last_line(out)); return; }
    snprintf(pending_uuid, sizeof pending_uuid, "%.*s", (int)(b - a - 1), a + 1);
    const char *pass = pending_hidden ? fbuf[F_HPASS] : fbuf[F_PASS];
    int fd = pass[0] ? secret_fd("802-11-wireless-security.psk", pass) : -1;
    char what[160];
    snprintf(what, sizeof what, "Connecting to %s", pending_ssid);
    if (fd >= 0) run(what, up_done, fd, "--wait", "45", "connection", "up", pending_uuid, "passwd-file", "/dev/fd/3", (char *)0);
    else run(what, up_done, -1, "--wait", "45", "connection", "up", pending_uuid, (char *)0);
}
/* A network not known: its profile added (no secret in it), then brought up with the secret. */
static void join_new(const char *ssid, const char *sec, const char *pass, int hidden)
{
    if (busy()) return;
    const char *km = 0;
    if (strstr(sec, "802.1X")) { say_msg(1, "%s needs an enterprise sign-in: not done here", ssid); return; }
    if (pass[0]) km = strstr(sec, "WPA3") && !strstr(sec, "WPA2") && !strstr(sec, "WPA1") ? "sae" : "wpa-psk";
    if (strstr(sec, "WEP")) { say_msg(1, "%s uses WEP, which is not safe and not done here", ssid); return; }
    if (needs_pass(sec) && !pass[0]) { focus = hidden ? F_HPASS : F_PASS; say_msg(1, "Type %s's passphrase", ssid); return; }
    if (km && strlen(pass) < 8) { say_msg(1, "A passphrase has at least 8 characters"); return; }
    COPY(pending_ssid, ssid);
    pending_hidden = hidden;
    char what[160];
    snprintf(what, sizeof what, "Adding %s", ssid);
    if (km) run(what, add_done, -1, "connection", "add", "type", "wifi", "con-name", ssid, "ssid", ssid,
                "802-11-wireless.hidden", hidden ? "yes" : "no", "wifi-sec.key-mgmt", km, (char *)0);
    else if (strstr(sec, "OWE")) run(what, add_done, -1, "connection", "add", "type", "wifi", "con-name", ssid, "ssid", ssid, "wifi-sec.key-mgmt", "owe", (char *)0);
    else run(what, add_done, -1, "connection", "add", "type", "wifi", "con-name", ssid, "ssid", ssid, "802-11-wireless.hidden", hidden ? "yes" : "no", (char *)0);
}
static void connect_done(int ok, const char *out)
{
    if (ok) say_msg(0, "Connected to %s", pending_ssid);
    else if (strstr(out, "ecrets")) say_msg(1, "Could not connect to %s: its passphrase changed? Forget it, then Connect", pending_ssid);
    else say_msg(1, "Could not connect to %s: %s", pending_ssid, last_line(out));
}
static void connect_ap(void)
{
    ap_t *a = sel_ap();
    if (!a) { say_msg(1, "Choose a network first"); return; }
    const char *uuid = saved(a->ssid);
    if (!uuid) { join_new(a->ssid, a->sec, fbuf[F_PASS], 0); return; }
    char what[160];
    COPY(pending_ssid, a->ssid);
    snprintf(what, sizeof what, "Connecting to %s", a->ssid);
    run(what, connect_done, -1, "--wait", "45", "connection", "up", uuid, (char *)0);
}
static void disconnect_ap(void)
{
    ap_t *a = sel_ap();
    const char *uuid = a ? saved(a->ssid) : 0;
    if (uuid) run("Disconnecting", 0, -1, "connection", "down", uuid, (char *)0);
}
static void scan_done(int ok, const char *out)
{
    scanning = 0;
    if (!ok) { say_msg(1, "Could not scan: %s", last_line(out)); return; }
    read_aps(out);
    say_msg(0, "%d network%s in range", naps, naps == 1 ? "" : "s");
}
static void scan(void)
{
    if (!wifi_dev[0] || !wifi_on || busy()) return;
    if (run("Scanning", scan_done, -1, "-t", "-f", "IN-USE,SIGNAL,SECURITY,SSID", "device", "wifi", "list", "--rescan", "yes", (char *)0) == 0) scanning = 1;
}
static void save_done(int ok, const char *out)
{
    if (!ok) { say_msg(1, "Not saved: %s", last_line(out)); return; }
    con_t *c = con_by(sel_con);
    if (c && c->active) {                                /* (in use: brought up again, so it takes effect) */
        char what[160];
        snprintf(what, sizeof what, "Saved; reconnecting %s", c->name);
        run(what, 0, -1, "connection", "up", sel_con, (char *)0);
        return;
    }
    say_msg(0, "Saved");
}
/* The profile's changes (only those), in one "nmcli connection modify". */
static void save_con(void)
{
    if (!sel_con[0] || busy()) return;
    char *argv[24] = { "connection", "modify", sel_con };
    int n = 3;
    COPY(ed.name, fbuf[F_NAME]); COPY(ed.addr, fbuf[F_ADDR]); COPY(ed.gw, fbuf[F_GW]); COPY(ed.dns, fbuf[F_DNS]);
    if (!ed.name[0]) { say_msg(1, "A connection needs a name"); return; }
    if (ed.ip && ed.v4 == 1 && !ed.addr[0]) { focus = F_ADDR; say_msg(1, "Manual: type an address, such as 192.168.1.20/24"); return; }
    if (strcmp(ed.name, orig.name)) { argv[n++] = "connection.id"; argv[n++] = ed.name; }
    if (ed.autoc != orig.autoc) { argv[n++] = "connection.autoconnect"; argv[n++] = ed.autoc ? "yes" : "no"; }
    if (ed.ip) {
        if (ed.v4 >= 0 && ed.v4 != orig.v4) { argv[n++] = "ipv4.method"; argv[n++] = (char *)v4_vals[ed.v4]; }
        int manual = ed.v4 == 1;
        if (strcmp(ed.addr, orig.addr) || (!manual && orig.addr[0])) { argv[n++] = "ipv4.addresses"; argv[n++] = manual ? ed.addr : ""; }
        if (strcmp(ed.gw, orig.gw) || (!manual && orig.gw[0])) { argv[n++] = "ipv4.gateway"; argv[n++] = manual ? ed.gw : ""; }
        if (strcmp(ed.dns, orig.dns)) { argv[n++] = "ipv4.dns"; argv[n++] = ed.dns; }
        if (ed.v6 >= 0 && ed.v6 != orig.v6) { argv[n++] = "ipv6.method"; argv[n++] = (char *)v6_vals[ed.v6]; }
    }
    if (n == 3) { say_msg(0, "Nothing changed"); return; }
    argv[n] = 0;
    run_argv("Saving", save_done, -1, argv);
}
static void forget_done(int ok, const char *out) { if (ok) say_msg(0, "Forgotten"); else say_msg(1, "%s", last_line(out)); }
static void ask_for(int act, const char *arg, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void ask_for(int act, const char *arg, const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vsnprintf(ask.text, sizeof ask.text, fmt, a);
    va_end(a);
    COPY(ask.arg, arg);
    ask.on = 1; ask.act = act;
    win_redraw();
}
static void answer(int yes)
{
    ask.on = 0;
    win_redraw();
    if (!yes) return;
    if (ask.act == A_FORGET || ask.act == A_DELETE) run(ask.act == A_FORGET ? "Forgetting" : "Deleting", forget_done, -1, "connection", "delete", ask.arg, (char *)0);
    else if (ask.act == A_NETOFF) run("Turning networking off", 0, -1, "networking", "off", (char *)0);
}
static void forget_ap(void)
{
    ap_t *a = sel_ap();
    const char *uuid = a ? saved(a->ssid) : 0;
    if (uuid) ask_for(A_FORGET, uuid, "Forget %s? Its passphrase goes too.", a->ssid);
}
static void field_enter(int f)
{
    if (f == F_PASS) connect_ap();
    else if (f == F_HSSID || f == F_HPASS) {
        if (!fbuf[F_HSSID][0]) { focus = F_HSSID; say_msg(1, "Type the network's name"); return; }
        join_new(fbuf[F_HSSID], fbuf[F_HPASS][0] ? "WPA2" : "", fbuf[F_HPASS], 1);
    } else if (f == F_HOST) {
        if (fbuf[F_HOST][0] && !busy()) run("Setting the hostname", 0, -1, "general", "hostname", fbuf[F_HOST], (char *)0);
    } else save_con();
}

/* ---- Drawing, as Settings draws (src/settings.c): rows on a card. */
static int X0, Y0, X1, Y1, cy, lx, cx, dry;
static int fits(int h) { return !dry && cy >= Y0 + 4 && cy + h <= Y1 - 6; }
static void txt(int x, int y, const char *s, int k, uint32_t c, int x1)   /* (UTF-8: shown in ASCII) */
{
    char a[512];
    ascii(s, a, sizeof a);
    dl_text(x, y, a, strlen(a), k, c, X0, Y0, x1, Y1);
}
static void title(int x, const char *s)
{
    dl_text(x, cy, s, strlen(s), STS, C_TXT, X0, Y0, X1, Y1);
    dl_text(x + 1, cy, s, strlen(s), STS, C_TXT, X0, Y0, X1, Y1);
}
static void group(const char *s)
{
    cy += 6;
    if (fits(26)) dl_text(lx, cy, s, strlen(s), SSS, C_DIM, X0, Y0, X1, Y1);
    cy += 30;
}
static void row_label(const char *s)
{
    if (cx == lx) { if (fits(30)) dl_text(lx, cy, s, strlen(s), SF(2), C_DIM, X0, Y0, X1, Y1); cy += 34; }
    else if (fits(42)) dl_text(lx, cy + 10, s, strlen(s), SF(2), C_DIM, X0, Y0, cx - 12, Y1);
}
static void line_text(const char *name, const char *v)   /* a label, a value (mono: addresses) */
{
    if (!v || !v[0]) return;
    row_label(name);
    if (fits(30)) txt(cx, cy + (cx == lx ? 0 : 10), v, SS, C_TXT, X1 - 16);
    cy += cx == lx ? 34 : 42;
}
static int pill(int x, int y, const char *s, int on, int kind, int arg)   /* -> its right edge */
{
    int w = tws(s, 2) + 28;
    if (!dry && y >= Y0 + 4 && y + 42 <= Y1 - 6) {
        dl_rect(x, y, x + w, y + 42, 21, on ? C_ACC : C_FIELD);
        dl_frame(x, y, x + w, y + 42, 21, 1, on ? C_ACC : C_LINE);
        dl_text(x + 14, y + 10, s, strlen(s), SF(2), on ? C_INK : C_TXT, X0, Y0, X1, Y1);
        hitbox(x, y, x + w, y + 42, kind, arg);
    }
    return x + w;
}
static int pills_x;                                      /* a row of buttons, wrapped when narrow */
static void pills_begin(void) { pills_x = cx; }
static void pills(const char *s, int on, int kind, int arg)
{
    int w = tws(s, 2) + 28;
    if (pills_x > cx && pills_x + w > X1 - 24) { pills_x = cx; cy += ROW; }
    pills_x = pill(pills_x, cy, s, on, kind, arg) + 10;
}
static void pills_end(void) { cy += ROW; }
#define MAXCH 6
static void choices(const char *name, const char *const *s, int n, int on, int kind)   /* a segmented control */
{
    row_label(name);
    int x = cx, y = cy, ws[MAXCH];
    for (int i = 0; i < n; i++) ws[i] = tws(s[i], 2) + 28;
    if (fits(42)) {
        int x1 = x;
        for (int i = 0; i < n; i++) x1 += ws[i];
        dl_rect(x, y, x1, y + 42, 21, C_FIELD);
        for (int i = 0; i < n; i++) {
            int w = ws[i];
            if (i == on) {
                dl_rect(x, y, x + w, y + 42, 21, C_ACC);
                if (n > 1 && i == 0) dl_rect(x + 21, y, x + w, y + 42, 0, C_ACC);
                else if (n > 1 && i == n - 1) dl_rect(x, y, x + w - 21, y + 42, 0, C_ACC);
                else if (n > 1) dl_rect(x, y, x + w, y + 42, 0, C_ACC);
            } else if (i > 0 && i - 1 != on) dl_rect(x, y + 9, x + 1, y + 33, 0, C_LINE);
            dl_text(x + 14, y + 10, s[i], strlen(s[i]), SF(2), i == on ? C_INK : C_TXT, X0, Y0, X1, Y1);
            hitbox(x, y, x + w, y + 42, kind, i);
            x += w;
        }
        dl_frame(cx, y, x1, y + 42, 21, 1, C_LINE);
    }
    cy += ROW;
}
static void field(const char *name, int f, int secret, const char *hint)
{
    row_label(name);
    int x1 = X1 - 24;
    if (secret) x1 -= tws(show_pass ? "Hide" : "Show", 2) + 28 + 10;
    if (fits(44)) {
        char a[300];
        if (secret && !show_pass) { int n = 0; for (const char *p = fbuf[f]; *p && n < 255; p++) if ((*p & 0xc0) != 0x80) a[n++] = '*'; a[n] = 0; }
        else ascii(fbuf[f], a, sizeof a);
        int on = focus == f, n = (int)strlen(a), from = 0, cc = 0;
        for (int i = 0; i < fcur[f]; i++) cc += (fbuf[f][i] & 0xc0) != 0x80;   /* (the cursor's column) */
        if (cc > n) cc = n;
        while (from < cc && text_sw(a + from, cc - from, SF(2)) > x1 - cx - 32) from++;
        dl_rect(cx, cy, x1, cy + 44, 10, C_FIELD);
        dl_frame(cx, cy, x1, cy + 44, 10, on ? 2 : 1, on ? C_ACC : C_LINE);
        if (n) dl_text(cx + 14, cy + 11, a + from, n - from, SF(2), C_TXT, cx, cy, x1 - 10, cy + 44);
        else if (hint && !on) dl_text(cx + 14, cy + 11, hint, strlen(hint), SF(2), C_DIM, cx, cy, x1 - 10, cy + 44);
        if (on) { int w = text_sw(a + from, cc - from, SF(2)); dl_rect(cx + 14 + w, cy + 10, cx + 16 + w, cy + 34, 0, C_ACC); }
        hitbox(cx, cy, x1, cy + 44, K_FIELD, f);
        if (secret) pill(x1 + 10, cy + 1, show_pass ? "Hide" : "Show", 0, K_SHOW, 0);
    }
    cy += ROW;
}
static void note(const char *s, uint32_t c)              /* a line of text, across the card */
{
    if (fits(30)) txt(lx, cy, s, SF(2), c, X1 - 16);
    cy += 36;
}
static void bars(int x, int y, int signal, uint32_t on, uint32_t off)   /* four bars, as many lit as the signal */
{
    int lit = (signal + 12) / 25;
    for (int i = 0; i < 4; i++) dl_rect(x + i * 7, y + 18 - i * 5, x + i * 7 + 5, y + 24, 1, i < lit ? on : off);
}

static void page_wifi(void)
{
    if (!wifi_dev[0]) { note("No Wi-Fi on this machine.", C_DIM); return; }
    static const char *const onoff[2] = { "Off", "On" };
    choices("Wi-Fi", onoff, 2, wifi_on, K_RADIO);
    if (!wifi_hw) { note("Wi-Fi is turned off by a switch or key on the machine.", C_ERR); return; }
    if (!wifi_on) return;
    netdev_t *d = dev_by(wifi_dev);
    char t[300];
    if (d && !strncmp(d->state, "connected", 9) && d->con[0]) {
        snprintf(t, sizeof t, "Connected to %s", d->con);
        row_label("Now");
        if (fits(42)) txt(cx, cy + (cx == lx ? 0 : 10), t, SF(2), C_TXT, X1 - 16);
        cy += cx == lx ? 34 : ROW;
        line_text("Address", d->ip4);
    } else {
        row_label("Now");
        if (fits(42)) txt(cx, cy + (cx == lx ? 0 : 10), d && d->state[0] ? d->state : "Not connected", SF(2), C_DIM, X1 - 16);
        cy += cx == lx ? 34 : ROW;
    }
    pills_begin();
    pills(scanning ? "Scanning..." : "Scan", scanning, K_SCAN, 0);
    pills(hidden_open ? "Hidden network: close" : "Hidden network...", hidden_open, K_HIDDEN, 0);
    pills_end();
    if (hidden_open) {
        field("Network name", F_HSSID, 0, "its SSID, as it is written");
        field("Passphrase", F_HPASS, 1, "none for an open network");
        pills_begin(); pills("Connect", 1, K_CONNECT, 1); pills_end();
    }
    group(naps ? "NETWORKS IN RANGE" : scanning ? "Looking for networks..." : "No networks seen: Scan");
    for (int i = 0; i < naps; i++) {
        ap_t *a = &aps[i];
        int on = !strcmp(a->ssid, sel_ssid);
        const char *uuid = saved(a->ssid);
        if (fits(NROW)) {
            int hov = mx >= lx - 10 && mx < X1 - 14 && my >= cy && my < cy + NROW - 4 && !ask.on;
            if (on) dl_rect(lx - 10, cy, X1 - 14, cy + NROW - 4, 8, C_SEL);
            else if (hov) dl_rect(lx - 10, cy, X1 - 14, cy + NROW - 4, 8, C_BAR);
            bars(lx, cy + 7, a->signal, a->inuse ? C_ACC : C_TXT, C_LINE);
            int rx = X1 - 30;
            const char *sec = !needs_pass(a->sec) ? "open" : a->sec;
            rx -= text_sw(sec, strlen(sec), SSS);
            dl_text(rx, cy + 11, sec, strlen(sec), SSS, C_DIM, X0, Y0, X1, Y1);
            const char *st = a->inuse ? "Connected" : uuid ? "Saved" : "";
            if (*st) { rx -= text_sw(st, strlen(st), SSS) + 24; dl_text(rx, cy + 11, st, strlen(st), SSS, a->inuse ? C_ACCT : C_DIM, X0, Y0, X1, Y1); }
            txt(lx + 44, cy + 9, a->ssid, SF(2), C_TXT, rx - 16);
            if (a->inuse) txt(lx + 45, cy + 9, a->ssid, SF(2), C_TXT, rx - 16);   /* (bold) */
            hitbox(lx - 10, cy, X1 - 14, cy + NROW - 4, K_AP, i);
        }
        cy += NROW;
        if (!on) continue;
        cy += 4;                                         /* chosen: what can be done with it */
        if (!a->inuse && !uuid && needs_pass(a->sec) && !strstr(a->sec, "802.1X")) field("Passphrase", F_PASS, 1, "the network's password");
        pills_begin();
        if (a->inuse) pills("Disconnect", 0, K_DISCONNECT, 0);
        else pills("Connect", 1, K_CONNECT, 0);
        if (uuid) pills("Forget", 0, K_FORGET, 0);
        pills_end();
    }
}
static const char *type_name(const char *t)
{
    static const struct { const char *nm, *say; } k[] = {
        { "802-11-wireless", "Wi-Fi" }, { "802-3-ethernet", "Wired" }, { "vpn", "VPN" }, { "wireguard", "WireGuard" },
        { "bluetooth", "Bluetooth" }, { "bridge", "Bridge" }, { "bond", "Bond" }, { "vlan", "VLAN" }, { "tun", "Tunnel" },
        { "gsm", "Mobile" }, { "cdma", "Mobile" }, { "ethernet", "Wired" }, { "wifi", "Wi-Fi" }, { "bt", "Bluetooth" } };
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) if (!strcmp(t, k[i].nm)) return k[i].say;
    return t;
}
static void page_cons(void)
{
    if (!ncons) { note("NetworkManager keeps no connections.", C_DIM); return; }
    for (int i = 0; i < ncons; i++) {
        con_t *c = &cons[i];
        int on = !strcmp(c->uuid, sel_con);
        if (fits(NROW)) {
            int hov = mx >= lx - 10 && mx < X1 - 14 && my >= cy && my < cy + NROW - 4 && !ask.on;
            if (on) dl_rect(lx - 10, cy, X1 - 14, cy + NROW - 4, 8, C_SEL);
            else if (hov) dl_rect(lx - 10, cy, X1 - 14, cy + NROW - 4, 8, C_BAR);
            if (c->active) dl_rect(lx - 4, cy + 12, lx, cy + 30, 2, C_ACC);
            int rx = X1 - 30;
            char r[80];
            snprintf(r, sizeof r, "%s%s%s", type_name(c->type), c->active ? "  on " : "", c->active ? c->dev : "");
            rx -= text_sw(r, strlen(r), SSS);
            dl_text(rx, cy + 11, r, strlen(r), SSS, c->active ? C_ACCT : C_DIM, X0, Y0, X1, Y1);
            txt(lx + 12, cy + 9, c->name, SF(2), C_TXT, rx - 16);
            if (c->active) txt(lx + 13, cy + 9, c->name, SF(2), C_TXT, rx - 16);
            hitbox(lx - 10, cy, X1 - 14, cy + NROW - 4, K_CON, i);
        }
        cy += NROW;
        if (!on) continue;
        cy += 8;                                         /* chosen: its settings */
        field("Name", F_NAME, 0, 0);
        static const char *const yn[2] = { "No", "Yes" };
        choices("Connect by itself", yn, 2, ed.autoc, K_AUTO);
        if (ed.ip) {
            choices("IPv4", v4_names, 4, ed.v4, K_V4);
            if (ed.v4 == 1) {
                field("Address", F_ADDR, 0, "192.168.1.20/24 (several: commas)");
                field("Gateway", F_GW, 0, "192.168.1.1");
            }
            field("DNS servers", F_DNS, 0, ed.v4 == 1 ? "1.1.1.1, 9.9.9.9" : "none: the network's own");
            choices("IPv6", v6_names, 3, ed.v6, K_V6);
        }
        pills_begin();
        pills("Save", 1, K_SAVE, 0);
        if (c->active) pills("Disconnect", 0, K_DOWN, 0); else pills("Connect", 0, K_UP, 0);
        pills("Delete", 0, K_DELETE, 0);
        pills_end();
        cy += 8;
    }
}
static void page_devs(void)
{
    static const char *const onoff[2] = { "Off", "On" };
    choices("Networking", onoff, 2, networking, K_NETWORKING);
    char t[200];
    snprintf(t, sizeof t, "%s%s%s", nm_state, connectivity[0] && strcmp(connectivity, "unknown") ? ", internet: " : "", connectivity[0] && strcmp(connectivity, "unknown") ? connectivity : "");
    line_text("State", t);
    field("Hostname", F_HOST, 0, hostname);
    if (strcmp(fbuf[F_HOST], hostname) && fbuf[F_HOST][0]) { pills_begin(); pills("Set the hostname", 1, K_HOST, 0); pills_end(); }
    for (int i = 0; i < ndevs; i++) {
        netdev_t *d = &devs[i];
        snprintf(t, sizeof t, "%s  (%s)", d->name, type_name(d->type));
        cy += 8;
        if (fits(34)) title(lx, t);
        cy += 44;
        line_text("State", d->state);
        line_text("Connection", d->con);
        line_text("Hardware", d->hw);
        line_text("IPv4", d->ip4);
        line_text("Gateway", d->gw4);
        line_text("DNS", d->dns);
        line_text("IPv6", d->ip6);
        pills_begin();
        if (!strncmp(d->state, "connected", 9) || !strncmp(d->state, "connecting", 10)) pills("Disconnect", 0, K_DEVDOWN, i);
        else if (strcmp(d->state, "unmanaged")) pills("Connect", 0, K_DEVUP, i);
        pills_end();
    }
}
static void (*const pages[NPAGE])(void) = { page_wifi, page_cons, page_devs };

static void draw_status(void)
{
    int y = SH - STATUS;
    dl_rect(0, y, SW, SH, 0, C_PANEL);
    dl_rect(0, y, SW, y + 1, 0, C_LINE);
    int big_ = 1 << 20;
    if (ask.on) {                                        /* a question: its two answers */
        char a[240];
        ascii(ask.text, a, sizeof a);
        dl_text(16, y + 10, a, strlen(a), SF(2), C_TXT, -big_, -big_, big_, big_);
        const char *yes = ask.act == A_NETOFF ? "Turn off" : ask.act == A_FORGET ? "Forget" : "Delete";
        int x = 16 + text_sw(a, strlen(a), SF(2)) + 20, dw = tws(yes, 2) + 28, cw = tws("Cancel", 2) + 28;
        dl_rect(x, y + 4, x + dw, y + 34, 15, C_ACC);
        dl_text(x + 14, y + 10, yes, strlen(yes), SF(2), C_INK, -big_, -big_, big_, big_);
        hitbox(x, y + 4, x + dw, y + 34, K_ASK, 1);
        x += dw + 12;
        dl_frame(x, y + 4, x + cw, y + 34, 15, 1, C_LINE);
        dl_text(x + 14, y + 10, "Cancel", 6, SF(2), C_TXT, -big_, -big_, big_, big_);
        hitbox(x, y + 4, x + cw, y + 34, K_ASK, 0);
        return;
    }
    if (msg[0]) {
        char a[240];
        ascii(msg, a, sizeof a);
        dl_text(16, y + 10, a, strlen(a), SSS, msg_err ? C_ERR : C_ACCT, -big_, -big_, SW - 16, big_);
    }
}
void fm_draw(void)
{
    hits_begin(&hits);
    dl_rect(0, 0, SW, SH, 0, C_PANEL);
    X0 = 0; Y0 = 0; X1 = SW; Y1 = SH - STATUS;
    int paged = SW >= 720, top = Y0 + 4 - scroll[page];
    if (!nmcli_ok || !nm_ok) {                           /* nothing to talk to */
        lx = cx = 40; cy = 40;
        title(lx, "Network");
        cy += 60;
        note(!nmcli_ok ? "nmcli is not installed: atlas-net needs NetworkManager." : "NetworkManager is not running.", C_ERR);
        if (nmcli_ok) note("Start it (systemctl start NetworkManager), then this window follows.", C_DIM);
        draw_status();
        return;
    }
    int kx0, kx1 = X1 - 24;
    if (paged) {                                         /* the sidebar: the pages, the chosen one lit */
        int y = Y0 + 16;
        dl_rect(SBW, Y0, SBW + 1, Y1, 0, C_LINE);
        for (int i = 0; i < NPAGE; i++) {
            if (i == page) {
                dl_rect(12, y, SBW - 12, y + 40, 10, C_TERM);
                dl_rect(12, y + 10, 16, y + 30, 2, C_ACC);
                bold(30, y + 9, page_names[i], C_TXT, SBW - 12);
            } else label(30, y + 9, page_names[i], 2, C_TXT);
            hitbox(12, y, SBW - 12, y + 40, K_PAGE, i);
            y += 44;
        }
        kx0 = SBW + 24;
        cy = top + 16;
    } else {                                             /* narrow: the pages as buttons over the card */
        kx0 = 16; kx1 = X1 - 16;
        cy = top + 12;
        int x = 16;
        for (int i = 0; i < NPAGE; i++) x = pill(x, cy, page_names[i], i == page, K_PAGE, i) + 8;
        cy += ROW;
    }
    lx = kx0 + 22; cx = kx1 - kx0 >= 660 ? lx + 190 : lx;
    int x1 = X1;
    X1 = kx1;
    if (fits(30)) title(kx0 + 4, page_names[page]);
    cy += 54;
    int c0 = cy;
    dry = 1; pages[page](); dry = 0;                     /* (measured first: the card under the rows) */
    int ky0 = c0 - 20 > Y0 ? c0 - 20 : Y0, ky1 = cy + 4 < Y1 ? cy + 4 : Y1;
    if (ky1 > ky0) { dl_rect(kx0, ky0, kx1, ky1, 14, C_TERM); dl_frame(kx0, ky0, kx1, ky1, 14, 1, C_LINE); }
    cy = c0;
    pages[page]();
    cy += 24;
    X1 = x1;
    content_h = cy - top;
    draw_status();
}

/* ---- The pointer. */
static void choose_page(int p)
{
    page = (p + NPAGE) % NPAGE;
    focus = -1;
    if (page == P_DEVS) { COPY(fbuf[F_HOST], hostname); fcur[F_HOST] = (int)strlen(fbuf[F_HOST]); }
}
void fm_motion(int x, int y) { mx = x; my = y; win_redraw(); }
void fm_button(int x, int y, int button, int pressed)
{
    if (!pressed || button != 1) return;
    const hit_t *h = hits_at(&hits, x, y);
    int kind = h ? h->kind : K_NONE, arg = h ? h->arg : 0;
    if (ask.on) { if (kind == K_ASK) answer(arg); return; }   /* (a question first) */
    if (kind != K_FIELD && kind != K_SHOW) focus = -1;
    char what[160];
    switch (kind) {
    case K_PAGE: choose_page(arg); break;
    case K_RADIO: if (arg != wifi_on && !busy()) run(arg ? "Turning Wi-Fi on" : "Turning Wi-Fi off", 0, -1, "radio", "wifi", arg ? "on" : "off", (char *)0); break;
    case K_SCAN: scan(); break;
    case K_HIDDEN: hidden_open = !hidden_open; if (hidden_open) focus = F_HSSID; break;
    case K_AP:
        if (strcmp(sel_ssid, aps[arg].ssid)) { COPY(sel_ssid, aps[arg].ssid); wipe(F_PASS); }
        else sel_ssid[0] = 0;                            /* (again: closed) */
        if (sel_ssid[0] && !aps[arg].inuse && needs_pass(aps[arg].sec) && !saved(aps[arg].ssid)) focus = F_PASS;
        break;
    case K_CONNECT: if (arg) field_enter(F_HSSID); else connect_ap(); break;
    case K_DISCONNECT: disconnect_ap(); break;
    case K_FORGET: forget_ap(); break;
    case K_FIELD: focus = arg; fcur[arg] = (int)strlen(fbuf[arg]); break;
    case K_SHOW: show_pass = !show_pass; break;
    case K_CON:
        if (strcmp(sel_con, cons[arg].uuid)) { COPY(sel_con, cons[arg].uuid); load_con(); }
        else sel_con[0] = 0;
        break;
    case K_AUTO: ed.autoc = arg; break;
    case K_V4: ed.v4 = arg; break;
    case K_V6: ed.v6 = arg; break;
    case K_SAVE: save_con(); break;
    case K_UP: case K_DOWN: {
        con_t *c = con_by(sel_con);
        if (!c) break;
        snprintf(what, sizeof what, "%s %s", kind == K_UP ? "Connecting" : "Disconnecting", c->name);
        run(what, 0, -1, "connection", kind == K_UP ? "up" : "down", sel_con, (char *)0);
        break;
    }
    case K_DELETE: { con_t *c = con_by(sel_con); if (c) ask_for(A_DELETE, c->uuid, "Delete %s for good?", c->name); break; }
    case K_DEVUP: case K_DEVDOWN:
        if (arg < ndevs) {
            snprintf(what, sizeof what, "%s %s", kind == K_DEVUP ? "Connecting" : "Disconnecting", devs[arg].name);
            run(what, 0, -1, "device", kind == K_DEVUP ? "connect" : "disconnect", devs[arg].name, (char *)0);
        }
        break;
    case K_NETWORKING:
        if (arg == networking) break;
        if (arg) run("Turning networking on", 0, -1, "networking", "on", (char *)0);
        else ask_for(A_NETOFF, "", "Turn all networking off?");
        break;
    case K_HOST: field_enter(F_HOST); break;
    }
    win_redraw();
}
void fm_wheel(double steps)
{
    int vis = SH - STATUS, max = content_h - vis;
    scroll[page] += (int)(steps * 40);
    if (scroll[page] > max) scroll[page] = max;
    if (scroll[page] < 0) scroll[page] = 0;
    win_redraw();
}

/* ---- Keys. */
static void next_field(int back)                         /* Tab: the next field shown on this page */
{
    static const int order[NPAGE][6] = { { F_HSSID, F_HPASS, F_PASS, -1 }, { F_NAME, F_ADDR, F_GW, F_DNS, -1 }, { F_HOST, -1 } };
    const int *o = order[page];
    int k = -1, m = 0;
    while (o[m] >= 0) m++;
    for (int i = 0; i < m; i++) if (o[i] == focus) k = i;
    if (k < 0) k = back ? 0 : m - 1;
    for (int i = 1; i <= m; i++) {
        int f = o[(k + (back ? m - i : i)) % m];
        ap_t *a = sel_ap();
        int shown = f == F_PASS ? a && !a->inuse && !saved(a->ssid) && needs_pass(a->sec) : f == F_HSSID || f == F_HPASS ? hidden_open :
                    f == F_ADDR || f == F_GW ? ed.ip && ed.v4 == 1 : f == F_DNS ? ed.ip : 1;
        if (f >= F_NAME && f <= F_DNS && !sel_con[0]) shown = 0;
        if (shown) { focus = f; fcur[f] = (int)strlen(fbuf[f]); return; }
    }
}
static int utf8_len(uint32_t c) { return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }
static void field_key(xkb_keysym_t sym, uint32_t ch, int mods)
{
    char *b = fbuf[focus];
    int *c = &fcur[focus], n = (int)strlen(b);
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { field_enter(focus); return; }
    if (sym == XKB_KEY_Escape) { focus = -1; return; }
    if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) { next_field((mods & M_SHIFT) || sym == XKB_KEY_ISO_Left_Tab); return; }
    if (sym == XKB_KEY_Left) { while (*c > 0 && (b[--*c] & 0xc0) == 0x80) ; }
    else if (sym == XKB_KEY_Right) { if (*c < n) { ++*c; while (*c < n && (b[*c] & 0xc0) == 0x80) ++*c; } }
    else if (sym == XKB_KEY_Home) *c = 0;
    else if (sym == XKB_KEY_End) *c = n;
    else if (sym == XKB_KEY_BackSpace) {
        int a = *c;
        while (a > 0 && (b[--a] & 0xc0) == 0x80) ;
        if (a < *c) { memmove(b + a, b + *c, n - *c + 1); *c = a; }
    } else if (sym == XKB_KEY_Delete) {
        int e = *c;
        if (e < n) { e++; while (e < n && (b[e] & 0xc0) == 0x80) e++; memmove(b + *c, b + e, n - e + 1); }
    } else if (sym == XKB_KEY_u && (mods & M_CTRL)) { wipe(focus); }
    else if (ch >= 32 && ch != 127 && !(mods & M_CTRL)) {   /* a character, UTF-8 */
        char u[4];
        int l = utf8_len(ch);
        if (l == 1) u[0] = (char)ch;
        else if (l == 2) { u[0] = (char)(0xc0 | ch >> 6); u[1] = (char)(0x80 | (ch & 63)); }
        else if (l == 3) { u[0] = (char)(0xe0 | ch >> 12); u[1] = (char)(0x80 | (ch >> 6 & 63)); u[2] = (char)(0x80 | (ch & 63)); }
        else { u[0] = (char)(0xf0 | ch >> 18); u[1] = (char)(0x80 | (ch >> 12 & 63)); u[2] = (char)(0x80 | (ch >> 6 & 63)); u[3] = (char)(0x80 | (ch & 63)); }
        if (n + l < (int)sizeof fbuf[0]) { memmove(b + *c + l, b + *c, n - *c + 1); memcpy(b + *c, u, l); *c += l; }
    }
}
static void move_sel(int d)                              /* Up, Down: the network or the profile chosen */
{
    if (page == P_WIFI && naps) {
        int k = -1;
        for (int i = 0; i < naps; i++) if (!strcmp(aps[i].ssid, sel_ssid)) k = i;
        k = k < 0 ? 0 : (k + d + naps) % naps;
        COPY(sel_ssid, aps[k].ssid);
        wipe(F_PASS);
    } else if (page == P_CONS && ncons) {
        int k = -1;
        for (int i = 0; i < ncons; i++) if (!strcmp(cons[i].uuid, sel_con)) k = i;
        k = k < 0 ? 0 : (k + d + ncons) % ncons;
        COPY(sel_con, cons[k].uuid);
        load_con();
    }
}
void fm_key(xkb_keysym_t sym, uint32_t ch, int mods)
{
    if (sym == XKB_KEY_Shift_L || sym == XKB_KEY_Shift_R || sym == XKB_KEY_Control_L || sym == XKB_KEY_Control_R ||
        sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R || sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R) return;
    int ctrl = mods & M_CTRL;
    if (ask.on) {
        if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) answer(1);
        else if (sym == XKB_KEY_Escape) answer(0);
        return;
    }
    if (ctrl && (sym == XKB_KEY_Page_Down || sym == XKB_KEY_Page_Up)) choose_page(page + (sym == XKB_KEY_Page_Down ? 1 : -1));
    else if (ctrl && (xkb_keysym_to_lower(sym) == XKB_KEY_w || xkb_keysym_to_lower(sym) == XKB_KEY_q)) win_quit();
    else if (focus >= 0) field_key(sym, ch, mods);
    else switch (sym) {
    case XKB_KEY_Down: move_sel(1); break;
    case XKB_KEY_Up: move_sel(-1); break;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter:
        if (page == P_WIFI && sel_ap()) { ap_t *a = sel_ap(); if (!a->inuse && needs_pass(a->sec) && !saved(a->ssid) && !fbuf[F_PASS][0]) focus = F_PASS; else if (!a->inuse) connect_ap(); }
        else if (page == P_CONS && sel_con[0]) { con_t *c = con_by(sel_con); if (c && !c->active) run("Connecting", 0, -1, "connection", "up", sel_con, (char *)0); }
        break;
    case XKB_KEY_Delete:
        if (page == P_WIFI) forget_ap();
        else if (page == P_CONS && con_by(sel_con)) ask_for(A_DELETE, sel_con, "Delete %s for good?", con_by(sel_con)->name);
        break;
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: next_field((mods & M_SHIFT) || sym == XKB_KEY_ISO_Left_Tab); break;
    case XKB_KEY_F5: if (page == P_WIFI) scan(); else refresh(); break;
    case XKB_KEY_Escape: sel_ssid[0] = 0; msg[0] = 0; break;
    }
    win_redraw();
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    win_open("Network", "atlas.net");
    if (argc > 1) for (int i = 0; i < NPAGE; i++) if (!strcasecmp(argv[1], page_names[i]) || (i == P_WIFI && !strcasecmp(argv[1], "wifi"))) page = i;
    refresh();
    choose_page(page);
    if (nm_ok) monitor_start();
    win_run();
    for (int f = 0; f < NF; f++) explicit_bzero(fbuf[f], sizeof fbuf[f]);
    return 0;
}
