/*
 * wm.c - The desktop's windows: the BeOS way of doing it, in glass (from
 * SIEOS's user/atlas/wm.c).
 *
 *   ┌─────────────────────┐   ┌─────────────┐
 *   │ ▣ Terminal 2   □  x │   │ Term 80 x 25 │  the tab: as wide as its title, on the
 *   ├─────────────────────┴───┴─────────────┤  window's top left: the app's icon, the
 *   │  the app's window (in glass)           │  title; □ zooms (the whole desk, again:
 *   │                                    ⟋⟋⟋ │  back), x closes (both shown with the
 *   └───────────────────────────────────────┘  pointer over the tab); drag it to move,
 *                                               double-click it to minimize, right-click
 * for the window's menu. The grip (or an edge) resizes. With the tab up,
 * the app's id and size sit at the right of the tab's row, on glass.
 *
 * Every window's tab is milky glass; in the focused one amber rises (220
 * ms, a slight overshoot), in the others it sinks back into the frame (170
 * ms). The tab is drawn before the window's glass, so only what rose above
 * the frame shows.
 *
 * A window opened, minimized or brought back shows where it comes from or
 * goes: a ghost of it (a pane of light, its outline) flies between its box
 * and its app's row on the Deskbar (200 ms, easing out); minimized, the row
 * then pulses once. It fades in (140 ms). Switching workspaces, the windows
 * slide an eighth of the screen sideways as they fade (out 150 ms, in 170).
 * These move and fade the scene's buffers, nothing is drawn again. The
 * animation ticks every 16 ms only while a tab, a ghost or a fade moves.
 *
 * Each window sits in a rim of clear glass (10 pixels at its sides and
 * bottom, curving in under its top corners; a strip of it at the top) and
 * casts a soft shadow below it (three shades, a little wider each, no
 * blur); the focused window's is deeper (five, further down), with a thin
 * light rim around its glass. The insides of Settings are dimmed by 3/16
 * when it is not in focus; an app's are shown as it drew them.
 *
 * The frame is drawn in four bands around the app (and the grip over its
 * corner), so what the app covers is never drawn. An app that draws its own
 * title bar (GTK 4's) gets no strip: its bar is the window's top, under the
 * tab. A window may go past the screen's bottom and sides; its tab never
 * leaves the screen (past the left side, the tab slides to stay on it).
 * With several screens, each window is on the one holding its tab: new
 * windows open on the pointer's, zoom and full screen fill their own. Each
 * window lives on one of 4 workspaces (all screens switch together);
 * minimized ones wait on the Deskbar.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"

#define MINW 360                         /* the smallest window */
#define MINH 200
#define BOX 20                           /* the tab's close and zoom boxes */
#define TAB_L 44                         /* the tab: its icon, before the title */
#define TAB_R (16 + BOX + 8 + BOX + 12)  /* ... zoom and close, after it */
#define RIM 10                           /* the glass around a window's sides and bottom */
#define MARGIN 32                        /* what its rim and shadow reach beyond it */

static int cws;                          /* the workspace on the screen */
static struct view *tabbed;               /* whose tab is up */
static struct { int on, mask, px, py, b[4]; struct view *v; } dg;   /* a move (mask 0) or a resize */

/* ---- The tab's animation: progress 0..1024, eased. */
static int prog(uint64_t t0, int ms)
{
    if (!t0) return 1024;
    uint64_t e = (now_ns() - t0) / 1000000;
    return e >= (uint64_t)ms ? 1024 : (int)(e * 1024 / ms);
}
static int ease_in(int p) { return (int)((int64_t)p * p * p >> 20); }
static int back_out(int p)                       /* past the end a little, then back */
{
    int64_t t = p - 1024;
    return 1024 + (int)((2253 * t * t * t >> 30) + (1229 * t * t >> 20));
}
static int rise(struct view *v)                  /* how far the tab is out of the frame now */
{
    int up = v->r_to > v->r_from, p = prog(v->r_t0, up ? 220 : 170);
    if (p >= 1024) { v->r_t0 = 0; return v->r_to; }
    return v->r_from + (v->r_to - v->r_from) * (up ? back_out(p) : ease_in(p)) / 1024;
}

static int ease_out(int p) { return 1024 - ease_in(1024 - p); }

/* ---- Motion: a window's ghost to or from its Deskbar row, the row's
 * pulse; windows fading and sliding (their whole tree: no redraw). */
#define GHOST_MS 200
#define PULSE_MS 500
static struct {
    int on, dock;                                /* (dock: to the row, then its pulse) */
    box_t a, b;                                  /* from, to (picture) */
    uint64_t t0, p0;                             /* flying since; pulsing since (0: not yet) */
    struct chrome c;                             /* (drawn alone, over the windows and the Deskbar) */
    hits_t none;                                 /* (no click boxes: the pointer goes through) */
} gh;
static void ghost(struct view *v, int to_dock)
{
    if (!gh.c.sb) chrome_init(&gh.c, srv.l_bar, CH_MENU, 0, &gh.none);
    box_t w = { v->x0, v->y0 - TAB_H, v->x1, v->y1 }, d = bar_row_box(v);
    gh.a = to_dock ? w : d; gh.b = to_dock ? d : w;
    gh.on = 1; gh.dock = to_dock; gh.t0 = now_ns(); gh.p0 = 0;
    wlr_scene_node_raise_to_top(&gh.c.sb->node);
    anim_kick();
}
static int ghost_step(void)                      /* -> 1 while it flies or pulses */
{
    if (!gh.on) return 0;
    dl_reset();
    int live = 1, r[4], g = 0;
    if (!gh.p0) {                                        /* flying: a pane of light, its outline */
        int e = ease_out(prog(gh.t0, GHOST_MS));
        const int *a = &gh.a.x0, *b = &gh.b.x0;
        for (int i = 0; i < 4; i++) r[i] = a[i] + (int)((int64_t)(b[i] - a[i]) * e >> 10);
        if (r[2] - r[0] > 4 && r[3] - r[1] > 4) {
            dl_rect(r[0], r[1], r[2], r[3], 16, 0xffffff | ALPHA(5));
            dl_frame(r[0], r[1], r[2], r[3], 16, 2, 0xffffff | ALPHA(20));
        }
        if (e >= 1024) { if (gh.dock) gh.p0 = now_ns(); else live = 0; }   /* (minimized: the row pulses) */
    } else {                                             /* pulsing: an amber ring, growing and fading */
        int e = ease_out(prog(gh.p0, PULSE_MS)), k = 29 * (1024 - e) >> 10;
        g = 1 + 10 * e / 1024;
        memcpy(r, &gh.b.x0, sizeof r);
        if (k > 0) dl_frame(r[0] - g, r[1] - g, r[2] + g, r[3] + g, 10 + g, g, (C_ACC & 0xffffff) | ALPHA(k));
        live = e < 1024;
    }
    if (!live) { gh.on = 0; chrome_hide(&gh.c); return 0; }
    chrome_draw(&gh.c, dsc(r[0] - g - 2), dsc(r[1] - g - 2), dsc(r[2] + g + 2), dsc(r[3] + g + 2));
    return 1;
}

static void opacity(struct wlr_scene_node *n, float a)   /* every buffer of a tree, shown or not (glass over white too) */
{
    if (n->type == WLR_SCENE_NODE_BUFFER) wlr_scene_buffer_set_opacity(wlr_scene_buffer_from_node(n), a);
    else if (n->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_node *c;
        wl_list_for_each(c, &wlr_scene_tree_from_node(n)->children, link) opacity(c, a);
    }
}
static int fade_step(struct view *v)             /* -> 1 while it fades */
{
    int p = prog(v->o_t0, v->o_ms), e = v->o_out ? ease_in(p) : ease_out(p), dx = v->o_dx * (v->o_out ? e : 1024 - e) / 1024;
    float a = v->o_out ? 1 - e / 1024.f : e / 1024.f;
    if (p >= 1024) {
        v->o_t0 = 0; a = 1; dx = 0;
        if (v->o_out && !wm_shown(v)) wlr_scene_node_set_enabled(&v->tree->node, false);
    }
    wlr_scene_node_set_position(&v->tree->node, dx, 0);
    opacity(&v->tree->node, a);
    return v->o_t0 != 0;
}
static void fade(struct view *v, int out, int ms, int dx)   /* in (from dx layout pixels aside) or out (to there) */
{
    v->o_t0 = now_ns(); v->o_out = out; v->o_ms = ms; v->o_dx = dx;
    fade_step(v);
    anim_kick();
}
static void still(struct view *v) { if (v->o_t0) { v->o_t0 = 1; v->o_ms = 1; fade_step(v); } }   /* (at rest, now) */
static int eighth(void) { return L(PW / 8); }   /* (a workspace's slide, layout pixels) */

static void title_of(struct view *v, char *t, size_t cap) { ascii(view_title(v), t, cap); }

/* ---- Boxes. */
int wm_strip(struct view *v) { return v->csd ? 3 : WM_STRIP; }
static struct output *out_of(struct view *v)            /* its screen: the one holding its tab (what moves it) */
{
    int x = v->x0 + 120 < v->x1 ? v->x0 + 120 : (v->x0 + v->x1) / 2, y = v->y0 - TAB_H / 2;
    return output_of_box((box_t){ x, y, x + 1, y + 1 });
}
static box_t screen_of(struct view *v) { struct output *o = out_of(v); return o ? desk_screen(o) : (box_t){ 0, 0, SW, SH }; }
static box_t area_of(struct output *o) { return o ? desk_area(o) : (box_t){ 16, 58, SW - 16, SH - 16 }; }
static box_t here(void) { return area_of(output_at(srv.px, srv.py)); }   /* (the pointer's screen: new windows) */
static int tab_w(struct view *v)                 /* the tab: its icon, its title, the two boxes */
{
    char t[128];
    title_of(v, t, sizeof t);
    int n = TAB_L + tws(t, 2) + TAB_R, room = v->x1 - v->x0;
    return n < room ? n : room;
}
static int tab_x(struct view *v) { int l = screen_of(v).x0; return v->x0 < l ? l : v->x0; }   /* (past the left side: it slides) */

static void min_size(struct view *v, int *mw, int *mh)   /* ours, or the app's if it asks for more */
{
    *mw = MINW; *mh = MINH;
    int w, h;
    view_min_size(v, &w, &h);
    int aw = dunit(P(w)) + 2 * WM_IN, ah = dunit(P(h)) + wm_strip(v) + WM_IN;
    if (w && aw > *mw) *mw = aw;
    if (h && ah > *mh) *mh = ah;
}

/* Sizes within limits; the tab wholly on the screen: no further left than
 * the window can still hold it, no further right or down than it fits. */
static void clamp(struct view *v)
{
    int wd = v->x1 - v->x0, h = v->y1 - v->y0, mw, mh;
    struct output *o = out_of(v);
    box_t S = o ? desk_screen(o) : (box_t){ 0, 0, SW, SH }, A = area_of(o);
    min_size(v, &mw, &mh);
    if (wd > S.x1 - S.x0) wd = S.x1 - S.x0;
    if (h > S.y1 - S.y0) h = S.y1 - S.y0;
    if (wd < mw) wd = mw;
    if (h < mh) h = mh;
    v->x1 = v->x0 + wd; v->y1 = v->y0 + h;
    int t = tab_w(v);
    if (v->x0 > S.x1 - t) v->x0 = S.x1 - t;
    if (v->x0 < S.x0 + t - wd) v->x0 = S.x0 + t - wd;
    if (v->y0 > S.y1 - 8) v->y0 = S.y1 - 8;
    if (v->y0 < A.y0) v->y0 = A.y0;
    v->x1 = v->x0 + wd; v->y1 = v->y0 + h;
}

/* The zoom (or the first screen's scale) changed: the picture's units
 * are another size, so each window's box is scaled to keep its place and
 * size on the screen. */
void wm_rezoom(int oldz)
{
    int z = draw_zoom_get();
    struct view *v;
    wl_list_for_each(v, &srv.views, link) {
        int *b[8] = { &v->x0, &v->y0, &v->x1, &v->y1, &v->rx0, &v->ry0, &v->rx1, &v->ry1 };
        for (int i = 0; i < 8; i++) *b[i] = (int)((int64_t)*b[i] * oldz / z);
    }
}

/* The screens changed (one more, one less, a panel): each window keeps
 * its place if its screen is still there, else goes onto the nearest;
 * zoomed and full-screen ones fill their screen again. */
void wm_relayout(void)
{
    struct view *v;
    wl_list_for_each(v, &srv.views, link) {
        if (!v->known) continue;
        if (v->max) { box_t A = area_of(out_of(v)); v->x0 = A.x0; v->y0 = A.y0; v->x1 = A.x1; v->y1 = A.y1; }
        clamp(v);
        if (v->fs) { struct output *o = out_of(v); if (o) wlr_output_layout_get_box(srv.layout, o->wlr, &v->fsb); }
        view_configure(v);
        wm_draw(v);
    }
}

/* Where the app goes in its window, in screen pixels (fullscreen: the whole output). */
void wm_client_box(struct view *v, int *x0, int *y0, int *x1, int *y1)
{
    if (v->fs) { *x0 = P(v->fsb.x); *y0 = P(v->fsb.y); *x1 = P(v->fsb.x + v->fsb.width); *y1 = P(v->fsb.y + v->fsb.height); return; }
    *x0 = dsc(v->x0 + WM_IN); *y0 = dsc(v->y0 + wm_strip(v)); *x1 = dsc(v->x1 - WM_IN); *y1 = dsc(v->y1 - WM_IN);
}

/* The app chose its own size (a dialog, a terminal's whole cells): the
 * window follows it, its top left staying. */
void wm_adopt(struct view *v, int w, int h)
{
    if (v->fs || w <= 0 || h <= 0) return;
    int cx0 = dsc(v->x0 + WM_IN), cy0 = dsc(v->y0 + wm_strip(v));
    v->x1 = dunit(cx0 + P(w)) + WM_IN;
    v->y1 = dunit(cy0 + P(h)) + WM_IN;
    v->max = 0;
    wm_draw(v);
}

/* ---- The stacking order (srv.views, back to front). */
static void lift(struct view *v) { wl_list_remove(&v->link); wl_list_insert(srv.views.prev, &v->link); view_restack(); }
int wm_shown(struct view *v) { return v && v->mapped && v->known && !v->min && v->ws == cws; }

/* A new window: down and to the right of the front one (its tab stays in
 * view), back to the top left when it would not fit. Each kind of app
 * opens at a size for what it shows. */
void wm_place(struct view *v)
{
    box_t A = here();
    int AX0 = A.x0, AY0 = A.y0, AX1 = A.x1, AY1 = A.y1, aw = AX1 - AX0, ah = AY1 - AY0, ic = view_icon(v);
    struct view *top = wm_top();
    if (top && out_of(top) != output_at(srv.px, srv.py)) top = 0;   /* (down and right of the front one on this screen) */
    int w = aw * 56 / 100, h = ah * 70 / 100;
    if (ic == I_FILE || ic == I_WEB || ic == I_MAIL || ic == I_GIT) { v->x0 = AX0 + 20; v->y0 = AY0; v->x1 = v->x0 + aw * 76 / 100; v->y1 = v->y0 + ah * 88 / 100; }
    else if (ic == I_SETTINGS) { v->x0 = AX0 + 60; v->x1 = v->x0 + aw * 54 / 100; v->y0 = AY0; v->y1 = AY1; }
    else {
        if (ic == I_FOLDER) { w = aw * 62 / 100; h = ah * 66 / 100; }   /* (files: a list beside its places) */
        v->x0 = AX0 + 30; v->y0 = AY0 + 10;
        if (top && top->x0 + 48 + w <= AX1 && top->y0 + 48 + h <= AY1) { v->x0 = top->x0 + 48; v->y0 = top->y0 + 48; }
        v->x1 = v->x0 + w; v->y1 = v->y0 + h;
    }
    v->known = 1; v->min = v->max = 0; v->ws = cws;
    v->r_from = v->r_to = 0; v->r_t0 = 0;
    clamp(v);
}

/* A dialog, or an app that keeps its own size: centred over its parent
 * (or the desk), at the size it chose. */
static void place_own(struct view *v, int w, int h)
{
    int cw = dunit(P(w)) + 2 * WM_IN, chh = dunit(P(h)) + wm_strip(v) + WM_IN;
    struct view *p = view_parent(v);
    box_t A = here();
    int mx = p && p->known ? (p->x0 + p->x1) / 2 : (A.x0 + A.x1) / 2, my = p && p->known ? (p->y0 + p->y1) / 2 : (A.y0 + A.y1) / 2;
    v->x0 = mx - cw / 2; v->y0 = my - chh / 2; v->x1 = v->x0 + cw; v->y1 = v->y0 + chh;
    if (p && p->known) v->ws = p->ws;
    clamp(v);
}

/* ---- What the views ask for. */
void wm_show(struct view *v)                     /* on the screen and in front (its workspace with it) */
{
    if (!v) return;
    int appear = v->min || (v->ws == cws && !v->tree->node.enabled);   /* (opened, brought back: from its row) */
    if (!v->known) {                                 /* (it chose its own size: a dialog) */
        int w, h;
        view_own_size(v, &w, &h);
        v->known = 1; v->ws = cws;
        place_own(v, w > 0 ? w : 400, h > 0 ? h : 300);
    }
    if (v->ws != cws) wm_switch(v->ws);
    v->min = 0;
    lift(v);
    wlr_scene_node_set_enabled(&v->tree->node, true);
    view_configure(v);
    wm_draw(v);
    if (appear) { ghost(v, 0); fade(v, 0, 140, 0); }
}
void wm_min(struct view *v)
{
    if (!v || !v->known || v->min) return;
    v->min = 1;
    if (v->ws == cws) ghost(v, 1);                       /* (into its row) */
    still(v);
    wlr_scene_node_set_enabled(&v->tree->node, false);
    say("window minimized: %s", view_title(v));
    if (view_focused() == v) view_focus(wm_top());
    bar_draw();
}
void wm_toggle_max(struct view *v)               /* zoom: the whole desk, or back */
{
    if (!v || !v->known || v->fs) return;
    if (v->max) { v->x0 = v->rx0; v->y0 = v->ry0; v->x1 = v->rx1; v->y1 = v->ry1; v->max = 0; }
    else {
        box_t A = area_of(out_of(v));
        v->rx0 = v->x0; v->ry0 = v->y0; v->rx1 = v->x1; v->ry1 = v->y1;
        v->x0 = A.x0; v->y0 = A.y0; v->x1 = A.x1; v->y1 = A.y1; v->max = 1;
    }
    clamp(v);
    view_configure(v);
    wm_draw(v);
    bar_draw();
}
void wm_fullscreen(struct view *v, int on)
{
    if (!v || !v->known || !!on == !!v->fs) return;
    v->fs = !!on;
    struct output *o = out_of(v);
    if (v->fs && o) wlr_output_layout_get_box(srv.layout, o->wlr, &v->fsb);
    else if (v->fs) v->fs = 0;
    wlr_scene_node_reparent(&v->tree->node, v->fs ? srv.l_fs : srv.l_wins);
    view_restack();
    view_configure(v);
    wm_draw(v);
}
void wm_gone(struct view *v)
{
    if (dg.on && dg.v == v) dg.on = 0;
    if (tabbed == v) tabbed = 0;
    still(v);
    v->known = 0;
}

struct view *wm_top(void)                        /* the front window of this workspace, or 0 */
{
    struct view *v;
    wl_list_for_each_reverse(v, &srv.views, link) if (wm_shown(v)) return v;
    return 0;
}
int wm_workspace(void) { return cws; }
void wm_switch(int ws)
{
    if (ws < 0 || ws >= WM_NWS || ws == cws) return;
    int old = cws, dx = ws > cws ? -eighth() : eighth();   /* (to a later one: they go left, its come from the right) */
    cws = ws;
    struct view *v;
    wl_list_for_each(v, &srv.views, link) {
        int was = v->mapped && v->known && !v->min && v->ws == old;
        if (wm_shown(v)) { wlr_scene_node_set_enabled(&v->tree->node, true); fade(v, 0, 170, -dx); }
        else if (was) fade(v, 1, 150, dx);               /* (shown until it faded) */
        else { still(v); wlr_scene_node_set_enabled(&v->tree->node, false); }
    }
    say("workspace %d", ws + 1);
    struct view *f = view_focused();
    if (!wm_shown(f)) view_focus(wm_top());
    bar_draw();
    desk_draw();
    pointer_refocus();
}
void wm_send(struct view *v, int ws)             /* to another workspace (it stays there) */
{
    if (!v || !v->known || ws < 0 || ws >= WM_NWS) return;
    int was = wm_shown(v);
    v->ws = ws;
    if (was && !wm_shown(v)) fade(v, 1, 150, ws > cws ? eighth() : -eighth());   /* (off toward its workspace) */
    else wlr_scene_node_set_enabled(&v->tree->node, wm_shown(v));
    say("window to workspace %d: %s", ws + 1, view_title(v));
    if (view_focused() == v && !wm_shown(v)) view_focus(wm_top());
    bar_draw();
    desk_draw();
}
void wm_cycle(void)                              /* the backmost window of this workspace comes in front */
{
    struct view *v;
    wl_list_for_each(v, &srv.views, link)
        if (wm_shown(v)) { if (v != wm_top()) view_focus(v); return; }
}

/* Workspace ws's windows as small frames in a cell of the Deskbar, back
 * to front; the front one (on the current workspace) brighter. */
void wm_minimap(int ws, int x0, int y0, int x1, int y1, uint32_t c, uint32_t front)
{
    struct view *top = ws == cws ? wm_top() : 0, *v;
    struct output *o;
    box_t B = { 1 << 30, 1 << 30, -(1 << 30), -(1 << 30) };   /* (all the screens) */
    wl_list_for_each(o, &srv.outputs, link) {
        box_t b = desk_screen(o);
        if (b.x0 < B.x0) B.x0 = b.x0;
        if (b.y0 < B.y0) B.y0 = b.y0;
        if (b.x1 > B.x1) B.x1 = b.x1;
        if (b.y1 > B.y1) B.y1 = b.y1;
    }
    int AX0 = B.x0, AY0 = B.y0, aw = B.x1 - B.x0, ah = B.y1 - B.y0;
    if (aw <= 0 || ah <= 0) return;
    wl_list_for_each(v, &srv.views, link) {
        if (!v->mapped || !v->known || v->min || v->ws != ws) continue;
        int a = x0 + (int)((int64_t)(v->x0 - AX0) * (x1 - x0) / aw), b = y0 + (int)((int64_t)(v->y0 - AY0) * (y1 - y0) / ah);
        int c_ = x0 + (int)((int64_t)(v->x1 - AX0) * (x1 - x0) / aw), d = y0 + (int)((int64_t)(v->y1 - AY0) * (y1 - y0) / ah);
        if (a < x0) a = x0;
        if (c_ > x1) c_ = x1;
        if (d > y1) d = y1;
        if (c_ - a < 4 || d - b < 3) continue;
        dl_rect(a, b, c_, d, 0, C_PANEL);                /* (a front window hides those behind it) */
        dl_frame(a, b, c_, d, 0, 2, v == top ? front : c);
    }
}

/* ---- Moving and resizing with the pointer. */
int wm_grab(struct view *v, int mask, int x, int y)
{
    if (!wm_shown(v) || v->fs) return 0;
    dg.on = 1; dg.v = v; dg.mask = mask;
    dg.px = x; dg.py = y;
    dg.b[0] = v->x0; dg.b[1] = v->y0; dg.b[2] = v->x1; dg.b[3] = v->y1;
    if (mask) view_set_resizing(v, 1);
    return 1;
}
int wm_grabbing(void) { return dg.on; }
void wm_motion(int x, int y)
{
    if (!dg.on) return;
    struct view *v = dg.v;
    int dx = x - dg.px, dy = y - dg.py, mw, mh;
    min_size(v, &mw, &mh);
    v->x0 = dg.b[0]; v->y0 = dg.b[1]; v->x1 = dg.b[2]; v->y1 = dg.b[3];
    if (!dg.mask) { v->x0 += dx; v->x1 += dx; v->y0 += dy; v->y1 += dy; }
    else {
        if (dg.mask & 1) { v->x0 += dx; if (v->x0 > v->x1 - mw) v->x0 = v->x1 - mw; }
        if (dg.mask & 2) { v->x1 += dx; if (v->x1 < v->x0 + mw) v->x1 = v->x0 + mw; }
        if (dg.mask & 4) { v->y0 += dy; if (v->y0 > v->y1 - mh) v->y0 = v->y1 - mh; }
        if (dg.mask & 8) { v->y1 += dy; if (v->y1 < v->y0 + mh) v->y1 = v->y0 + mh; }
    }
    if (v->max) { v->max = 0; bar_draw(); }
    clamp(v);
    view_configure(v);
    wm_draw(v);
}
void wm_drop(void)
{
    if (!dg.on) return;
    struct view *v = dg.v;
    say("window at %d %d %d %d: %s", v->x0, v->y0, v->x1, v->y1, view_title(v));
    if (dg.mask) view_set_resizing(v, 0);
    dg.on = 0;
    view_configure(v);
    bar_draw();
}

/* ---- Drawing. */
static void wbox(int x, int y, int zoom, uint32_t c, int arg, int live)
{
    if (zoom) dl_frame(x + 3, y + 3, x + BOX - 3, y + BOX - 3, 2, 2, c);   /* (zoom: a square) */
    else for (int k = 0; k <= 11; k++) {                 /* (close: a plain x, no box; solid strokes at any zoom) */
        dl_rect(x + 3 + k, y + 3 + k, x + 6 + k, y + 6 + k, 0, c);
        dl_rect(x + 14 - k, y + 3 + k, x + 17 - k, y + 6 + k, 0, c);
    }
    if (live) hitbox(x - 4, y - 4, x + BOX + 4, y + BOX + 4, H_WMBTN, arg);
}

static void info(struct view *v, char *t, size_t cap)   /* the tab's row, at the right, its tab up: the app, its size */
{
    int x0, y0, x1, y1;
    wm_client_box(v, &x0, &y0, &x1, &y1);
    char id[64];
    ascii(view_app(v), id, sizeof id);
    snprintf(t, cap, "%s%s%d x %d", id, id[0] ? "  " : "", L(x1 - x0), L(y1 - y0));
}

/* The window's display list: its tab (and its info), shadow, frame,
 * strip, grip, and where a press on them goes. */
static void build(struct view *v)
{
    int x0 = v->x0, y0 = v->y0, x1 = v->x1, y1 = v->y1, t = tab_w(v), tx = tab_x(v), r = rise(v);
    char title[128];
    title_of(v, title, sizeof title);
    int n = strlen(title), up = r >= TAB_H / 2;          /* (half out: it is the tab; else the strip) */
    dl_reset();
    hits_begin(&v->hits);
    /* the tab, before the glass: only what rose above the frame shows */
    if (r > 0) {
        int ty = y0 - r;
        dl_rect(tx, ty, tx + t, y0 + 12, 10, C_ACC);
        if (up) hitbox(tx, ty, tx + t, y0, H_WTAB, 0);   /* (before its boxes: they win) */
        int cx = tx + t - 12 - BOX, zx = cx - 8 - BOX;   /* (close at the end, zoom before it) */
        icon_ink(view_icon(v), tx + 14, ty + 5, 2, 0);
        dl_text(tx + TAB_L, ty + 6, title, n, SF(2), C_INK, tx + TAB_L - 2, ty, zx - 12, y0);
        dl_text(tx + TAB_L + 1, ty + 6, title, n, SF(2), C_INK, tx + TAB_L - 2, ty, zx - 12, y0);   /* (bold) */
        wbox(cx, ty + 7, 0, C_INK, 2, up);
        if (zx >= tx + TAB_L) wbox(zx, ty + 7, 1, C_INK, 1, up);
    }
    /* its shadow, soft: steps a little wider each, below its glass (within MARGIN); the focused
       window's deeper (five steps, further down): it stands above the others */
    int f = view_focused() == v;
    for (int k = f ? 5 : 3; k >= 1; k--)
        dl_shadow(x0 - RIM - 3 * k + 3, y0 + (f ? 3 : 2) * k + 3, x1 + RIM + 3 * k - 3, y1 + RIM + 4 * k, 18 + 3 * k, 3, x0 - RIM + 6, y0, x1 + RIM - 6, y1 + RIM - 6);
    /* the window: a slab of clear glass, its strip at the top and a rim at
       its sides and bottom, curving in under its top corners; the app covers
       its middle (the glass is not drawn there) */
    dl_glass(x0 - RIM, y0, x1 + RIM, y1 + RIM, 18, C_CLEAR, x0 + WM_IN, y0 + wm_strip(v), x1 - WM_IN, y1 - WM_IN);
    if (r > 0) dl_rect(tx + 2, y0, tx + t - 2, y0 + 2, 0, C_ACC);   /* (the tab joins it) */
    if (up && !v->csd)                                   /* its amber, seen through the strip's glass (SIEOS's glass blurs */
        for (int k = 0; k < 6; k++)                      /* the tab under it; here the glass is over the desk: drawn) */
            dl_rect(tx - 4 + 2 * k, y0 + 2, tx + t + 4 - 2 * k, y0 + 22 - 3 * k, 8, ALPHA(2) | (C_ACC & 0xffffff));
    hitbox(x0, y0, x1, y1, H_WIN, 0);
    if (!v->csd) {                                       /* (its own title bar: it is the strip, and moves it) */
        if (!up) {                                       /* sunk: the title in the strip, a handle to move it */
            dl_text(tx + 22, y0 + 6, title, n, SF(2), C_GTXT, x0 + 2, y0, x1 - 20, y0 + WM_STRIP);
            hitbox(x0 + 8, y0 + 3, x1 - 8, y0 + WM_STRIP, H_WTAB, 0);
        } else {                                         /* up: the strip says more */
            char in[96];
            info(v, in, sizeof in);
            int k = strlen(in);
            if (k) dl_text(x1 - 22 - text_sw(in, k, SSS), y0 + 8, in, k, SSS, C_GTXT, x0 + 20, y0, x1 - 20, y0 + WM_STRIP);
            hitbox(tx + t, y0 + 3, x1 - 8, y0 + WM_STRIP, H_WTAB, 0);   /* (the strip beside the tab moves it too) */
        }
    }
    /* the grip: three short diagonals in the bottom-right corner */
    for (int i = 0; i < 3; i++)
        for (int k = 0; k <= 4 + 5 * i; k += 2)
            dl_rect(x1 - 10 - (4 + 5 * i) + k, y1 - 10 - k, x1 - 8 - (4 + 5 * i) + k, y1 - 8 - k, 0, C_GDIM);
    /* the edges and the grip, last: they win near the frame (the rim too) */
    hitbox(x0 - RIM - 2, y0 + 16, x0 + 5, y1 - 16, H_WEDGE, 1);
    hitbox(x1 - 5, y0 + 16, x1 + RIM + 2, y1 - 16, H_WEDGE, 2);
    hitbox(tx + t, y0 - 4, x1 - 16, y0 + 3, H_WEDGE, 4);
    hitbox(x0 + 16, y1 - 5, x1 - 16, y1 + RIM + 2, H_WEDGE, 8);
    hitbox(x0 - RIM - 2, y1 - 14, x0 + 14, y1 + RIM + 2, H_WEDGE, 9);
    hitbox(x1 - 24, y1 - 24, x1 + RIM + 2, y1 + RIM + 2, H_WEDGE, 10);
    hitbox(x1 - 16, y0 - 4, x1 + RIM + 2, y0 + 14, H_WEDGE, 6);
    hitbox(x0 - RIM - 2, y0 - 4, x0 + 14, y0 + 3, H_WEDGE, 5);
}

/* Draw the window as it is now: the bands of its frame around the app, the
 * grip over the app's corner, the dim over Settings when not in focus. */
static int is_term(struct view *v) { return !strcmp(view_app(v), "Term"); }   /* (Atlas's terminal: its ground smoked glass) */
static void smoke(struct view *v, int on, int cx0, int cy0, int cx1, int cy1)   /* (as SIEOS's terminals: the desk through them, blurred) */
{
    if (!on) { chrome_hide(&v->smoke); return; }
    dl_reset();
    hits_begin(&v->shits);
    dl_rect(v->x0 - RIM, v->y0 - TAB_H, v->x1 + RIM, v->y1 + RIM, 0, C_SMOKE);
    chrome_draw(&v->smoke, cx0, cy0, cx1, cy1);
}

void wm_draw(struct view *v)
{
    if (!v || !v->known || !v->mapped) return;
    int f = view_focused() == v;
    int cx0, cy0, cx1, cy1;
    wm_client_box(v, &cx0, &cy0, &cx1, &cy1);
    view_place_app(v);
    if (v->fs) {                                         /* fullscreen: the app alone */
        for (int i = 0; i < NBAND; i++) chrome_hide(&v->band[i]);
        chrome_hide(&v->grip);
        wlr_scene_node_set_enabled(&v->dim->node, false);
        if (is_term(v)) solid_place(v->ground, 0, 0, 0, 0);
        else solid_place(v->ground, cx0, cy0, cx1, cy1);
        smoke(v, is_term(v), cx0, cy0, cx1, cy1);
        wlr_scene_node_set_enabled(&v->app->node, true);
        wlr_scene_node_set_enabled(&v->popups->node, true);
        hits_begin(&v->hits);
        return;
    }
    build(v);
    int ox0 = dsc(v->x0 - MARGIN), oy0 = dsc(v->y0 - TAB_H - 4), ox1 = dsc(v->x1 + MARGIN), oy1 = dsc(v->y1 + MARGIN);
    chrome_draw(&v->band[0], ox0, oy0, ox1, cy0);
    chrome_draw(&v->band[1], ox0, cy1, ox1, oy1);
    chrome_draw(&v->band[2], ox0, cy0, cx0, cy1);
    chrome_draw(&v->band[3], cx1, cy0, ox1, cy1);
    int gx = dsc(v->x1 - 26), gy = dsc(v->y1 - 26);
    chrome_draw(&v->grip, gx > cx0 ? gx : cx0, gy > cy0 ? gy : cy0, cx1, cy1);
    if (is_term(v)) solid_place(v->ground, 0, 0, 0, 0);   /* (its ground: smoked glass) */
    else solid_place(v->ground, cx0, cy0, cx1, cy1);
    smoke(v, is_term(v), cx0, cy0, cx1, cy1);
    solid_place(v->dim, cx0, cy0, cx1, cy1);
    wlr_scene_node_set_enabled(&v->dim->node, !f && v->internal);
    wlr_scene_node_set_enabled(&v->app->node, true);
    wlr_scene_node_set_enabled(&v->popups->node, true);
    if (v->internal) settings_draw(v);                   /* (its inside is ours) */
}
void wm_draw_all(void) { struct view *v; wl_list_for_each(v, &srv.views, link) wm_draw(v); }

/* The focus moved: the old tab sinks, the new one rises. */
void wm_focus_changed(void)
{
    struct view *f = view_focused();
    if (f == tabbed) return;
    if (tabbed && tabbed->known) { tabbed->r_from = rise(tabbed); tabbed->r_to = 0; tabbed->r_t0 = now_ns(); wm_draw(tabbed); }
    if (f && f->known) { f->r_from = rise(f); f->r_to = TAB_H; f->r_t0 = now_ns(); wm_draw(f); }
    tabbed = f;
    anim_kick();
}

/* The animation's next frame: the tabs that move, the ghost, the windows
 * fading. -> 1 while one still moves. */
int wm_tick(void)
{
    int any = ghost_step();
    struct view *v;
    wl_list_for_each(v, &srv.views, link) {
        if (v->o_t0) any |= fade_step(v);
        if (!v->known || (!v->r_t0 && !v->was)) continue;
        int moving = v->r_t0 != 0;
        if (moving) rise(v);                             /* (ends it when its time is up) */
        wm_draw(v);
        v->was = moving;
        any |= moving || v->r_t0;
    }
    return any;
}
