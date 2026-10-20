/*
 * icons.c - The desktop's icons: one family of pixel maps, 16 x 16, in a
 * box 12 x scale wide (24 at 2: the Deskbar, menus, tabs), each map pixel
 * whole screen pixels, so they stay sharp at every zoom.
 * The SIEOS logo is a picture (logo.c, made from logo-glass.svg by
 * tools/mklogo.py), smoothly scaled. From SIEOS's user/atlas/icons.c,
 * with a globe for web browsers.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <string.h>
#include <strings.h>
#include "look.h"

extern const uint32_t logo_pal[2][64];                  /* logo.c (tools/mklogo.py) */
extern const uint8_t logo32[32 * 32], logo64[64 * 64];
void logo(int x, int y, int scale)                       /* (16 x scale wide: the picture nearest that on the screen) */
{
    int n = 16 * scale, big = (int)(((int64_t)n * draw_zoom_get()) >> 16) > 40;
    dl_pic(x, y, x + n, y + n, big ? logo64 : logo32, logo_pal[big], big ? 64 : 32, big ? 64 : 32);
}

/* One family on a 16 x 16 grid: an ink outline (#), white faces (w), one
 * amber (a) or blue (b) fill (SIEOS's maps; the globe is atlas_wm's). */
static const char *const maps[I_N][16] = {
    [I_TERM] = {
        "................", ".##############.", ".#wwwwwwwwwwww#.", ".##############.",
        ".#bbbbbbbbbbbb#.", ".#bwbbbbbbbbbb#.", ".#bbwbbbbbbbbb#.", ".#bbbwbbbbbbbb#.",
        ".#bbwbbbbbbbbb#.", ".#bwbbbaaaabbb#.", ".#bbbbbbbbbbbb#.", ".#bbbbbbbbbbbb#.",
        ".##############.", "................", "................", "................" },
    [I_SIA] = {
        "................", "...##########...", "..#wwwwwwwwww#..", ".#wwwwwwwwwwww#.",
        ".#wwwwwwawwwww#.", ".#wwwwwaaawwww#.", ".#wwwwaaaaawww#.", ".#wwwwwaaawwww#.",
        ".#wwwwwwawwwww#.", "..#wwwwwwwwww#..", "...####ww####...", "......#w#.......",
        ".....#w#........", ".....##.........", "................", "................" },
    [I_FOLDER] = {
        "................", "................", "..#####.........", ".#aaaaa#........",
        ".#aaaaaa######..", ".#aaaaaaaaaaaa#.", ".##############.", ".#wwwwwwwwwwww#.",
        ".#aaaaaaaaaaaa#.", ".#aaaaaaaaaaaa#.", ".#aaaaaaaaaaaa#.", ".#aaaaaaaaaaaa#.",
        ".#aaaaaaaaaaaa#.", ".##############.", "................", "................" },
    [I_FILE] = {
        "................", "..#########.....", "..#wwwwwww##....", "..#wwwwwww#w#...",
        "..#wwwwwww####..", "..#wwwwwwwwww#..", "..#w#####wwww#..", "..#wwwwwwwwww#..",
        "..#w#######ww#..", "..#wwwwwwwwww#..", "..#w####aaaww#..", "..#wwwwwwwwww#..",
        "..#wwwwwwwwww#..", "..############..", "................", "................" },
    [I_SETTINGS] = {
        "................", "................", "....###.........", ".###aaa########.",
        "....###.........", "................", ".........###....", ".########aaa###.",
        ".........###....", "................", "......###.......", ".#####bbb######.",
        "......###.......", "................", "................", "................" },
    [I_APP] = {
        "................", ".......##.......", ".....##bb##.....", "...##bbbbbb##...",
        ".##wwbbbbbbww##.", ".#wwwwbbbbwwww#.", ".#wwwwwwwwwwww#.", ".#bbwwwaawwwbb#.",
        ".#bbbwwaawwbbb#.", ".#bbbbb##bbbbb#.", ".#bbbbb##bbbbb#.", ".##bbbb##bbbb##.",
        "...##bb##bb##...", ".....######.....", ".......##.......", "................" },
    [I_WINDOW] = {
        "................", ".#######........", ".#aaaaa#........", ".##############.",
        ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.",
        ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.",
        ".#wwwwwwwwwwww#.", ".##############.", "................", "................" },
    [I_PINS] = {
        "................", "...#.#.#.#.#....", "...#.#.#.#.#....", ".#############..",
        ".#bbbbbbbbbbb#..", ".#bbbbbbbbbbb#..", ".#bbaabbbbbbb#..", ".#bbaabbbbbbb#..",
        ".#bbbbbbbbbbb#..", ".#bbbbbbbbbbb#..", ".#############..", "...#.#.#.#.#....",
        "...#.#.#.#.#....", "................", "................", "................" },
    [I_GIT] = {
        "................", "...##...........", "..#aa#..........", "..#aa#....##....",
        "...##....#bb#...", "...#.....#bb#...", "...#......##....", "...#.......#....",
        "...#......#.....", "...#....##......", "...#..##........", "...###..........",
        "...##...........", "..#aa#..........", "..#aa#..........", "...##..........." },
    [I_MAIL] = {
        "................", "................", "................", ".##############.",
        ".##wwwwwwwwww##.", ".#w#wwwwwwww#w#.", ".#ww#wwwwww#ww#.", ".#www#wwww#www#.",
        ".#wwww#aa#wwww#.", ".#wwwwwaawwwww#.", ".#wwwwwwwwwwww#.", ".#wwwwwwwwwwww#.",
        ".##############.", "................", "................", "................" },
    [I_CAM] = {
        "................", "................", "....#####.......", ".##############.",
        ".#wwwwwwwwwwaa#.", ".#wwww####wwww#.", ".#www#bbbb#www#.", ".#ww#bbwbbb#ww#.",
        ".#ww#bbbbbb#ww#.", ".#ww#bbbbbb#ww#.", ".#www#bbbb#www#.", ".#wwww####wwww#.",
        ".#wwwwwwwwwwww#.", ".##############.", "................", "................" },
    [I_WEB] = {
        "................", ".....######.....", "...##bbwwbb##...", "..#bbbwbbwbbb#..",
        ".#bbbwbbbbwbbb#.", ".#wwwwwwwwwwww#.", ".#bbbwbbbbwbbb#.", ".#bbbwbbbbwbbb#.",
        ".#bbbwbbbbwbbb#.", ".#bbbwbbbbwbbb#.", ".#wwwwwwwwwwww#.", ".#bbbwbbbbwbbb#.",
        "..#bbbwbbwbbb#..", "...##bbwwbb##...", ".....######.....", "................" },
};
/* the palette of all of them */
static const char keys[] = "#wab";
static const uint32_t cols[] = { 0x1b2240, 0xffffff, 0xf6b93b, 0x5b7ef2 };

/* In the box of 12 x scale (as the callers lay it out), centred: each map
 * pixel whole screen pixels, as many as fill the box best, so it stays
 * sharp at every zoom. */
static void put(int kind, int x, int y, int scale, const char *k, const uint32_t *c)
{
    if (kind < 0 || kind >= I_N) return;
    int z = draw_zoom_get(), box = 12 * scale;
    int n = (int)((((int64_t)box * z >> 16) + 8) / 16);  /* (screen pixels a map pixel: the nearest) */
    if (n < 1) n = 1;
    int off = (box - (int)(((int64_t)(16 * n) << 16) / z)) / 2;
    dl_pixmap_px(x + off, y + off, maps[kind], 16, 16, k, c, n);
}
void icon(int kind, int x, int y, int scale) { put(kind, x, y, scale, keys, cols); }

/* Its shape only, in one ink (the faces left out): the ink on the accent
 * (an icon on a window's tab), or white (on clear glass). */
static const char ink_keys[] = "#ab";
static uint32_t ink_cols[2][3];                          /* (C_INK, C_GTXT: the look's; set when drawn) */
void icon_ink(int kind, int x, int y, int scale, int white)
{
    for (int i = 0; i < 3; i++) ink_cols[white != 0][i] = white ? C_GTXT : C_INK;
    put(kind, x, y, scale, ink_keys, ink_cols[white != 0]);
}

static const struct { const char *name; int icon; } names[] = {
    { "terminal", I_TERM }, { "assistant", I_SIA }, { "folder", I_FOLDER }, { "files", I_FOLDER },
    { "file", I_FILE }, { "editor", I_FILE }, { "settings", I_SETTINGS }, { "app", I_APP },
    { "window", I_WINDOW }, { "pins", I_PINS }, { "git", I_GIT }, { "mail", I_MAIL },
    { "camera", I_CAM }, { "web", I_WEB }, { "browser", I_WEB },
};
const char *icon_name(int icon)
{
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (names[i].icon == icon) return names[i].name;
    return "app";
}
int icon_by_name(const char *name)
{
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcasecmp(name, names[i].name)) return names[i].icon;
    return -1;
}

/* An app's icon, guessed from words in its id ("org.gnome.Nautilus": a folder). */
int icon_for_app(const char *id)
{
    static const struct { const char *word; int icon; } guess[] = {
        { "term", I_TERM }, { "foot", I_TERM }, { "kitty", I_TERM }, { "alacritty", I_TERM }, { "konsole", I_TERM },
        { "console", I_TERM }, { "xterm", I_TERM }, { "wezterm", I_TERM }, { "ghostty", I_TERM },
        { "nautilus", I_FOLDER }, { "thunar", I_FOLDER }, { "dolphin", I_FOLDER }, { "nemo", I_FOLDER },
        { "files", I_FOLDER }, { "pcmanfm", I_FOLDER },
        { "edit", I_FILE }, { "gedit", I_FILE }, { "kate", I_FILE }, { "code", I_FILE }, { "vim", I_FILE },
        { "emacs", I_FILE }, { "writer", I_FILE }, { "document", I_FILE }, { "evince", I_FILE }, { "papers", I_FILE },
        { "settings", I_SETTINGS }, { "control", I_SETTINGS }, { "tweaks", I_SETTINGS }, { "pavucontrol", I_SETTINGS },
        { "firefox", I_WEB }, { "chrom", I_WEB }, { "browser", I_WEB }, { "epiphany", I_WEB }, { "web", I_WEB },
        { "librewolf", I_WEB }, { "brave", I_WEB }, { "qutebrowser", I_WEB },
        { "mail", I_MAIL }, { "thunderbird", I_MAIL }, { "evolution", I_MAIL }, { "geary", I_MAIL },
        { "git", I_GIT }, { "cheese", I_CAM }, { "camera", I_CAM }, { "snapshot", I_CAM },
    };
    char low[128];
    size_t n = 0;
    for (; id[n] && n < sizeof low - 1; n++) low[n] = (char)(id[n] >= 'A' && id[n] <= 'Z' ? id[n] + 32 : id[n]);
    low[n] = 0;
    for (size_t i = 0; i < sizeof guess / sizeof *guess; i++) if (strstr(low, guess[i].word)) return guess[i].icon;
    return I_WINDOW;
}
