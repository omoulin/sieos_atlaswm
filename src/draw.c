/*
 * draw.c - The drawing layer of the desktop.
 *
 * The desktop describes a picture as a "display list": the desk,
 * rectangles (rounded or not), glass, outlines, shades, text, small pixel
 * images (the icons: a map of characters, each one a colour of a palette)
 * and pictures (the logo). dl_render() turns it into the pixels of one
 * canvas: a band of a window's frame, the Deskbar, a menu, the desk. Each
 * canvas is a buffer of the scene, over or under the apps' windows. So
 * only what changed is drawn again, and never what an app's window covers.
 *
 * Canvases have alpha (premultiplied): what is not drawn shows what is
 * behind, and a shade over nothing is a shadow (black, an eighth to a
 * half opaque) over the windows and the desk under it.
 *
 * The look: glass. The desk is a deep navy lit by five wide glows, all
 * blues (worked out at 1/16 of the screen, then smoothed to each pixel as
 * it is drawn); at night (draw_night) a darker navy, the glows turned
 * down, and the night palette. Glass (a FROST colour: dl_rect, dl_glass)
 * shows what is behind it, the desk or other windows, blurred and more
 * vivid, its tint laid over it as much as its ALPHA says: clear (the Deskbar, a window's
 * frame), milky (menus, cards) or smoked; a light edge, a bright line
 * inside its top. A canvas cannot see what is behind it, so glass is drawn
 * twice: its tint alone (as if over black) and its tint with all the light
 * it lets through (as if over white). Both are straight lines in what is
 * behind, and so is everything drawn over glass, so the compositor, which
 * sees the whole frame, puts the blurred picture behind it in between
 * (glass.c). A GLOSS colour (the accent) is solid with glass's top line.
 *
 * The zoom (draw_zoom): atlas lays out a picture of SW x SH, the screen's
 * PW x PH divided by the zoom; each item is scaled to the screen as it is
 * added (its box, corners, lines, text, icons), so it is drawn sharp at
 * any zoom, not a picture stretched. At 100% text lands on the native
 * fonts (DejaVu Sans Mono rendered once), pixel for pixel; SANS text on
 * the proportional ones (DejaVu Sans, sans.c), sharp at 100% and 200%.
 *
 * From SIEOS's user/atlas/draw.c. No floating point: integers throughout.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "draw.h"
#include "fontsans.h"

extern const uint8_t font[95][9];         /* font.c (SIEOS tools/mkfont.py) */
extern const uint8_t font15[95][16][9];   /* font15.c: the native one at 1.5, */
extern const uint8_t font18[95][20][11];  /* font18.c: titles, at 11/6 */
extern const uint8_t font13[95][15][8];   /* font13.c: small text, at 8/6 */
/* The native fonts: text at exactly this scale on the screen (16.16) is
 * drawn from them pixel for pixel. */
static const struct { int scale, w, h; const uint8_t *px; } natives[] = {
    { 3 << 15, 9, 16, &font15[0][0][0] }, { 120150, 11, 20, &font18[0][0][0] }, { 87382, 8, 15, &font13[0][0][0] },
};

#define MAXP  4096                  /* display list entries (more are dropped) */
#define ARENA (32 << 10)            /* the display list's text */

enum { P_DESK, P_RECT, P_FRAME, P_TEXT, P_PIXMAP, P_SHADE, P_PIC };
typedef struct {
    uint8_t type;
    uint16_t r, t, len;
    int scale;                      /* text: 16.16 */
    int x0, y0, x1, y1;             /* its box on the screen (may extend beyond it) */
    int cx0, cy0, cx1, cy1;         /* text: clipped to this box; glass, a shadow: its hole */
    uint32_t c, c2, text;           /* colours; text: where its characters are in the arena */
    const char *const *rows;        /* a pixmap: its rows, ... */
    const char *keys;               /* ... the characters of its palette, ... */
    const uint32_t *cols;           /* ... their colours ('.' and ' ': none) */
} prim_t;

int PW = 1920, PH = 1080;           /* the primary output, in pixels */
int SW = 2560, SH = 1440;           /* the picture atlas lays out: the screen / the zoom */
static int Z = 49152;               /* the zoom, 16.16: screen pixels per picture pixel (0.75: 100%) */
static prim_t dl[MAXP];
static int np, arena_n;
static char arena[ARENA];

/* The canvas being drawn: rows of `stride` pixels, its top left at (tx, ty);
 * glass drawn over this behind it (GLASS_...), and whether any was. */
static struct { uint32_t *px; int stride, x0, y0, w, h, glass, drew; } T;
static inline uint32_t *row(int y) { return T.px + (ptrdiff_t)(y - T.y0) * T.stride - T.x0; }   /* (index it by x) */

/* The palettes (the order of PAL_...): day's, and night's, the same glass
 * smoke-tinted, light text on it, the same amber (SIEOS's). pal is the one
 * in use (draw_night). */
static const uint32_t pal_day[NPAL] = {
    0x34478f, FROST | ALPHA(23) | 0xf6f8fd, 0xb9c1d6, GLOSS | 0xffc93c, 0x1a2238, 0x4a5470, 0xd9e0f2, 0xfbfcff, 0x1a2238, 0xc8322a,
    0xbcd0f7, 0x3a4a8c, 0x2a1d00, 0xf3f5fa, 0xffffff, 0x2a3350, 0x8a5200,
    FROST | ALPHA(7) | 0xffffff, 0xffffff, 0xe2e8f8, ALPHA(7) | 0xffffff, ALPHA(8) | 0xffffff, ALPHA(5) | 0xffffff,
    FROST | ALPHA(21) | 0x0e1426, 0xc9d1ec };
static const uint32_t pal_night[NPAL] = {
    0x161d36, FROST | ALPHA(25) | 0x1b2135, 0x3b4460, GLOSS | 0xffc93c, 0xe4e8f4, 0x9aa3bf, 0x2d3552, 0x171c2c, 0xd6dcec, 0xff6b5e,
    0x34477d, 0x222b4d, 0x2a1d00, 0x1c2236, 0x10141f, 0x0a0e1a, 0xffc93c,
    FROST | ALPHA(12) | 0x0c1122, 0xf0f2f8, 0xa9b2cc, ALPHA(6) | 0xffffff, ALPHA(6) | 0xffffff, ALPHA(4) | 0xffffff,
    FROST | ALPHA(24) | 0x090c18, 0xc3cbe4 };
uint32_t pal[NPAL];
static int night;
#define DESK_MID pal[PAL_BG]        /* (the desk's middle: what solid glass is over) */

static int isqrt(unsigned v)
{
    unsigned r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    for (; b; b >>= 2)
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1;
    return (int)r;
}

static uint32_t mix(uint32_t a, uint32_t b, int k)   /* k/32 of the way from a to b */
{
    int r = (int)(a >> 16 & 255), g = (int)(a >> 8 & 255), bl = (int)(a & 255);
    r += ((int)(b >> 16 & 255) - r) * k >> 5; g += ((int)(b >> 8 & 255) - g) * k >> 5; bl += ((int)(b & 255) - bl) * k >> 5;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
}
uint32_t draw_shown(uint32_t c)     /* (glass: over the desk's middle blue) */
{
    return c & (FROST | ALPHA_M) ? mix(DESK_MID, c & 0xffffff, ALPHA_OF(c)) : c & 0xffffff;
}

/* ---- The desk's colours: a deep navy under five wide glows, all blues
 * (no rainbow), where (in thousandths of the screen's width and height),
 * how far (in thousandths of its width), each fading evenly to nothing,
 * the last on top. Worked out at 1/WG of a screen's size, for the size
 * last asked. */
static const struct { int x, y, r; uint32_t c, n; } lights[] = {   /* (c by day, n by night) */
    { 440, 460, 196, 0x7e8ff5, 0x2f3a78 }, { 60, 920, 419, 0x3d52c8, 0x1c2668 }, { 620, 880, 348, 0x2e6fd6, 0x173b7a },
    { 860, 260, 351, 0x6fa6f5, 0x2a4580 }, { 160, 200, 391, 0x4f74f0, 0x223680 } };
#define WG 16
static uint32_t *wall;
static int wcols, wrows, ww, wh;
static void wall_init(int w, int h)
{
    if (wall && w == ww && h == wh) return;
    uint32_t *nw = malloc((size_t)(w / WG + 2) * (h / WG + 2) * 4);
    if (!nw) return;
    free(wall);
    wall = nw; ww = w; wh = h; wcols = w / WG + 2; wrows = h / WG + 2;
    for (int j = 0; j < wrows; j++)
        for (int i = 0; i < wcols; i++) {
            int c[3] = { night ? 0x0b : 0x1c, night ? 0x0f : 0x26, night ? 0x1e : 0x47 };
            for (int k = 0; k < (int)(sizeof lights / sizeof *lights); k++) {
                int dx = i * WG - lights[k].x * w / 1000, dy = j * WG - lights[k].y * h / 1000, r = lights[k].r * w / 1000;
                int d = isqrt((unsigned)(dx * dx + dy * dy));
                if (d >= r) continue;
                int a = 256 - d * 256 / r;
                uint32_t lc = night ? lights[k].n : lights[k].c;
                for (int ch = 0; ch < 3; ch++) c[ch] += (((int)(lc >> (16 - 8 * ch)) & 255) - c[ch]) * a / 256;
            }
            wall[j * wcols + i] = (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | (uint32_t)c[2];
        }
}
/* Row y of the desk, pixels [lo, hi) (the desk's own coordinates), into d:
 * each pixel between the four nearest points of the 1/WG picture (down the
 * column once per WG pixels, then a step added per pixel along the row). */
static void wall_row(int y, int lo, int hi, uint32_t *d)
{
    if (y < 0) y = 0;
    if (y >= wh) y = wh - 1;
    const uint32_t *a = wall + (y / WG) * wcols, *b = a + wcols;
    int fy = y % WG;
    for (int x = lo; x < hi;) {
        int i = x / WG, end = (i + 1) * WG < hi ? (i + 1) * WG : hi, fx = x % WG, v[3], dv[3];
        for (int ch = 0, s = 16; ch < 3; ch++, s -= 8) {
            int l = (int)(a[i] >> s & 255) * (WG - fy) + (int)(b[i] >> s & 255) * fy;         /* (x WG) */
            int r = (int)(a[i + 1] >> s & 255) * (WG - fy) + (int)(b[i + 1] >> s & 255) * fy;
            v[ch] = l * (WG - fx) + r * fx; dv[ch] = r - l;                                   /* (x WG x WG) */
        }
        for (; x < end; x++, v[0] += dv[0], v[1] += dv[1], v[2] += dv[2])
            d[x] = 0xff000000u | (uint32_t)(v[0] >> 8) << 16 | (uint32_t)(v[1] >> 8) << 8 | (uint32_t)(v[2] >> 8);
    }
}

static int sc(int v) { return (int)(((int64_t)v * Z) >> 16); }                 /* a position */
static int sz(int v) { return v <= 0 ? 0 : (int)(((int64_t)v * Z + 32768) >> 16); }   /* a size, rounded */
int dsc(int v) { return sc(v); }
int dunit(int px)
{
    int v = (int)((((int64_t)px << 16) + Z - 1) / Z) - 1;
    while (sc(v) < px) v++;
    while (sc(v - 1) >= px) v--;
    return v;
}
static void relayout(void) { SW = (int)(((int64_t)PW << 16) / Z); SH = (int)(((int64_t)PH << 16) / Z); }
void draw_zoom(int z) { Z = z < 16384 ? 16384 : z > 262144 ? 262144 : z; relayout(); }
int  draw_zoom_get(void) { return Z; }
void draw_size(int pw, int ph) { PW = pw; PH = ph; relayout(); }
void draw_night(int on)
{
    night = on;
    memcpy(pal, on ? pal_night : pal_day, sizeof pal);
    ww = 0;                         /* (the desk worked out again) */
}
int  draw_night_get(void) { return night; }

void dl_reset(void) { np = 0; arena_n = 0; }
int  text_w(int n, int scale) { return (int)((int64_t)n * CW * (scale & ~SANS) >> 16); }

static int text_scale(int scale) { scale = (int)(((int64_t)(scale & ~SANS) * Z) >> 16); return scale < 4096 ? 4096 : scale; }
int text_sw(const char *s, int n, int scale)
{
    if (!(scale & SANS)) return text_w(n, scale);
    return n <= 0 ? 0 : (int)((sans_w(s, n, text_scale(scale)) + Z - 1) / Z);
}
int text_fit(const char *s, int n, int scale, int w)
{
    if (!(scale & SANS)) { int cw = text_w(1, scale), k = cw > 0 ? w / cw : n; return k < 0 ? 0 : k < n ? k : n; }
    return sans_fit(s, n, text_scale(scale), (int64_t)w * Z);   /* (screen pixels, 16.16: as text_sw) */
}

static prim_t *add(int type, int x0, int y0, int x1, int y1, uint32_t c)
{
    if (np == MAXP || x1 <= x0 || y1 <= y0) return 0;
    prim_t *p = &dl[np++];
    *p = (prim_t){ .type = type, .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .c = type == P_RECT ? c : type == P_FRAME ? c & ~(FROST | GLOSS) : c & ~(FROST | GLOSS | ALPHA_M) };   /* (glass, gloss:
                                       fills only; see-through: fills and outlines) */
    return p;
}

/* A box to the screen: a box at least a pixel wide stays one (a thin line
 * never vanishes when zoomed out). */
static void box(int *x0, int *y0, int *x1, int *y1)
{
    int a = sc(*x0), b = sc(*y0), c = sc(*x1), d = sc(*y1);
    if (c <= a && *x1 > *x0) c = a + 1;
    if (d <= b && *y1 > *y0) d = b + 1;
    *x0 = a; *y0 = b; *x1 = c; *y1 = d;
}

void dl_desk(int x0, int y0, int x1, int y1) { box(&x0, &y0, &x1, &y1); add(P_DESK, x0, y0, x1, y1, 0); }

void dl_rect(int x0, int y0, int x1, int y1, int r, uint32_t c)
{
    box(&x0, &y0, &x1, &y1);
    prim_t *p = add(P_RECT, x0, y0, x1, y1, c);
    if (p) p->r = sz(r);
}

void dl_glass(int x0, int y0, int x1, int y1, int r, uint32_t c, int hx0, int hy0, int hx1, int hy1)
{
    box(&x0, &y0, &x1, &y1);
    prim_t *p = add(P_RECT, x0, y0, x1, y1, c);
    if (!p) return;
    p->r = sz(r);
    if (hx1 > hx0 && hy1 > hy0) { box(&hx0, &hy0, &hx1, &hy1); p->cx0 = hx0; p->cy0 = hy0; p->cx1 = hx1; p->cy1 = hy1; }   /* (the hole) */
}

void dl_shade(int x0, int y0, int x1, int y1, int r, int k) { dl_shadow(x0, y0, x1, y1, r, k, 0, 0, 0, 0); }
void dl_shadow(int x0, int y0, int x1, int y1, int r, int k, int hx0, int hy0, int hx1, int hy1)
{
    box(&x0, &y0, &x1, &y1);
    prim_t *p = add(P_SHADE, x0, y0, x1, y1, 0);
    if (!p) return;
    p->r = sz(r); p->t = k;
    if (hx1 > hx0 && hy1 > hy0) { box(&hx0, &hy0, &hx1, &hy1); p->cx0 = hx0; p->cy0 = hy0; p->cx1 = hx1; p->cy1 = hy1; }   /* (the hole) */
}

void dl_frame(int x0, int y0, int x1, int y1, int r, int t, uint32_t c)
{
    box(&x0, &y0, &x1, &y1);
    prim_t *p = add(P_FRAME, x0, y0, x1, y1, c);
    if (p) { p->r = sz(r); p->t = t > 0 && sz(t) < 1 ? 1 : sz(t); }
}

void dl_text(int x, int y, const char *s, int n, int scale, uint32_t c, int cx0, int cy0, int cx1, int cy1)
{
    if (n <= 0 || arena_n + n > ARENA) return;
    int sans = !!(scale & SANS);
    x = sc(x); y = sc(y); scale = text_scale(scale);
    box(&cx0, &cy0, &cx1, &cy1);
    if (cx0 < x) cx0 = x;
    if (cy0 < y) cy0 = y;
    int x1 = x + (sans ? (int)((sans_w(s, n, scale) + 65535) >> 16) + 2 : text_w(n, scale)), y1 = y + (int)((int64_t)CH * scale >> 16);
    if (cx1 > x1) cx1 = x1;
    if (cy1 > y1) cy1 = y1;
    prim_t *p = add(P_TEXT, cx0, cy0, cx1, cy1, c);
    if (!p) return;
    p->cx0 = x; p->cy0 = y;          /* (for text, cx0/cy0 hold where the text starts) */
    p->scale = scale; p->len = n; p->text = arena_n; p->t = sans;
    memcpy(arena + arena_n, s, n);
    arena_n += n;
}

static void pixmap(int x, int y, const char *const *rows, int w, int h, const char *keys, const uint32_t *cols, int s)
{
    prim_t *p = add(P_PIXMAP, x, y, x + (int)(((int64_t)w * s + 65535) >> 16), y + (int)(((int64_t)h * s + 65535) >> 16), 0);
    if (p) { p->rows = rows; p->keys = keys; p->cols = cols; p->scale = s; p->r = w; p->t = h; }
}
void dl_pixmap(int x, int y, const char *const *rows, int w, int h, const char *keys, const uint32_t *cols, int scale)
{
    int s = (int)(((int64_t)scale * Z) < 16384 ? 16384 : (int64_t)scale * Z);   /* 16.16: screen pixels per map pixel */
    pixmap(sc(x), sc(y), rows, w, h, keys, cols, s);
}
void dl_pixmap_px(int x, int y, const char *const *rows, int w, int h, const char *keys, const uint32_t *cols, int px)
{
    pixmap(sc(x), sc(y), rows, w, h, keys, cols, (px < 1 ? 1 : px) << 16);
}

void dl_pic(int x0, int y0, int x1, int y1, const uint8_t *px, const uint32_t *pal_, int w, int h)
{
    box(&x0, &y0, &x1, &y1);
    prim_t *p = add(P_PIC, x0, y0, x1, y1, 0);
    if (p) { p->keys = (const char *)px; p->cols = pal_; p->r = w; p->t = h; }
}

/* How far row y of a box [y0, y1) with corner radius r starts inside. */
static int inset(int y, int y0, int y1, int r)
{
    if (r <= 0) return 0;
    int d = y < y0 + r ? y0 + r - y : y >= y1 - r ? y - (y1 - r) + 1 : 0;
    return d ? r - isqrt((unsigned)(r * r - (d - 1) * (d - 1))) : 0;
}

/* c (opaque) over o, a of 256 of it: each channel, alpha too (premultiplied). */
static uint32_t blend(uint32_t o, uint32_t c, int a)
{
    if (a >= 256) return c;
    return ((((o >> 24) * (256 - a) + (c >> 24) * a) >> 8) << 24) |
           ((((o >> 16 & 255) * (256 - a) + (c >> 16 & 255) * a) >> 8) << 16) |
           ((((o >> 8 & 255) * (256 - a) + (c >> 8 & 255) * a) >> 8) << 8) |
           (((o & 255) * (256 - a) + (c & 255) * a) >> 8);
}

/* Fill [x0, x1) of row y, clipped to [lo, hi) (see-through: over what is there). */
static void span(int y, int x0, int x1, int lo, int hi, uint32_t c)
{
    if (x0 < lo) x0 = lo;
    if (x1 > hi) x1 = hi;
    uint32_t *d = row(y), k = c & ALPHA_M ? (uint32_t)ALPHA_OF(c) * 8 : 256;
    c = 0xff000000u | (c & 0xffffff);
    if (k < 256) for (int x = x0; x < x1; x++) d[x] = blend(d[x], c, (int)k);
    else for (int x = x0; x < x1; x++) d[x] = c;
}

static uint32_t shine(uint32_t v, int l)   /* v lit: l/256 of the way to white */
{
    return (v & 0xff000000u) | ((v >> 16 & 255) + ((255 - (v >> 16 & 255)) * l >> 8)) << 16 |
           ((v >> 8 & 255) + ((255 - (v >> 8 & 255)) * l >> 8)) << 8 | ((v & 255) + ((255 - (v & 255)) * l >> 8));
}
/* Light on a glass or a gloss (filled first): its edge a little lit, a
 * bright line just inside its top (a gloss: its top row). Shining is a
 * straight line in what is under it, so it holds for glass's two drawings. */
static void lit(const prim_t *p, int lo, int hi, int ya, int yb)
{
    int fill = !(p->c & FROST), small = p->y1 - p->y0 < 6 || p->x1 - p->x0 < 6;
    for (int y = ya; y < yb; y++) {
        int i = inset(y, p->y0, p->y1, p->r), xl = p->x0 + i, xr = p->x1 - i - 1;
        int a = xl > lo ? xl : lo, b = xr + 1 < hi ? xr + 1 : hi, dt = y - p->y0;
        uint32_t *d = row(y);
        if (fill) for (int x = a; x < b; x++) d[x] = 0xff000000u | (p->c & 0xffffff);
        if (small || a >= b) continue;
        if (fill) { if (dt == 0) for (int x = a; x < b; x++) d[x] = shine(d[x], 150); continue; }
        int hole = p->cx1 > p->cx0 && y >= p->cy0 && y < p->cy1;
        if (dt == 0 || y == p->y1 - 1) {
            for (int x = a; x < b; x++) if (!hole || x < p->cx0 || x >= p->cx1) d[x] = shine(d[x], 120);
            continue;
        }
        if (dt == 1) for (int x = a + 1; x < b - 1; x++) d[x] = shine(d[x], 170);
        if (xl >= lo && xl < hi) d[xl] = shine(d[xl], 120);
        if (xr >= lo && xr < hi && xr != xl) d[xr] = shine(d[xr], 120);
    }
}
/* Glass: what it adds over what is behind it, V (black, white, or the
 * desk's middle: GLASS_...): V less its alpha, its tint by its alpha.
 * Not in its hole, which is left as it was. */
static void glass(const prim_t *p, int lo, int hi, int ya, int yb)
{
    uint32_t c = p->c & 0xffffff, V = T.glass == GLASS_LIGHT ? 0xffffff : T.glass == GLASS_SOLID ? DESK_MID : 0;
    int al = ALPHA_OF(p->c), hole = p->cx1 > p->cx0 && p->cy1 > p->cy0;
    uint32_t g = 0xff000000u;
    for (int s = 16; s >= 0; s -= 8) g |= (uint32_t)(((int)(V >> s & 255) * (32 - al) + (int)(c >> s & 255) * al) >> 5) << s;
    for (int y = ya; y < yb; y++) {
        int i = inset(y, p->y0, p->y1, p->r), a = p->x0 + i > lo ? p->x0 + i : lo, b = p->x1 - i < hi ? p->x1 - i : hi;
        uint32_t *d = row(y);
        for (int x = a; x < b; x++) {
            if (hole && y >= p->cy0 && y < p->cy1 && x >= p->cx0 && x < p->cx1) { x = p->cx1 - 1; continue; }
            d[x] = g;
        }
        if (a < b) T.drew = 1;
    }
    lit(p, lo, hi, ya, yb);
}

/* Rasterize primitive p in rows [ya0, yb0), columns [lo, hi). */
static void raster(const prim_t *p, int ya0, int yb0, int lo, int hi)
{
    int ya = p->y0 > ya0 ? p->y0 : ya0, yb = p->y1 < yb0 ? p->y1 : yb0;
    if (ya >= yb || p->x1 <= lo || p->x0 >= hi) return;
    switch (p->type) {
    case P_DESK:                     /* (its own coordinates: the screen's box) */
        wall_init(p->x1 - p->x0, p->y1 - p->y0);
        if (!wall) break;
        for (int y = ya; y < yb; y++) {
            int a = p->x0 > lo ? p->x0 : lo, b = p->x1 < hi ? p->x1 : hi;
            wall_row(y - p->y0, a - p->x0, b - p->x0, row(y) + p->x0);
        }
        break;
    case P_RECT:
        if (p->c & FROST) glass(p, lo, hi, ya, yb);
        else if (p->c & GLOSS) lit(p, lo, hi, ya, yb);
        else for (int y = ya; y < yb; y++) { int i = inset(y, p->y0, p->y1, p->r); span(y, p->x0 + i, p->x1 - i, lo, hi, p->c); }
        break;
    case P_FRAME: {
        int r = p->r, t = p->t, ir = r > t ? r - t : 0;
        for (int y = ya; y < yb; y++) {
            int i = inset(y, p->y0, p->y1, r);
            if (y < p->y0 + t || y >= p->y1 - t) { span(y, p->x0 + i, p->x1 - i, lo, hi, p->c); continue; }
            int j = inset(y, p->y0 + t, p->y1 - t, ir);
            span(y, p->x0 + i, p->x0 + t + j, lo, hi, p->c);
            span(y, p->x1 - t - j, p->x1 - i, lo, hi, p->c);
        }
        break;
    }
    case P_SHADE:                    /* darken what is there, or a shadow over nothing: black, 1/4 (k 1), 1/2 (k 2), 1/8 (k 3) */
        for (int y = ya; y < yb; y++) {
            int i = inset(y, p->y0, p->y1, p->r), a = p->x0 + i > lo ? p->x0 + i : lo, b = p->x1 - i < hi ? p->x1 - i : hi;
            int ha = b, hb = b;          /* (the hole: what the window covers, skipped) */
            if (p->cx1 > p->cx0 && y >= p->cy0 && y < p->cy1) { ha = p->cx0 > a ? p->cx0 : a; hb = p->cx1 > ha ? p->cx1 : ha; }
            uint32_t *d = row(y);
            for (int x = a; x < b; x++) {
                if (x >= ha && x < hb) { x = hb - 1; continue; }
                if (p->t == 3) d[x] = d[x] - (d[x] >> 3 & 0x1f1f1f1fu) + 0x20000000u;
                else if (p->t == 2) d[x] = (d[x] >> 1 & 0x7f7f7f7fu) + 0x80000000u;
                else d[x] = d[x] - (d[x] >> 2 & 0x3f3f3f3fu) + 0x40000000u;
            }
        }
        break;
    case P_PIXMAP:                   /* each screen pixel: its map character's colour */
        for (int y = ya; y < yb; y++) {
            int ry = (int)(((int64_t)(y - p->y0) << 16) / p->scale);
            if (ry >= p->t) continue;
            const char *rw = p->rows[ry];
            uint32_t *d = row(y);
            for (int x = p->x0 > lo ? p->x0 : lo; x < p->x1 && x < hi; x++) {
                int rx = (int)(((int64_t)(x - p->x0) << 16) / p->scale);
                if (rx >= p->r) break;
                char ch = rw[rx];
                if (ch == '.' || ch == ' ') continue;
                const char *k = strchr(p->keys, ch);
                if (k) d[x] = (p->cols[k - p->keys] & 0xffffff) | 0xff000000u;
            }
        }
        break;
    case P_PIC: {                    /* each screen pixel: between the four nearest picture pixels, laid over by their opacity */
        const uint8_t *ix = (const uint8_t *)p->keys;
        int a = p->x0 > lo ? p->x0 : lo, b = p->x1 < hi ? p->x1 : hi, w = p->r, h = p->t;
        int64_t ku = ((int64_t)w << 16) / (p->x1 - p->x0), kv = ((int64_t)h << 16) / (p->y1 - p->y0);   /* (picture pixels a screen pixel, 16.16) */
        for (int y = ya; y < yb; y++) {
            int64_t v = (y - p->y0) * kv + kv / 2 - 32768;
            int j = v < 0 ? 0 : (int)(v >> 16), fy = v < 0 ? 0 : (int)(v >> 12 & 15), j2 = j + 1 < h ? j + 1 : h - 1;
            if (j >= h) j = h - 1;
            const uint8_t *r0 = ix + j * w, *r1 = ix + j2 * w;
            uint32_t *d = row(y);
            for (int x = a; x < b; x++) {
                int64_t u = (x - p->x0) * ku + ku / 2 - 32768;
                int i = u < 0 ? 0 : (int)(u >> 16), fx = u < 0 ? 0 : (int)(u >> 12 & 15), i2 = i + 1 < w ? i + 1 : w - 1;
                if (i >= w) i = w - 1;
                uint32_t c[4] = { p->cols[r0[i]], p->cols[r0[i2]], p->cols[r1[i]], p->cols[r1[i2]] };
                int wt[4] = { (16 - fx) * (16 - fy), fx * (16 - fy), (16 - fx) * fy, fx * fy }, A = 0, R = 0, G = 0, B = 0;
                for (int k = 0; k < 4; k++) {           /* (weighed by their opacity too) */
                    int o = wt[k] * (int)(c[k] >> 24);
                    A += o; R += o * (int)(c[k] >> 16 & 255); G += o * (int)(c[k] >> 8 & 255); B += o * (int)(c[k] & 255);
                }
                if (!A) continue;
                d[x] = blend(d[x], 0xff000000u | (uint32_t)(R / A) << 16 | (uint32_t)(G / A) << 8 | (uint32_t)(B / A), A >> 8);
            }
        }
        break;
    }
    case P_TEXT: {                   /* each screen pixel: how much of it the font's pixels cover */
        int s = p->scale, cl = p->x0 > lo ? p->x0 : lo, cr = p->x1 < hi ? p->x1 : hi;
        uint32_t c = 0xff000000u | (p->c & 0xffffff);
        if (p->t) { sans_draw(T.px - T.x0, T.stride, T.y0, p->cx0, p->cy0, arena + p->text, p->len, s, c, cl, ya, cr, yb); break; }
        int nf = 0;
        while (nf < (int)(sizeof natives / sizeof *natives) && natives[nf].scale != s) nf++;
        if (nf < (int)(sizeof natives / sizeof *natives)) {   /* a native font, pixel for pixel (100%) */
            int fw = natives[nf].w, fh = natives[nf].h;
            const uint8_t *fp = natives[nf].px;
            for (int y = ya; y < yb; y++) {
                int rw = y - p->cy0;
                if (rw < 0 || rw >= fh) continue;
                uint32_t *d = row(y);
                for (int x = cl; x < cr; x++) {
                    int k = (x - p->cx0) / fw, col = (x - p->cx0) % fw;
                    if (k >= p->len) break;
                    unsigned ch = (uint8_t)arena[p->text + k];
                    int a = fp[((ch >= 32 && ch < 127 ? ch - 32 : '?' - 32) * fh + rw) * fw + col];
                    if (!a) continue;
                    a += a >> 7;                                 /* (0..255 -> 0..256) */
                    d[x] = blend(d[x], c, a);
                }
            }
            break;
        }
        int64_t step = ((int64_t)1 << 32) / s;          /* a screen pixel, in font pixels (16.16) */
        for (int y = ya; y < yb; y++) {
            int64_t v0 = ((int64_t)(y - p->cy0) << 32) / s, v1 = v0 + step;
            int r0 = (int)(v0 >> 16), r1 = (int)((v1 - 1) >> 16);
            if (r1 < 1 || r0 > 9) continue;              /* (font row 0 is blank; glyphs: rows 1..9) */
            uint32_t *d = row(y);
            int64_t u0 = ((int64_t)(cl - p->cx0) << 32) / s;
            for (int x = cl; x < cr; x++, u0 += step) {
                int64_t u1 = u0 + step, cov = 0;
                int c0 = (int)(u0 >> 16), c1 = (int)((u1 - 1) >> 16);
                for (int j = r0; j <= r1; j++) {
                    if (j < 1 || j > 9) continue;
                    int64_t oy = (v1 < ((int64_t)(j + 1) << 16) ? v1 : (int64_t)(j + 1) << 16) - (v0 > ((int64_t)j << 16) ? v0 : (int64_t)j << 16);
                    for (int i = c0; i <= c1; i++) {
                        int k = i / CW, b = i % CW;
                        if (k >= p->len || b >= 5) continue;
                        unsigned ch = (uint8_t)arena[p->text + k];
                        if (!(font[ch >= 32 && ch < 127 ? ch - 32 : '?' - 32][j - 1] & 0x10 >> b)) continue;
                        int64_t ox = (u1 < ((int64_t)(i + 1) << 16) ? u1 : (int64_t)(i + 1) << 16) - (u0 > ((int64_t)i << 16) ? u0 : (int64_t)i << 16);
                        cov += ox * oy >> 16;
                    }
                }
                if (!cov) continue;
                int a = (int)(cov * 256 / ((u1 - u0) * (v1 - v0) >> 16));   /* 0..256 */
                if (a >= 250) a = 256;
                d[x] = blend(d[x], c, a);
            }
        }
        break;
    }
    }
}

int dl_render(uint32_t *px, int stride, int x0, int y0, int w, int h, int glass_)
{
    T.px = px; T.stride = stride; T.x0 = x0; T.y0 = y0; T.w = w; T.h = h; T.glass = glass_; T.drew = 0;
    for (int y = 0; y < h; y++) memset(px + (size_t)y * stride, 0, (size_t)w * 4);
    for (int i = 0; i < np; i++) raster(&dl[i], y0, y0 + h, x0, x0 + w);
    return T.drew;
}
