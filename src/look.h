/*
 * look.h - What draws like Atlas, with no compositor in it: the palette,
 * the type scale, click boxes, text helpers and the icons. atlas_wm
 * (atlas.h) and its apps (atlas-files) share it, with draw.c.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "draw.h"

/* Colours: the look's palette (draw.c): glass over a navy desk lit by
 * blue glows: clear glass with white text (the Deskbar, a window's frame),
 * milky glass with dark text (menus, cards), light panels, one amber
 * accent (the tab). At night (draw_night): the glass smoked, light text
 * on menus and panels too, the same amber, a darker desk. */
#define C_BG     pal[PAL_BG]          /* the desk's middle (dl_desk draws the desk) */
#define C_GLASS  pal[PAL_GLASS]       /* a menu, a card: milky glass (fills only), dark text on it */
#define C_CLEAR  pal[PAL_CLEAR]       /* the Deskbar, a window's frame: clear glass, white text on it */
#define C_GTXT   pal[PAL_GTXT]        /* text on clear glass, */
#define C_GDIM   pal[PAL_GDIM]        /* and less of it */
#define C_GLINE  pal[PAL_GLINE]       /* a line on clear glass (see-through) */
#define C_WASH   pal[PAL_WASH]        /* the chosen row on clear glass (see-through) */
#define C_CELL   pal[PAL_CELL]        /* a cell on clear glass (see-through) */
#define C_SMOKE  pal[PAL_SMOKE]       /* dark smoked glass, */
#define C_SMOKEFG pal[PAL_SMOKEFG]    /* its text */
#define C_LINE   pal[PAL_LINE]        /* their edge */
#define C_ACC    pal[PAL_ACC]         /* focus, the current workspace, buttons that matter */
#define C_TXT    pal[PAL_TXT]
#define C_DIM    pal[PAL_DIM]
#define C_BAR    pal[PAL_BAR]         /* the chosen row, separators */
#define C_ERR    pal[PAL_ERR]
#define C_INK    pal[PAL_INK]         /* text on the accent (a tab, a chosen button) */
#define C_PANEL  pal[PAL_PANEL]       /* the ground of Settings and Files */
#define C_ACCT   pal[PAL_ACCT]        /* the accent as text (titles): readable on panels */
#define C_TERM   pal[PAL_TERM]        /* a list's ground (Files) */
#define C_SEL    pal[PAL_SEL]         /* the chosen rows */
#define C_FIELD  pal[PAL_FIELD]       /* where you type */
int look_night(int m);                /* the palette m (0 day, 1 night, 2 auto: ~/.config/atlas/look's night=): night now? */

/* The type scale, for dl_text (16.16): titles, the body (S(2)), small text.
 * At 100% each lands on a native font: 11 x 20, 9 x 16 and 8 x 15 pixels. */
#define ST 160200
#define SS 116510
static inline int tw(int n, int k) { return text_w(n, S(k)); }   /* n characters at scale k (mono) */
/* The proportional font (draw.h's SANS): titles, tabs, labels, buttons,
 * names, fields; numbers and keys stay mono. label() and bold() use it up
 * to scale 3 (bigger text keeps the mono font: the clocks). */
#define SF(k) (S(k) | SANS)
#define STS   (ST | SANS)
#define SSS   (SS | SANS)
static inline int tws(const char *s, int k) { return text_sw(s, strlen(s), k > 3 ? S(k) : SF(k)); }   /* s at scale k, as label() draws it */
void label(int x, int y, const char *s, int k, uint32_t c);
void bold(int x, int y, const char *s, uint32_t c, int cx1);   /* scale 2 */

uint64_t now_ns(void);
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* the log (stderr) */
void ascii(const char *utf8, char *out, size_t cap);  /* for the font: accents dropped, dashes plain, else one '?' */

/* ---- Click boxes, in the picture's coordinates: what a press at (x, y)
 * means. Each piece of chrome keeps its own; searched from the last. */
enum { H_NONE, H_WIN, H_WTAB, H_WMBTN, H_WEDGE, H_DESK, H_DBMENU, H_DBAPP, H_DBWS, H_DBBG, H_MENU, H_LOCKGO };
typedef struct { int x0, y0, x1, y1, kind, arg; } hit_t;
typedef struct { hit_t *h; int n, cap; } hits_t;
void hits_begin(hits_t *hs);                          /* the boxes that follow go to hs */
void hitbox(int x0, int y0, int x1, int y1, int kind, int arg);
const hit_t *hits_at(const hits_t *hs, int x, int y); /* the last box holding (x, y), or 0 */

/* ---- Icons (icons.c): 16 x 16 pixel maps, in a box of 12 x scale; the logo, a picture (logo.c). */
enum { I_TERM, I_SIA, I_FOLDER, I_FILE, I_SETTINGS, I_APP, I_WINDOW, I_PINS, I_GIT, I_MAIL, I_CAM, I_WEB, I_NET, I_N };
void icon(int kind, int x, int y, int scale);
void icon_ink(int kind, int x, int y, int scale, int white);   /* its outline only, in C_INK (on the accent: a tab) or white (on clear glass) */
void logo(int x, int y, int scale);                   /* 16 x scale wide */
int  icon_by_name(const char *name);                  /* "terminal", "folder"...; -1 */
int  icon_for_app(const char *app_id);                /* a guess from an app's id */

