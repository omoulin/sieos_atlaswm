/*
 * draw.h - The drawing layer of the desktop: a display list rasterized
 * into ARGB canvases (the chrome: tabs, frames, the Deskbar, menus, the
 * desk), each shown as a buffer of the scene. A port of SIEOS's
 * user/atlas/draw.c, with alpha (shadows over what is behind).
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

extern int PW, PH;                  /* the primary output, in pixels */
extern int SW, SH;                  /* the picture atlas lays out in: PW x PH / the zoom */
#define CW 6                        /* a character cell of the font, in pixels at scale 1 */
#define CH 11

void dl_reset(void);                /* start a new picture */
void dl_desk(int x0, int y0, int x1, int y1);                               /* the desk's colours: a screen's box */
void dl_rect(int x0, int y0, int x1, int y1, int r, uint32_t c);           /* filled, corner radius r (c FROST | tint: glass; GLOSS: glossy) */
void dl_glass(int x0, int y0, int x1, int y1, int r, uint32_t c, int hx0, int hy0, int hx1, int hy1);   /* glass (c FROST),
                                       not drawn in the hole (what will cover it: a window's insides) */
void dl_frame(int x0, int y0, int x1, int y1, int r, int t, uint32_t c);   /* outline, t pixels thick */
void dl_shade(int x0, int y0, int x1, int y1, int r, int k);               /* darken what is under it: k 1 to 3/4, 2 to 1/2, 3 to 7/8 */
void dl_shadow(int x0, int y0, int x1, int y1, int r, int k, int hx0, int hy0, int hx1, int hy1);   /* the same, but not
                                       in the hole (what will cover it: a window over its own shadow) */
/* Text. Its size is a 16.16 fixed-point scale (S(2): twice the font's
 * 6 x 11 pixels), any value, so text grows smoothly with the zoom. */
#define S(n) ((n) << 16)
void dl_text(int x, int y, const char *s, int n, int scale, uint32_t c,
             int cx0, int cy0, int cx1, int cy1);                         /* clipped to the box */
#define SANS (1 << 30)              /* or-ed into the scale: the proportional font (fontsans.c), for
                                       titles, labels, buttons, names; without it the mono one (numbers, keys) */
int  text_w(int n, int scale);      /* width in pixels of n characters (mono) */
int  text_sw(const char *s, int n, int scale);   /* width in pixels of s's n characters, either font */
int  text_fit(const char *s, int n, int scale, int w);   /* how many of s's n characters fit in w pixels */
/* A pixel image: h rows of w characters, each a colour (cols[i] for keys[i];
 * '.' and ' ' leave what is under it), every pixel scale x scale. The rows
 * are not copied: they must stay (static data). */
void dl_pixmap(int x, int y, const char *const *rows, int w, int h, const char *keys, const uint32_t *cols, int scale);
void dl_pixmap_px(int x, int y, const char *const *rows, int w, int h, const char *keys, const uint32_t *cols, int px);   /* every pixel px x px on the screen, whatever the zoom */
void dl_pic(int x0, int y0, int x1, int y1, const uint8_t *px, const uint32_t *pal, int w, int h);   /* w x h pixels,
                                       each an index into pal (0xAARRGGBB: A its opacity), smoothly scaled into the box */
/* Rasterize the display list into a canvas: w x h pixels (premultiplied
 * 0xAARRGGBB, stride in pixels), cleared first, its top left at screen
 * pixel (x0, y0). Glass is drawn as what it adds over what is behind it:
 * its tint alone (behind: GLASS_TINT, black), or its tint and all the
 * light it lets through (GLASS_LIGHT, white behind it), so the compositor
 * works out the rest from the two (glass.c); GLASS_SOLID: over the desk's
 * middle (a program's own window). -> 1 if glass was drawn in it. */
enum { GLASS_TINT, GLASS_LIGHT, GLASS_SOLID };
int  dl_render(uint32_t *px, int stride, int x0, int y0, int w, int h, int glass);

/* The picture's coordinates to the screen's (pixels of the outputs'
 * buffers): a position, and a picture position for a screen one. */
int  dsc(int v);
int  dunit(int px);                 /* the smallest v with dsc(v) >= px */

/* The colours the desktop draws in (look.h's C_...), the look's: glass.
 * A colour with FROST is glass of that tint (dl_rect, dl_glass): what is
 * behind it shows through, blurred, the tint laid over it by its ALPHA. A
 * fill or an outline with an ALPHA (and no FROST) is laid over what is
 * there, unblurred: see-through. */
#define FROST 0x02000000u
#define GLOSS 0x04000000u           /* (a solid fill with glass's light: the accent) */
#define ALPHA(k) ((uint32_t)(k) << 27)   /* k/32 of the colour (1 to 31) */
#define ALPHA_M 0xf8000000u
#define ALPHA_OF(c) ((int)((c) >> 27))
enum { PAL_BG, PAL_GLASS, PAL_LINE, PAL_ACC, PAL_TXT, PAL_DIM, PAL_BAR, PAL_TERM, PAL_TERMFG, PAL_ERR,
       PAL_SEL, PAL_GRID, PAL_INK, PAL_PANEL, PAL_FIELD, PAL_GAP, PAL_ACCT,
       PAL_CLEAR, PAL_GTXT, PAL_GDIM, PAL_GLINE, PAL_WASH, PAL_CELL, PAL_SMOKE, PAL_SMOKEFG, NPAL };
extern uint32_t pal[NPAL];
uint32_t draw_shown(uint32_t c);    /* a colour as it shows on the screen (glass: over the desk's middle) */
/* The zoom, 16.16: screen pixels per picture pixel (49152, 0.75: 100%
 * on a scale-1 output). Then redraw everything (SW and SH change). */
void draw_zoom(int z);
int  draw_zoom_get(void);
void draw_size(int pw, int ph);     /* the primary output's size in pixels */
void draw_night(int on);            /* the look: day's palette and desk (0), or night's (1); then redraw everything */
int  draw_night_get(void);
