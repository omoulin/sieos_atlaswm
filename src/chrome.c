/*
 * chrome.c - The pieces of the desktop drawn on the CPU (draw.c's display
 * list: a window's frame and tab, the Deskbar, a menu, the desk), each a
 * buffer of the scene in memory, and the click boxes that say what a
 * press on one means.
 *
 * A piece takes the pointer only where it has a click box: a window's
 * shadow lets it through to what is behind.
 *
 * A piece with glass in it has two buffers, one above the other: its
 * glass over black (shown) and over white (hidden). The compositor shows
 * one, then the other, to see the frame each way (chrome_glass); glass.c
 * also has masks of where its glass is.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include "atlas.h"

/* ---- A buffer in memory: ARGB8888, premultiplied. */
struct pixbuf { struct wlr_buffer base; uint32_t *px; };
static void pb_destroy(struct wlr_buffer *b) { struct pixbuf *p = wl_container_of(b, p, base); free(p->px); free(p); }
static bool pb_access(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format, size_t *stride)
{
    (void)flags;
    struct pixbuf *p = wl_container_of(b, p, base);
    *data = p->px; *format = DRM_FORMAT_ARGB8888; *stride = (size_t)b->width * 4;
    return true;
}
static void pb_end(struct wlr_buffer *b) { (void)b; }
static const struct wlr_buffer_impl pb_impl = { .destroy = pb_destroy, .begin_data_ptr_access = pb_access, .end_data_ptr_access = pb_end };

struct wlr_buffer *pixbuf_create(int w, int h)
{
    struct pixbuf *p = calloc(1, sizeof *p);
    if (!p) return 0;
    p->px = calloc((size_t)w * h, 4);
    if (!p->px) { free(p); return 0; }
    wlr_buffer_init(&p->base, &pb_impl, w, h);
    return &p->base;
}
uint32_t *pixbuf_data(struct wlr_buffer *b, int *stride) { struct pixbuf *p = wl_container_of(b, p, base); *stride = b->width; return p->px; }

static bool no_input(struct wlr_scene_buffer *sb, double *sx, double *sy) { (void)sb; (void)sx; (void)sy; return false; }

struct wlr_scene_buffer *solid_create(struct wlr_scene_tree *parent, uint32_t argb, bool input)
{
    struct wlr_buffer *b = pixbuf_create(1, 1);
    int st;
    *pixbuf_data(b, &st) = argb;
    struct wlr_scene_buffer *sb = wlr_scene_buffer_create(parent, b);
    wlr_buffer_drop(b);
    if (!input) sb->point_accepts_input = no_input;
    return sb;
}
void solid_color(struct wlr_scene_buffer *sb, uint32_t argb)
{
    struct wlr_buffer *b = pixbuf_create(1, 1);
    int st;
    *pixbuf_data(b, &st) = argb;
    wlr_scene_buffer_set_buffer(sb, b);
    wlr_buffer_drop(b);
}
void solid_place(struct wlr_scene_buffer *sb, int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0) { wlr_scene_node_set_enabled(&sb->node, false); return; }
    wlr_scene_node_set_position(&sb->node, L(x0), L(y0));
    wlr_scene_buffer_set_dest_size(sb, L(x1) - L(x0), L(y1) - L(y0));
    wlr_scene_node_set_enabled(&sb->node, true);
}

/* ---- Chrome. */
static bool chrome_input(struct wlr_scene_buffer *sb, double *sx, double *sy)
{
    struct chrome *c = sb->node.data;
    if (!c || !c->hits) return true;
    int x = dunit(c->x0 + P(*sx)), y = dunit(c->y0 + P(*sy));
    return hits_at(c->hits, x, y) != 0;
}

static struct wl_list glasses = { &glasses, &glasses };   /* the pieces with glass in them */

void chrome_init(struct chrome *c, struct wlr_scene_tree *parent, int kind, void *owner, hits_t *hits)
{
    *c = (struct chrome){ .kind = kind, .owner = owner, .hits = hits };
    wl_list_init(&c->glink);
    c->sb = wlr_scene_buffer_create(parent, NULL);
    c->sb->node.data = c;
    c->sb->point_accepts_input = chrome_input;
    wlr_scene_node_set_enabled(&c->sb->node, false);
}

static struct wlr_buffer *render(int x0, int y0, int w, int h, int glass, int *drew, uint32_t **px)
{
    int st = 0;
    struct wlr_buffer *b = pixbuf_create(w, h);
    if (b) { *px = pixbuf_data(b, &st); *drew = dl_render(*px, st, x0, y0, w, h, glass); }
    return b;
}
static void show(struct wlr_scene_buffer *sb, struct wlr_buffer *b, int x0, int y0, int x1, int y1)
{
    wlr_scene_buffer_set_buffer(sb, b);
    wlr_buffer_drop(b);
    wlr_scene_buffer_set_dest_size(sb, L(x1) - L(x0), L(y1) - L(y0));
    wlr_scene_node_set_position(&sb->node, L(x0), L(y0));
}
static void masks_free(struct chrome *c)
{
    for (int i = 0; i < 2; i++) {
        if (c->mask[i]) wlr_texture_destroy(c->mask[i]);
        if (c->mbuf[i]) wlr_buffer_drop(c->mbuf[i]);
        c->mask[i] = 0; c->mbuf[i] = 0;
    }
}
/* Where its glass is (it differs over black and over white): white there,
 * and black there, for glass.c's masks. */
static void masks_make(struct chrome *c, const uint32_t *a, const uint32_t *b, int w, int h)
{
    masks_free(c);
    for (int i = 0; i < 2; i++) {
        int st;
        if (!(c->mbuf[i] = pixbuf_create(w, h))) return;
        uint32_t *m = pixbuf_data(c->mbuf[i], &st), on = i ? 0xff000000u : 0xffffffffu;
        for (int k = 0; k < w * h; k++) m[k] = a[k] != b[k] ? on : 0;
        c->mask[i] = wlr_texture_from_buffer(srv.renderer, c->mbuf[i]);
    }
}
void chrome_draw(struct chrome *c, int x0, int y0, int x1, int y1)
{
    if (!c->sb) return;
    if (x1 <= x0 || y1 <= y0) { chrome_hide(c); return; }
    int w = x1 - x0, h = y1 - y0, glass = 0, dummy;
    uint32_t *pa = 0, *pb = 0;
    struct wlr_buffer *b = render(x0, y0, w, h, GLASS_TINT, &glass, &pa), *lb;
    if (!b) return;
    c->x0 = x0; c->y0 = y0; c->w = w; c->h = h;
    if (glass && (lb = render(x0, y0, w, h, GLASS_LIGHT, &dummy, &pb))) {   /* (over white too: hidden but for glass.c) */
        masks_make(c, pa, pb, w, h);
        if (!c->lit) {
            c->lit = wlr_scene_buffer_create(c->sb->node.parent, NULL);
            c->lit->point_accepts_input = no_input;
            wlr_scene_node_place_above(&c->lit->node, &c->sb->node);
            wlr_scene_node_set_enabled(&c->lit->node, false);
        }
        show(c->lit, lb, x0, y0, x1, y1);
        if (wl_list_empty(&c->glink)) wl_list_insert(&glasses, &c->glink);
    } else if (!wl_list_empty(&c->glink)) {
        wl_list_remove(&c->glink);
        wl_list_init(&c->glink);
        masks_free(c);
        if (c->lit) wlr_scene_node_set_enabled(&c->lit->node, false);
    }
    show(c->sb, b, x0, y0, x1, y1);
    wlr_scene_node_set_enabled(&c->sb->node, true);
    c->shown = 1;
}
void chrome_hide(struct chrome *c)
{
    if (c->sb) wlr_scene_node_set_enabled(&c->sb->node, false);
    if (c->lit) wlr_scene_node_set_enabled(&c->lit->node, false);
    c->shown = 0;
}
void chrome_fini(struct chrome *c)
{
    if (!wl_list_empty(&c->glink)) { wl_list_remove(&c->glink); wl_list_init(&c->glink); }
    masks_free(c);
    if (c->lit) wlr_scene_node_destroy(&c->lit->node);
    if (c->sb) wlr_scene_node_destroy(&c->sb->node);
    c->sb = 0; c->lit = 0;
}
int chrome_has_glass(struct chrome *c) { return c && !wl_list_empty(&c->glink); }

/* The pieces with glass, as glass.c wants them for each of its frames:
 * over black (GLASS_TINT, as they are shown) or over white (GLASS_LIGHT). */
void chrome_glass(int glass)
{
    struct chrome *c;
    wl_list_for_each(c, &glasses, glink) {
        if (!c->shown) continue;
        wlr_scene_node_set_enabled(&c->sb->node, glass == GLASS_TINT);
        wlr_scene_node_set_enabled(&c->lit->node, glass == GLASS_LIGHT);
    }
}
/* Is any glass seen on this box of the layout? */
int chrome_glass_in(struct wlr_box b)
{
    struct chrome *c;
    wl_list_for_each(c, &glasses, glink) {
        int lx, ly;
        if (!c->shown || !wlr_scene_node_coords(&c->sb->node, &lx, &ly)) continue;
        pixman_region32_t r;
        pixman_region32_init(&r);
        pixman_region32_intersect_rect(&r, &c->sb->node.visible, b.x, b.y, b.width, b.height);
        int seen = pixman_region32_not_empty(&r);
        pixman_region32_fini(&r);
        if (seen) return 1;
    }
    return 0;
}

struct chrome *chrome_at(struct wlr_scene_node *node)
{
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return 0;
    struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
    return sb->point_accepts_input == chrome_input ? node->data : 0;
}

