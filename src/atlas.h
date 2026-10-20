/*
 * atlas.h - What atlas_wm's files share: the compositor (main.c), its
 * outputs and the glass (output.c, glass.c), the apps' windows (view.c),
 * the window manager (wm.c), the chrome drawn on the CPU (chrome.c), the
 * Deskbar and the menus (deskbar.c), the palette (palette.c), X11 apps (xwayland.c), panels and
 * wallpapers (layers.c), screen lockers (lock.c), the icons (icons.c).
 *
 * The desktop is SIEOS's Atlas (title tabs as wide as their title, the
 * Deskbar at the top right, 4 workspaces), as a Wayland compositor on
 * wlroots 0.18; README.md.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "draw.h"

#include "look.h"

/* ---- Chrome: a canvas drawn from the display list, shown as a buffer of
 * the scene. Its box is in screen pixels; it carries who it belongs to, for
 * the pointer (chrome.c). */
enum { CH_DESK = 1, CH_VIEW, CH_BAR, CH_MENU, CH_LOCK };
struct chrome {
    struct wlr_scene_buffer *sb;
    int kind;
    void *owner;                                     /* a view, or 0 */
    hits_t *hits;                                    /* where it takes the pointer (0: everywhere) */
    int x0, y0, w, h;                                /* screen pixels */
    int shown;
    struct wlr_scene_buffer *lit;                    /* glass in it: drawn over white too (chrome.c), */
    struct wlr_texture *mask[2];                     /* where it is: white, black (glass.c) */
    struct wlr_buffer *mbuf[2];
    struct wl_list glink;
    int level;                                       /* (glass.c: which of a frame's levels it is) */
};
void chrome_init(struct chrome *c, struct wlr_scene_tree *parent, int kind, void *owner, hits_t *hits);
void chrome_draw(struct chrome *c, int x0, int y0, int x1, int y1);   /* the display list, into that box */
void chrome_hide(struct chrome *c);
void chrome_fini(struct chrome *c);
struct wlr_buffer *pixbuf_create(int w, int h);       /* ARGB8888 in memory, cleared */
uint32_t *pixbuf_data(struct wlr_buffer *b, int *stride);
struct wlr_scene_buffer *solid_create(struct wlr_scene_tree *parent, uint32_t argb, bool input);   /* one pixel, stretched */
void solid_color(struct wlr_scene_buffer *sb, uint32_t argb);
void solid_place(struct wlr_scene_buffer *sb, int x0, int y0, int x1, int y1);   /* screen pixels */
struct chrome *chrome_at(struct wlr_scene_node *node);   /* a chrome's buffer, or 0 */
void chrome_glass(int glass);                         /* the pieces with glass: over black (GLASS_TINT) or white (GLASS_LIGHT) */
int  chrome_glass_in(struct wlr_box b);               /* glass seen in this box of the layout */
int  chrome_has_glass(struct chrome *c);

/* A box in the picture's coordinates. */
typedef struct { int x0, y0, x1, y1; } box_t;

/* ---- The windows (view.c: the xdg-shell toplevels; xwayland.c: X11's;
 * wm.c: their places). */
#define WM_NWS 4                                      /* workspaces */
#define TAB_H 34                                      /* a window's tab */
#define WM_STRIP TAB_H                                /* the strip at a window's top (title when its tab sank) */
#define WM_IN 6                                       /* the frame beside and under the app */
#define NBAND 4

struct wlr_xwayland_surface;
struct view {
    struct wl_list link;                             /* srv.views: back to front */
    struct wlr_xdg_toplevel *top;                    /* a Wayland app's window, or ... */
    struct wlr_xwayland_surface *xs;                 /* ... an X11 app's */
    struct wlr_scene_tree *xsurf;                    /* (X11: its surface in app) */
    struct wlr_scene_tree *tree;                     /* the window: frame, app, grip */
    struct wlr_scene_tree *app, *popups;             /* the app's surfaces; its menus (over its frame) */
    struct chrome band[NBAND], grip;                 /* the frame around the app; the grip */
    struct chrome smoke;                             /* under a Term (Atlas's terminal, see-through): smoked glass */
    hits_t shits;                                    /* (none: it takes no clicks) */
    struct wlr_scene_buffer *ground, *dim;           /* under the app (it may not fill its box); over it, not in focus */
    hits_t hits;
    struct wlr_xdg_toplevel_decoration_v1 *deco;     /* it asked for our decorations (xdg-decoration; GTK 3: KDE's, view.c) */
    int csd;                                         /* it draws its own title bar: no strip of ours */
    int internal;                                    /* a window of atlas_wm's own (Settings): its inside is chrome */
    struct chrome body;                              /* ... drawn here */
    hits_t bhits;
    int scroll;
    struct wlr_foreign_toplevel_handle_v1 *ftl;      /* what taskbars see of it (toplevels.c) */
    struct wlr_ext_foreign_toplevel_handle_v1 *eftl;
    struct wlr_output *ftl_out;
    int ftl_state;                                   /* (what they were last told) */
    int mapped, known, ws, min, max, fs;             /* placed once; its workspace; minimized; zoomed; fullscreen */
    struct wlr_box fsb;                              /* fullscreen: its output (layout) */
    int x0, y0, x1, y1;                              /* its box (the tab is above it), picture coordinates */
    int rx0, ry0, rx1, ry1;                          /* zoomed or fullscreen: the box to go back to */
    int r_from, r_to;                                /* the tab: rising (to TAB_H) or sinking (to 0) ... */
    uint64_t r_t0;                                   /* ... since then (0: at rest) */
    int was;                                         /* moved at the last tick: one more redraw */
    uint64_t o_t0;                                   /* fading in or out (open, restore, a workspace) since then (0: at rest) */
    int o_out, o_ms, o_dx;                           /* ... out; how long; sliding from (in) or to (out) dx layout pixels */
    uint64_t attn_ns;                                /* asks for attention since then (0: not): the tray's pill */
    int sent_w, sent_h;                              /* the size last asked of the app (layout pixels) */
    uint32_t sent_serial;
    uint64_t focus_ns;                               /* last in focus (the Deskbar brings that one) */
    struct wl_listener map, unmap, commit, destroy, request_move, request_resize, request_maximize,
                       request_fullscreen, request_minimize, request_menu, set_title, set_app_id,
                       deco_mode, deco_destroy,
                       associate, dissociate, request_configure, request_activate, set_decorations, set_geometry,
                       ftl_activate, ftl_maximize, ftl_minimize, ftl_fullscreen, ftl_close, ftl_destroy;
};
void view_init(void);
const char *view_title(struct view *v);
const char *view_app(struct view *v);                 /* its app_id, "" if none */
int  view_icon(struct view *v);                       /* its app's icon (I_...) */
void view_close(struct view *v);
void view_configure(struct view *v);                  /* the app: its box, its state (activated...) */
void view_focus(struct view *v);                      /* in front, the keys (0: none) */
struct view *view_focused(void);
void view_restack(void);                              /* the scene in the order of srv.views */
void view_place_app(struct view *v);                  /* the app's surfaces where its box says */
struct view *view_of(struct wlr_xdg_toplevel *t);
struct wlr_surface *view_surface(struct view *v);    /* its main surface (keys go there), or 0 */
void view_min_size(struct view *v, int *w, int *h);   /* what the app asks at least (layout pixels; 0: nothing) */
void view_own_size(struct view *v, int *w, int *h);   /* the size it has now (layout pixels) */
struct view *view_parent(struct view *v);             /* a dialog's window */
void view_set_resizing(struct view *v, int on);
void view_csd_update(struct view *v);                 /* its decorations changed hands */
void view_tree_init(struct view *v);                  /* the window's tree (view.c's order) */
void view_mapped(struct view *v);                     /* (xwayland.c) shown: placed, in front */
void view_unmapped(struct view *v);
void view_destroyed(struct view *v);                  /* (frees it) */
void xwayland_init(void);
void xwayland_configure(struct view *v, int lx, int ly, int w, int h);
void xwayland_activate(struct view *v, int on);
/* wm.c */
void wm_relayout(void);                               /* the screens changed: each window still on one */
void wm_rezoom(int oldz);                             /* the zoom changed from oldz: boxes keep their screen size */
int  wm_strip(struct view *v);                        /* the strip at its top (its own title bar: almost none) */
void wm_place(struct view *v);                        /* a new window's box */
void wm_show(struct view *v);                         /* on the screen, in front (its workspace with it) */
void wm_min(struct view *v);
void wm_toggle_max(struct view *v);                   /* zoom: the whole desk, or back */
void wm_fullscreen(struct view *v, int on);
void wm_gone(struct view *v);
int  wm_shown(struct view *v);
struct view *wm_top(void);                            /* the front window of this workspace, or 0 */
int  wm_workspace(void);
void wm_switch(int ws);
void wm_send(struct view *v, int ws);
void wm_cycle(void);                                  /* the next window of this workspace, in front */
void wm_minimap(int ws, int x0, int y0, int x1, int y1, uint32_t c, uint32_t front);
int  wm_grab(struct view *v, int mask, int x, int y); /* a move (mask 0) or a resize (1 left 2 right 4 top 8 bottom) */
void wm_motion(int x, int y);
void wm_drop(void);
int  wm_grabbing(void);
void wm_draw(struct view *v);                         /* its frame, as it is now */
void wm_draw_all(void);
int  wm_tick(void);                                   /* the animation: -> 1 while something moves */
void wm_focus_changed(void);                          /* tabs rise and sink */
void wm_adopt(struct view *v, int w, int h);          /* the app chose this size (layout pixels): the box follows */
void wm_client_box(struct view *v, int *x0, int *y0, int *x1, int *y1);   /* where the app goes (screen pixels) */

/* ---- The Deskbar, the desk, the menus (deskbar.c). */
void bar_init(void);
void bar_layout(void);                                /* the outputs changed */
struct output;
box_t desk_screen(struct output *o);                  /* the whole screen, picture coordinates */
box_t desk_area(struct output *o);                    /* where its windows go (beside the Deskbar, under panels) */
void bar_draw(void);                                  /* the Deskbar again (windows, the clock) */
void desk_draw(void);                                 /* the desk again (each output) */
void bar_hover(int x, int y);                         /* the pointer, for the rows it lights */
box_t bar_row_box(struct view *v);                    /* v's app's row on the Deskbar (else the menu's row) */
void bar_attention(struct view *v, int on);           /* v asks for attention (an activation, X11's urgency), or not */
int  bar_press(struct chrome *c, int x, int y, int button);   /* 1 if taken */
int  menu_open_now(void);
void menu_close(void);
void menu_forget(struct view *v);                     /* it goes: a menu about it closes */
void menu_window(struct view *v, int x, int y);       /* a window's menu at (x, y) */
void menu_main(void);                                 /* the cube's menu */
void menu_key(xkb_keysym_t sym);
void bar_reload(void);                                /* the apps' file again */
void spawn(const char *cmd);
void spawn_terminal(void);
const char *term_preset(const char *name);           /* "foot": Atlas's Term command for it (title Term, its colours), or 0 */
/* palette.c: Super+Space, the apps, windows, files, actions, a command */
int  palette_on(void);
void palette_open(void);
void palette_close(void);                             /* (menu_close closes it too: the lock) */
int  palette_key(xkb_keysym_t sym, uint32_t ch);      /* every key while it is open */
int  palette_press(struct chrome *c, int x, int y);   /* 1 if taken; elsewhere: closed */

/* ---- Outputs and the glass (output.c, glass.c). */
struct glass_gl;
struct output {
    struct wl_list link;
    struct wlr_output *wlr;
    struct wlr_scene_output *so;
    struct wlr_swapchain *off[3];                    /* glass seen: the scene draws A and B here, glass.c what is behind it */
    struct glass_gl *glass;                          /* (glass.c's, on the GPU) */
    struct chrome desk;
    struct wlr_box usable;                           /* what panels leave (layout) */
    struct wlr_scene_buffer *lockbg;                 /* locked: black */
    struct chrome lockc;                             /* Atlas's own lock screen (lock.c) */
    struct wl_listener frame, request_state, destroy;
};
void output_init(void);
struct output *output_primary(void);
struct output *output_at(double lx, double ly);       /* the screen there, or the nearest */
struct output *output_of(struct wlr_output *wo);
struct output *output_of_box(box_t b);                /* the screen holding a box's middle (picture) */

/* ---- Panels, wallpapers, notifications (layers.c: wlr-layer-shell). */
void layers_init(void);
void layers_arrange(struct output *o);                /* place them; the windows' room follows */
int  layers_keys(void);                               /* a layer surface has the keys */
void layers_focus_at(struct wlr_surface *s);          /* a press on one: the keys if it takes them */
void layers_output_gone(struct output *o);

/* ---- Settings (settings.c): a window of atlas_wm's own. */
void settings_open(void);
void settings_draw(struct view *v);                   /* its inside, into its box */
void settings_press(struct view *v, int x, int y, int button);
void settings_scroll(struct view *v, double d);
int  settings_key(struct view *v, xkb_keysym_t sym, uint32_t ch);   /* 1 if a field took it */
void settings_gone(struct view *v);
void settings_load(void);                             /* ~/.config/atlas: the size, the palette, the mouse, the screens */
void settings_clock(void);                            /* a minute went by: the palette by the clock */
void settings_output(struct wlr_output *wo, struct wlr_output_state *st);   /* a new screen: its kept mode and scale */
void mouse_apply(struct wlr_input_device *d);         /* the mouse's settings onto a pointer (0: every one) */
int  mouse_left(void);                                /* left-handed: the buttons swapped */
int  mouse_natural(void);                             /* the wheel reversed */
const char *config_path(const char *name, char *buf, size_t cap);   /* ~/.config/atlas/NAME */

/* ---- Taskbars (toplevels.c: wlr-foreign-toplevel, ext-foreign-toplevel-list). */
void toplevels_init(void);
void toplevels_update(void);                          /* every window as it is now */
void toplevels_gone(struct view *v);

/* ---- The Deskbar's apps (deskbar.c), for Settings. */
int  apps_count(void);
const char *apps_name(int i);
int  apps_icon(int i);
void apps_move(int i, int d);                         /* up (-1) or down (1) */
void apps_remove(int i);
void apps_add(const char *name, const char *icon_name, const char *cmd, const char *id, int dock);
int  apps_dock(int i);                                /* a row on the Deskbar (else the menu only) */
void apps_set_dock(int i, int on);
const char *apps_cmd(int i);
void apps_edit(int i, const char *name, const char *cmd);
int  apps_has(const char *id);                        /* an app of the list runs as id */
void apps_save(void);
const char *icon_name(int icon);

/* ---- Screen lockers (lock.c: ext-session-lock). */
void lock_init(void);
int  locked(void);
void lock_output(struct output *o);                   /* a new screen while locked: black */
void lock_refocus(void);
void lock_now(void);                                  /* Atlas's own lock screen (Super+L, the menus, SIGUSR1) */
void lock_redraw(void);                               /* (the clock, the screens changed) */
int  lock_key(xkb_keysym_t sym, uint32_t ch);         /* 1 if it took the key */
int  lock_press(int x, int y);                        /* 1 if it took the press (picture coordinates) */
void output_damage_all(void);                         /* every output draws a new frame */
/* the frame into dst from the scene's with its glass over black (a) and white (b) (glass.c) */
bool glass_render(struct output *o, struct wlr_buffer *a, struct wlr_buffer *b, struct wlr_buffer *dst);
void glass_fini(struct output *o);

/* ---- The compositor (main.c). */
struct server {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wlr_backend *backend;
    struct wlr_session *session;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_output_layout *layout;
    struct wl_list outputs;
    /* the scene's layers, back to front */
    struct wlr_scene_tree *l_desk, *l_bg, *l_bottom, *l_wins, *l_bar, *l_top, *l_fs, *l_overlay, *l_menu, *l_xmenu, *l_lock;
    struct wlr_compositor *compositor;
    struct wlr_xwayland *xwayland;
    struct wlr_idle_notifier_v1 *idle;
    struct wlr_xdg_shell *xdg;
    struct wl_list views;
    struct wlr_cursor *cursor;
    struct wlr_buffer *arrow;                        /* the desktop's pointer */
    struct wlr_seat *seat;
    struct wl_list keyboards;
    double scale;                                    /* the first screen's scale (1, 1.25, 2...): screen pixels per layout pixel */
    int zoom_pct;                                    /* the size of everything: 100 */
    double px, py;                                   /* the pointer, layout coordinates */
    int client_cursor;                               /* an app's surface sets the pointer */
    struct wl_event_source *anim, *clock;
    char terminal[256];
    struct wl_listener new_output, new_input, new_toplevel, new_popup, new_deco,
                       cursor_motion, cursor_motion_abs, cursor_button, cursor_axis, cursor_frame,
                       request_cursor, request_set_selection, request_set_primary, output_layout_change;
};
extern struct server srv;
/* layout pixels <-> screen pixels (at the first screen's scale; the chrome is drawn at it) */
static inline int L(int px) { double v = px / srv.scale; return (int)(v < 0 ? v - 0.5 : v + 0.5); }
static inline int P(double lx) { double v = lx * srv.scale; return (int)(v < 0 ? v - 0.5 : v + 0.5); }
void anim_kick(void);                                 /* something moves: ticks every 16 ms until all rest */
void cursor_arrow(void);                              /* the desktop's own pointer */
void pointer_refocus(void);                           /* what is under the pointer changed */
void atlas_quit(void);
