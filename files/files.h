/*
 * files.h - atlas-files: a file manager in Atlas's look (README.md, Files).
 * wl.c is the window (a Wayland client: buffers, input, a loop); files.c
 * is the file manager, drawn with atlas_wm's display list (src/draw.c,
 * src/look.c, src/icons.c) in the picture's units. atlas-net (net/net.c)
 * is a window of the same kind: wl.c, and the fm_ calls of its own.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>
#include "../src/look.h"

/* ---- The window (wl.c). */
enum { M_CTRL = 1, M_SHIFT = 2, M_ALT = 4 };
void win_open(const char *title, const char *app_id); /* exits if there is no Wayland display */
void win_title(const char *title);
void win_redraw(void);                                /* draw again at the next frame */
void win_run(void);                                   /* until win_quit */
void win_quit(void);
void win_fd(int fd, void (*ready)(void));             /* another fd to wait on (inotify) */
void win_timer(int ms, void (*fire)(void));           /* once, ms from now (0: off) */
int  win_focused(void);                               /* it has the keys */
int  win_mods(void);                                  /* the modifiers held (M_...) */
uint64_t win_ms(void);                                /* a clock, in milliseconds */

/* ---- What the window calls (files.c); coordinates in picture units. */
void fm_draw(void);                                   /* the display list for SW x SH */
void fm_motion(int x, int y);
void fm_button(int x, int y, int button, int pressed);   /* button: 1 left, 2 middle, 3 right */
void fm_wheel(double steps);                          /* positive: down */
void fm_key(xkb_keysym_t sym, uint32_t ch, int mods); /* ch: the character typed, or 0 */
