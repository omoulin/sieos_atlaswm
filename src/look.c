/*
 * look.c - The click boxes and text helpers atlas_wm and its apps share
 * (look.h).
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "look.h"

/* ---- Click boxes. */
static hits_t *cur;
void hits_begin(hits_t *hs) { cur = hs; hs->n = 0; }
void hitbox(int x0, int y0, int x1, int y1, int kind, int arg)
{
    if (!cur) return;
    if (cur->n == cur->cap) {
        int cap = cur->cap ? cur->cap * 2 : 32;
        hit_t *h = realloc(cur->h, cap * sizeof *h);
        if (!h) return;
        cur->h = h; cur->cap = cap;
    }
    cur->h[cur->n++] = (hit_t){ x0, y0, x1, y1, kind, arg };
}
const hit_t *hits_at(const hits_t *hs, int x, int y)
{
    for (int i = hs->n - 1; i >= 0; i--)
        if (x >= hs->h[i].x0 && x < hs->h[i].x1 && y >= hs->h[i].y0 && y < hs->h[i].y1) return &hs->h[i];
    return 0;
}

/* ---- The palette: day, night, or auto (night from 19:00 to 7:00, by the
 * local clock). */
int look_night(int m)
{
    time_t t = time(0);
    struct tm tm;
    localtime_r(&t, &tm);
    return m == 1 || (m == 2 && (tm.tm_hour < 7 || tm.tm_hour >= 19));
}

/* ---- Text helpers. */
void label(int x, int y, const char *s, int k, uint32_t c) { dl_text(x, y, s, strlen(s), k > 3 ? S(k) : SF(k), c, -(1 << 20), -(1 << 20), 1 << 20, 1 << 20); }
void bold(int x, int y, const char *s, uint32_t c, int cx1)   /* (twice, a pixel apart) */
{
    dl_text(x, y, s, strlen(s), SF(2), c, -(1 << 20), -(1 << 20), cx1, 1 << 20);
    dl_text(x + 1, y, s, strlen(s), SF(2), c, -(1 << 20), -(1 << 20), cx1, 1 << 20);
}

/* ---- Text for the font, which is ASCII (titles, file names): accents are dropped, dashes and quotes
 * made plain, anything else one '?'. */
void ascii(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n + 1 < cap) {
        unsigned c = *p++;
        if (c < 0x80) { out[n++] = c < 32 ? ' ' : (char)c; continue; }
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        unsigned u = c & (0x3f >> more);
        for (int i = 0; i < more && (*p & 0xc0) == 0x80; i++) u = u << 6 | (*p++ & 0x3f);
        static const char lat[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
        char r = '?';
        if (u >= 0xc0 && u <= 0xff) r = lat[u - 0xc0];
        else if (u == 0x2013 || u == 0x2014 || u == 0x2212) r = '-';
        else if (u == 0x2018 || u == 0x2019) r = '\'';
        else if (u == 0x201c || u == 0x201d) r = '"';
        else if (u == 0x2026) r = '.';
        else if (u == 0xa0 || u == 0x2009 || u == 0x202f) r = ' ';
        else if (u == 0xb7 || u == 0x2022) r = '*';
        out[n++] = r;
    }
    out[n] = 0;
}
