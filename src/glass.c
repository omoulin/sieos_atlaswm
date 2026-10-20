/*
 * glass.c - The glass: what is behind it shows through, blurred and more
 * vivid, its tint laid over it (SIEOS's draw.c, as a compositor does it).
 *
 * The chrome draws its glass twice (draw.c): over black, A, its tint alone,
 * and over white, B, its tint and all the light it lets through. Whatever
 * is drawn over glass, the frame there is a straight line in what is
 * behind the glass, so output.c has the scene draw the frame both ways;
 * here, each pixel is
 *
 *     A + (B - A) x vivid(blur(what is behind that glass))
 *
 * B - A is how much of what is behind shows there: nothing where there is
 * no glass (A and B are the same), all of it where it is clear.
 *
 * What is behind depends on whose glass it is: a window's sees the desk
 * and the windows below it, not its own insides nor those in front; the
 * Deskbar's and the menus' see every window. So the glass seen on a screen
 * is cut into levels, each drawn here from the scene's buffers, back to
 * front, up to where its owner is (none of the glass: what is behind it):
 * the Deskbar and the menus; each window whose glass is near a window
 * below it (the frontmost few); and all the other windows', which see only
 * the desk. Where a level's glass is, is a mask drawn the same way, from
 * each piece's own (chrome.c): white for the level's, black for all other
 * glass over it. The blur is SIEOS's: what is behind at a quarter of its
 * size (each 4 x 4 pixels' mean), a tent 7 of those wide each way, across
 * then down, spread back between the four nearest; then 1.7 times as
 * saturated. Each level is drawn only around its glass.
 *
 * On the GPU: fragment shaders into textures of our own, then the output's
 * buffer. Under the pixman renderer: the same on the CPU.
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <math.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/swapchain.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/box.h>
#include <wlr/util/transform.h>
#include "atlas.h"

#define GQ 4                        /* the blur: at 1/GQ of the frame's size, */
#define GB 7                        /* a tent this many of those wide each way, */
#define GM 48                       /* so how far a glass pixel sees (GQ x (GB + 2), and some) */
#define MAXLV 8                     /* levels: the Deskbar and menus, the desk's, and up to 6 windows of their own */

/* ---- The levels of a frame. */
struct level { struct wlr_scene_node *stop; struct view *v; int top; pixman_box32_t box; };
static struct level lv[MAXLV];
static int nlv;
struct place { int ox, oy, tw, th; float scale; enum wl_output_transform tr; };   /* the output: its box, its buffer's size and turn */

static struct wlr_box to_buf(const struct place *p, int lx, int ly, int w, int h)   /* a box of the layout, in the buffer */
{
    int x0 = (int)lround((lx - p->ox) * p->scale), y0 = (int)lround((ly - p->oy) * p->scale);
    int x1 = (int)lround((lx + w - p->ox) * p->scale), y1 = (int)lround((ly + h - p->oy) * p->scale);
    struct wlr_box b = { x0, y0, x1 - x0, y1 - y0 };
    wlr_box_transform(&b, &b, wlr_output_transform_invert(p->tr), p->tw, p->th);
    return b;
}

/* The scene in order, from n until stop (-> 1 when it is reached), each
 * buffer and rectangle shown at its place in the layout. */
typedef void (*visit_fn)(struct wlr_scene_node *n, int lx, int ly, void *data);
static int walk(struct wlr_scene_node *n, int x, int y, struct wlr_scene_node *stop, visit_fn f, void *data)
{
    if (n == stop) return 1;
    if (!n->enabled) return 0;
    x += n->x; y += n->y;
    if (n->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_node *c;
        wl_list_for_each(c, &wlr_scene_tree_from_node(n)->children, link) if (walk(c, x, y, stop, f, data)) return 1;
    } else f(n, x, y, data);
    return 0;
}
static void node_size(struct wlr_scene_node *n, int *w, int *h)
{
    *w = *h = 0;
    if (n->type == WLR_SCENE_NODE_RECT) { struct wlr_scene_rect *r = wlr_scene_rect_from_node(n); *w = r->width; *h = r->height; return; }
    struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(n);
    if (sb->dst_width > 0 && sb->dst_height > 0) { *w = sb->dst_width; *h = sb->dst_height; }
    else if (sb->buffer) {
        int sw = sb->buffer->width, shh = sb->buffer->height;
        *w = sb->transform & WL_OUTPUT_TRANSFORM_90 ? shh : sw;
        *h = sb->transform & WL_OUTPUT_TRANSFORM_90 ? sw : shh;
    }
}

/* Is there a shown window below v near its glass? */
static int near_lower(struct view *v)
{
    int a0 = v->x0 - 80, b0 = v->y0 - TAB_H - 60, a1 = v->x1 + 80, b1 = v->y1 + 80;   /* (its glass, its shadow, what the blur sees) */
    struct view *u;
    wl_list_for_each(u, &srv.views, link) {
        if (u == v) break;
        if (!wm_shown(u) || u->fs) continue;
        if (u->x0 < a1 && u->x1 > a0 && u->y0 - TAB_H < b1 && u->y1 > b0) return 1;
    }
    return 0;
}

struct collect { const struct place *p; struct wlr_box ob; };
static void collect_one(struct wlr_scene_node *n, int lx, int ly, void *data)
{
    struct collect *cl = data;
    if (n->type != WLR_SCENE_NODE_BUFFER) return;
    struct chrome *c = chrome_at(n);
    if (!chrome_has_glass(c) || !c->shown) return;
    pixman_region32_t r;
    pixman_region32_init(&r);
    pixman_region32_intersect_rect(&r, &n->visible, cl->ob.x, cl->ob.y, cl->ob.width, cl->ob.height);
    int seen = pixman_region32_not_empty(&r);
    pixman_region32_fini(&r);
    c->level = -1;
    if (!seen) return;
    struct view *v = c->kind == CH_VIEW ? c->owner : 0;
    int k = 0, own = 0, nown = 0;
    for (; k < nlv && !(v ? lv[k].v == v : lv[k].top); k++) nown += lv[k].v != 0;   /* (its level: the top one, its window's, */
    if (k == nlv && v) {
        own = nown < MAXLV - 2 && near_lower(v);
        if (!own) for (k = 0; k < nlv && (lv[k].top || lv[k].v); k++) ;   /* or the desk's) */
    }
    if (k == nlv) {
        lv[nlv] = (struct level){ .v = own ? v : 0, .top = !v, .box = { 1 << 30, 1 << 30, -(1 << 30), -(1 << 30) } };
        lv[nlv].stop = !v ? &srv.l_bar->node : own ? &v->tree->node : &srv.l_wins->node;
        nlv++;
    }
    c->level = k;
    int w, h;
    node_size(n, &w, &h);
    struct wlr_box b = to_buf(cl->p, lx, ly, w, h);
    pixman_box32_t *x = &lv[k].box;
    if (b.x < x->x1) x->x1 = b.x;
    if (b.y < x->y1) x->y1 = b.y;
    if (b.x + b.width > x->x2) x->x2 = b.x + b.width;
    if (b.y + b.height > x->y2) x->y2 = b.y + b.height;
}

/* ---- Drawing a level's two pictures with the renderer: what is behind its
 * glass, and its mask. */
struct draw { const struct place *p; struct wlr_render_pass *pass; pixman_region32_t *clip; int level, mask; struct wlr_texture *tmp[64]; int ntmp; };
static void draw_one(struct wlr_scene_node *n, int lx, int ly, void *data)
{
    struct draw *d = data;
    int w, h;
    node_size(n, &w, &h);
    if (w <= 0 || h <= 0) return;
    struct wlr_box b = to_buf(d->p, lx, ly, w, h);
    if (n->type == WLR_SCENE_NODE_RECT) {
        if (d->mask) return;
        const float *c = wlr_scene_rect_from_node(n)->color;
        wlr_render_pass_add_rect(d->pass, &(struct wlr_render_rect_options){ .box = b, .clip = d->clip,
            .color = { c[0], c[1], c[2], c[3] }, .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED });
        return;
    }
    struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(n);
    struct chrome *c = chrome_at(n);
    if (d->mask) {                                       /* (glass only: the level's white, others' black) */
        if (!chrome_has_glass(c) || !c->shown || c->level < 0 || !c->mask[c->level != d->level]) return;
        wlr_render_pass_add_texture(d->pass, &(struct wlr_render_texture_options){ .texture = c->mask[c->level != d->level],
            .dst_box = b, .clip = d->clip, .transform = d->p->tr,
            .filter_mode = WLR_SCALE_FILTER_NEAREST, .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED });
        return;
    }
    if (chrome_has_glass(c)) return;                     /* (behind glass: none of it) */
    struct wlr_texture *t = sb->texture;
    if (!t && sb->buffer) {                              /* (one the scene has not drawn yet: a texture for now) */
        struct wlr_client_buffer *cb = wlr_client_buffer_get(sb->buffer);
        if (cb) t = cb->texture;
        else if (d->ntmp < 64 && (t = wlr_texture_from_buffer(srv.renderer, sb->buffer))) d->tmp[d->ntmp++] = t;
    }
    if (!t) return;
    enum wl_output_transform tr = wlr_output_transform_compose(wlr_output_transform_invert(sb->transform), d->p->tr);
    wlr_render_pass_add_texture(d->pass, &(struct wlr_render_texture_options){ .texture = t, .src_box = sb->src_box,
        .dst_box = b, .clip = d->clip, .transform = tr,
        .alpha = &sb->opacity, .filter_mode = sb->filter_mode, .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED });
}
static struct wlr_buffer *draw_level(struct output *o, const struct place *p, int k, int mask, const pixman_box32_t *area)
{
    struct wlr_buffer *buf = wlr_swapchain_acquire(o->off[2], NULL);
    if (!buf) return 0;
    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(srv.renderer, buf, NULL);
    if (!pass) { wlr_buffer_unlock(buf); return 0; }
    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, area->x1, area->y1, area->x2 - area->x1, area->y2 - area->y1);
    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){ .box = { 0, 0, buf->width, buf->height }, .clip = &clip,
        .color = { 0, 0, 0, mask ? 0 : 1 }, .blend_mode = WLR_RENDER_BLEND_MODE_NONE });
    struct draw d = { .p = p, .pass = pass, .clip = &clip, .level = k, .mask = mask };
    walk(&srv.scene->tree.node, 0, 0, mask ? 0 : lv[k].stop, draw_one, &d);
    bool ok = wlr_render_pass_submit(pass);
    for (int i = 0; i < d.ntmp; i++) wlr_texture_destroy(d.tmp[i]);
    pixman_region32_fini(&clip);
    if (!ok) { wlr_buffer_unlock(buf); return 0; }
    return buf;
}

/* ---- On the GPU. */
static const char vs_src[] =
    "attribute vec2 pos;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char down_body[] =                     /* (4 x 4 pixels' mean: four taps between four pixels each) */
    "uniform vec2 px;\n"
    "void main() {\n"
    "    vec2 c = gl_FragCoord.xy * 4.0 * px;\n"
    "    gl_FragColor = vec4(0.25 * (texture2D(tex, c - px).rgb + texture2D(tex, c + px).rgb +\n"
    "                                texture2D(tex, c + vec2(px.x, -px.y)).rgb + texture2D(tex, c + vec2(-px.x, px.y)).rgb), 1.0);\n"
    "}\n";
static const char tent_body[] =                     /* (one way: weights 8 - |k| of 64) */
    "uniform vec2 size, dir;\n"
    "void main() {\n"
    "    vec2 c = gl_FragCoord.xy / size;\n"
    "    vec3 s = vec3(0.0);\n"
    "    for (int k = -7; k <= 7; k++) s += (8.0 - abs(float(k))) * texture2D(tex, c + float(k) * dir).rgb;\n"
    "    gl_FragColor = vec4(s / 64.0, 1.0);\n"
    "}\n";
static const char copy_body[] =
    "uniform vec2 size;\n"
    "void main() { gl_FragColor = vec4(texture2D(tex, gl_FragCoord.xy / size).rgb, 1.0); }\n";
static const char add_body[] =                      /* ((B - A) x the mask x vivid(behind), added) */
    "uniform sampler2D_B texb, mask;\n"
    "uniform sampler2D blur;\n"
    "uniform vec2 size, qsize;\n"
    "void main() {\n"
    "    vec2 c = gl_FragCoord.xy / size;\n"
    "    vec3 v = texture2D(blur, gl_FragCoord.xy / (4.0 * qsize)).rgb;\n"
    "    v = clamp(v + (v - (v.r + v.g + v.b) / 3.0) * 0.7, 0.0, 1.0);\n"
    "    gl_FragColor = vec4((texture2D(texb, c).rgb - texture2D(tex, c).rgb) * texture2D(mask, c).r * v, 1.0);\n"
    "}\n";
enum { PR_DOWN, PR_TENT, PR_COPY, PR_ADD, NPR };
struct prog { GLuint p; GLint tex, texb, mask, blur, px, size, qsize, dir; };
static struct prog progs[NPR][2];                   /* [which][external: the frames' textures] */
static int compiled, has_ext;
struct glass_gl { GLuint tex[2], fbo[2]; int qw, qh; };   /* per output: the blur, a quarter of the size: across, then down */

static GLuint shader(GLenum type, const char *const *src, int n)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, n, src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        say("glass: shader: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}
static void link_prog(struct prog *pr, int external, const char *body)
{
    static const char prec[] =                       /* (highp where there is: a 4K frame's pixels) */
        "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n";
    const char *head = external ? "#define sampler2D_B samplerExternalOES\nuniform samplerExternalOES tex;\n"
                                : "#define sampler2D_B sampler2D\nuniform sampler2D tex;\n";
    const char *fs[] = { external ? "#extension GL_OES_EGL_image_external : require\n" : "", prec, head, body }, *vs[] = { vs_src };
    GLuint v = shader(GL_VERTEX_SHADER, vs, 1), f = shader(GL_FRAGMENT_SHADER, fs, 4);
    if (!v || !f) return;
    pr->p = glCreateProgram();
    glAttachShader(pr->p, v); glAttachShader(pr->p, f);
    glBindAttribLocation(pr->p, 0, "pos");
    glLinkProgram(pr->p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok;
    glGetProgramiv(pr->p, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(pr->p); pr->p = 0; return; }
    pr->tex = glGetUniformLocation(pr->p, "tex");
    pr->texb = glGetUniformLocation(pr->p, "texb");
    pr->mask = glGetUniformLocation(pr->p, "mask");
    pr->blur = glGetUniformLocation(pr->p, "blur");
    pr->px = glGetUniformLocation(pr->p, "px");
    pr->size = glGetUniformLocation(pr->p, "size");
    pr->qsize = glGetUniformLocation(pr->p, "qsize");
    pr->dir = glGetUniformLocation(pr->p, "dir");
}

static void quarter(struct glass_gl *g, int w, int h)   /* (its two textures, for this frame's size) */
{
    int qw = (w + GQ - 1) / GQ, qh = (h + GQ - 1) / GQ;
    if (g->tex[0] && g->qw == qw && g->qh == qh) return;
    if (!g->tex[0]) { glGenTextures(2, g->tex); glGenFramebuffers(2, g->fbo); }
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, g->tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, qw, qh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, g->fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->tex[i], 0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    g->qw = qw; g->qh = qh;
}
static void source(GLenum target, GLuint tex, int unit, GLint filter)
{
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(target, tex);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
static void quad(void)
{
    static const GLfloat q[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, q);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
}

/* Our GL between the renderer's passes: its context, then back as it was. */
static struct { EGLDisplay dpy, pdpy; EGLContext pctx; EGLSurface pdraw, pread; } cx;
static int gl_begin(void)
{
    struct wlr_egl *egl = wlr_gles2_renderer_get_egl(srv.renderer);
    cx.dpy = wlr_egl_get_display(egl); cx.pdpy = eglGetCurrentDisplay(); cx.pctx = eglGetCurrentContext();
    cx.pdraw = eglGetCurrentSurface(EGL_DRAW); cx.pread = eglGetCurrentSurface(EGL_READ);
    if (!eglMakeCurrent(cx.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, wlr_egl_get_context(egl))) return 0;
    if (!compiled) {
        static const char *const bodies[NPR] = { down_body, tent_body, copy_body, add_body };
        has_ext = wlr_gles2_renderer_check_ext(srv.renderer, "GL_OES_EGL_image_external");
        for (int k = 0; k < NPR; k++) {
            link_prog(&progs[k][0], 0, bodies[k]);
            if (k != PR_TENT && has_ext) link_prog(&progs[k][1], 1, bodies[k]);
        }
        compiled = 1;
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    return 1;
}
static void gl_end(void)
{
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(0);
    for (int i = 3; i >= 0; i--) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (has_ext) glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (cx.pctx != EGL_NO_CONTEXT) eglMakeCurrent(cx.pdpy, cx.pdraw, cx.pread, cx.pctx);
    else eglMakeCurrent(cx.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}
static int ext(struct wlr_gles2_texture_attribs *at) { return at->target == GL_TEXTURE_EXTERNAL_OES; }

static bool gpu(struct output *o, const struct place *p, struct wlr_buffer *a, struct wlr_buffer *b, struct wlr_buffer *dst)
{
    struct wlr_renderer *r = srv.renderer;
    struct wlr_texture *ta = wlr_texture_from_buffer(r, a), *tb = wlr_texture_from_buffer(r, b);
    GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(r, dst);
    bool ok = false;
    if (!o->glass) o->glass = calloc(1, sizeof *o->glass);
    if (!ta || !tb || !fbo || !o->glass || !gl_begin()) goto out;
    struct wlr_gles2_texture_attribs aa, ab;
    wlr_gles2_texture_get_attribs(ta, &aa);
    wlr_gles2_texture_get_attribs(tb, &ab);
    struct prog *cp = &progs[PR_COPY][ext(&aa)];
    if (!cp->p || ext(&aa) != ext(&ab)) { gl_end(); goto out; }
    struct glass_gl *g = o->glass;
    int w = dst->width, h = dst->height;
    quarter(g, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);              /* the frame: A, */
    glViewport(0, 0, w, h);
    glUseProgram(cp->p);
    source(aa.target, aa.tex, 0, GL_NEAREST);
    glUniform1i(cp->tex, 0);
    glUniform2f(cp->size, (GLfloat)w, (GLfloat)h);
    quad();
    gl_end();
    ok = true;
    for (int k = 0; k < nlv; k++) {                      /* then each level's light through its glass */
        pixman_box32_t in = lv[k].box, around = { in.x1 - GM, in.y1 - GM, in.x2 + GM, in.y2 + GM };
        struct wlr_buffer *back = draw_level(o, p, k, 0, &around), *mask = back ? draw_level(o, p, k, 1, &in) : 0;
        struct wlr_texture *tk = back ? wlr_texture_from_buffer(r, back) : 0, *tm = mask ? wlr_texture_from_buffer(r, mask) : 0;
        if (tk && tm && gl_begin()) {
            struct wlr_gles2_texture_attribs ak, am;
            wlr_gles2_texture_get_attribs(tk, &ak);
            wlr_gles2_texture_get_attribs(tm, &am);
            struct prog *dn = &progs[PR_DOWN][ext(&ak)], *tn = &progs[PR_TENT][0], *ad = &progs[PR_ADD][ext(&aa)];
            if (dn->p && tn->p && ad->p && ext(&am) == ext(&aa)) {
                glBindFramebuffer(GL_FRAMEBUFFER, g->fbo[0]);   /* behind, at a quarter of its size, */
                glViewport(0, 0, g->qw, g->qh);
                glUseProgram(dn->p);
                source(ak.target, ak.tex, 0, GL_LINEAR);
                glUniform1i(dn->tex, 0);
                glUniform2f(dn->px, 1.0f / w, 1.0f / h);
                quad();
                glUseProgram(tn->p);                     /* blurred across, then down, */
                glUniform1i(tn->tex, 0);
                glUniform2f(tn->size, (GLfloat)g->qw, (GLfloat)g->qh);
                for (int i = 0; i < 2; i++) {
                    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo[!i]);
                    source(GL_TEXTURE_2D, g->tex[i], 0, GL_NEAREST);
                    glUniform2f(tn->dir, i ? 0.0f : 1.0f / g->qw, i ? 1.0f / g->qh : 0.0f);
                    quad();
                }
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);  /* added where its glass is */
                glViewport(0, 0, w, h);
                glEnable(GL_SCISSOR_TEST);
                glScissor(in.x1, in.y1, in.x2 - in.x1, in.y2 - in.y1);
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE);
                glUseProgram(ad->p);
                source(aa.target, aa.tex, 0, GL_NEAREST);
                source(ab.target, ab.tex, 1, GL_NEAREST);
                source(am.target, am.tex, 2, GL_NEAREST);
                source(GL_TEXTURE_2D, g->tex[0], 3, GL_LINEAR);
                glUniform1i(ad->tex, 0);
                glUniform1i(ad->texb, 1);
                glUniform1i(ad->mask, 2);
                glUniform1i(ad->blur, 3);
                glUniform2f(ad->size, (GLfloat)w, (GLfloat)h);
                glUniform2f(ad->qsize, (GLfloat)g->qw, (GLfloat)g->qh);
                quad();
            }
            gl_end();
        }
        if (tk) wlr_texture_destroy(tk);
        if (tm) wlr_texture_destroy(tm);
        if (back) wlr_buffer_unlock(back);
        if (mask) wlr_buffer_unlock(mask);
    }
    if (gl_begin()) { glFinish(); gl_end(); }            /* (done before the output takes the buffer) */
out:
    if (ta) wlr_texture_destroy(ta);
    if (tb) wlr_texture_destroy(tb);
    return ok;
}

/* ---- On the CPU (the pixman renderer). */
struct frame { uint8_t *p; size_t stride; int bgr; };
static int open_frame(struct wlr_buffer *b, struct frame *f, uint32_t flags)
{
    void *p;
    uint32_t fmt;
    if (!wlr_buffer_begin_data_ptr_access(b, flags, &p, &fmt, &f->stride)) return 0;
    f->p = p;
    f->bgr = fmt == DRM_FORMAT_XBGR8888 || fmt == DRM_FORMAT_ABGR8888;
    return 1;
}
static inline uint32_t *row_of(const struct frame *f, int y) { return (uint32_t *)(f->p + (size_t)y * f->stride); }
static inline uint32_t rgb(const struct frame *f, uint32_t c) { return f->bgr ? (c & 0xff00) | (c & 255) << 16 | (c >> 16 & 255) : c & 0xffffff; }
static void tent(const uint32_t *s, uint32_t *d, int n, int step)   /* n values step apart, blurred */
{
    for (int i = 0; i < n; i++) {
        int r = 0, g = 0, b = 0;
        for (int k = -GB; k <= GB; k++) {
            int j = i + k < 0 ? 0 : i + k >= n ? n - 1 : i + k, w = GB + 1 - (k < 0 ? -k : k);
            uint32_t c = s[j * step];
            r += (c >> 16 & 255) * w; g += (c >> 8 & 255) * w; b += (c & 255) * w;
        }
        d[i * step] = (uint32_t)(r >> 6) << 16 | (uint32_t)(g >> 6) << 8 | (uint32_t)(b >> 6);
    }
}
/* dst += (B - A) x mask x vivid(blur(back)) in the box `in` (back holds `around` it) */
static void add_level(struct frame *D, struct frame *A, struct frame *B, struct frame *K, struct frame *M,
                      pixman_box32_t in, pixman_box32_t around)
{
    int i0 = around.x1 / GQ, j0 = around.y1 / GQ, qw = (around.x2 - 1) / GQ + 1 - i0, qh = (around.y2 - 1) / GQ + 1 - j0;
    if (qw <= 0 || qh <= 0) return;
    uint32_t *q = malloc((size_t)qw * qh * 8), *q2 = q ? q + (size_t)qw * qh : 0;
    if (!q) return;
    for (int j = 0; j < qh; j++)                         /* behind, at a quarter of its size */
        for (int i = 0; i < qw; i++) {
            int r = 0, g = 0, bl = 0, n = 0;
            for (int y = (j0 + j) * GQ; y < (j0 + j + 1) * GQ && y < around.y2; y++)
                for (int x = (i0 + i) * GQ; x < (i0 + i + 1) * GQ && x < around.x2; x++, n++) {
                    uint32_t c = rgb(K, row_of(K, y)[x]);
                    r += c >> 16 & 255; g += c >> 8 & 255; bl += c & 255;
                }
            q[j * qw + i] = n ? (uint32_t)(r / n) << 16 | (uint32_t)(g / n) << 8 | (uint32_t)(bl / n) : 0;
        }
    for (int j = 0; j < qh; j++) tent(q + j * qw, q2 + j * qw, qw, 1);   /* across, */
    for (int i = 0; i < qw; i++) tent(q2 + i, q + i, qh, qw);             /* then down */
    for (int y = in.y1; y < in.y2; y++) {
        uint32_t *d = row_of(D, y), *a = row_of(A, y), *b = row_of(B, y), *m = row_of(M, y);
        int v8 = 2 * y - GQ + 1, j = (v8 >> 3) - j0, fy = v8 & 7, j2 = j + 1;   /* (between block rows j and j + 1) */
        j = j < 0 ? 0 : j >= qh ? qh - 1 : j; j2 = j2 >= qh ? qh - 1 : j2 < 0 ? 0 : j2;
        const uint32_t *r0 = q + j * qw, *r1 = q + j2 * qw;
        for (int x = in.x1; x < in.x2; x++) {
            uint32_t ca = rgb(A, a[x]), cb = rgb(B, b[x]);
            if (ca == cb || !(rgb(M, m[x]) & 0x800000)) continue;   /* (no glass of this level here) */
            int u8 = 2 * x - GQ + 1, k = (u8 >> 3) - i0, fx = u8 & 7, k2 = k + 1;
            k = k < 0 ? 0 : k >= qw ? qw - 1 : k; k2 = k2 >= qw ? qw - 1 : k2 < 0 ? 0 : k2;
            uint32_t p00 = r0[k], p01 = r0[k2], p10 = r1[k], p11 = r1[k2], cd = rgb(D, d[x]), out = 0;
            int w00 = (8 - fx) * (8 - fy), w01 = fx * (8 - fy), w10 = (8 - fx) * fy, w11 = fx * fy, v[3];
            for (int c = 0, s = 16; c < 3; c++, s -= 8)
                v[c] = (int)((p00 >> s & 255) * w00 + (p01 >> s & 255) * w01 + (p10 >> s & 255) * w10 + (p11 >> s & 255) * w11) >> 6;
            int mid = (v[0] + v[1] + v[2]) / 3;
            for (int c = 0, s = 16; c < 3; c++, s -= 8) {
                int vv = v[c] + (v[c] - mid) * 7 / 10;   /* (vivid: 1.7 times as saturated) */
                vv = vv < 0 ? 0 : vv > 255 ? 255 : vv;
                int o_ = (int)(cd >> s & 255) + ((int)(cb >> s & 255) - (int)(ca >> s & 255)) * vv / 255;
                out |= (uint32_t)(o_ > 255 ? 255 : o_) << s;
            }
            d[x] = 0xff000000u | (D->bgr ? (out & 0xff00) | (out & 255) << 16 | (out >> 16 & 255) : out);
        }
    }
    free(q);
}
static bool cpu(struct output *o, const struct place *p, struct wlr_buffer *a, struct wlr_buffer *b, struct wlr_buffer *dst)
{
    struct frame A, B, D;
    if (!open_frame(a, &A, WLR_BUFFER_DATA_PTR_ACCESS_READ)) return false;
    if (!open_frame(b, &B, WLR_BUFFER_DATA_PTR_ACCESS_READ)) { wlr_buffer_end_data_ptr_access(a); return false; }
    if (!open_frame(dst, &D, WLR_BUFFER_DATA_PTR_ACCESS_READ | WLR_BUFFER_DATA_PTR_ACCESS_WRITE)) {
        wlr_buffer_end_data_ptr_access(b); wlr_buffer_end_data_ptr_access(a);
        return false;
    }
    for (int y = 0; y < dst->height; y++)                /* the frame: A, */
        for (int x = 0; x < dst->width; x++) {
            uint32_t c = rgb(&A, row_of(&A, y)[x]);
            row_of(&D, y)[x] = 0xff000000u | (D.bgr ? (c & 0xff00) | (c & 255) << 16 | (c >> 16 & 255) : c);
        }
    for (int k = 0; k < nlv; k++) {                      /* then each level's light through its glass */
        pixman_box32_t in = lv[k].box, around = { in.x1 - GM, in.y1 - GM, in.x2 + GM, in.y2 + GM };
        if (around.x1 < 0) around.x1 = 0;
        if (around.y1 < 0) around.y1 = 0;
        if (around.x2 > dst->width) around.x2 = dst->width;
        if (around.y2 > dst->height) around.y2 = dst->height;
        struct wlr_buffer *back = draw_level(o, p, k, 0, &around), *mask = back ? draw_level(o, p, k, 1, &in) : 0;
        struct frame K, M;
        if (mask && open_frame(back, &K, WLR_BUFFER_DATA_PTR_ACCESS_READ)) {
            if (open_frame(mask, &M, WLR_BUFFER_DATA_PTR_ACCESS_READ)) {
                add_level(&D, &A, &B, &K, &M, in, around);
                wlr_buffer_end_data_ptr_access(mask);
            }
            wlr_buffer_end_data_ptr_access(back);
        }
        if (back) wlr_buffer_unlock(back);
        if (mask) wlr_buffer_unlock(mask);
    }
    wlr_buffer_end_data_ptr_access(dst);
    wlr_buffer_end_data_ptr_access(b);
    wlr_buffer_end_data_ptr_access(a);
    return true;
}

bool glass_render(struct output *o, struct wlr_buffer *a, struct wlr_buffer *b, struct wlr_buffer *dst)
{
    if (!wlr_output_configure_primary_swapchain(o->wlr, NULL, &o->off[2])) return false;
    struct place p = { .scale = o->wlr->scale, .tr = o->wlr->transform };
    struct wlr_box ob;
    wlr_output_layout_get_box(srv.layout, o->wlr, &ob);
    p.ox = ob.x; p.oy = ob.y;
    wlr_output_transformed_resolution(o->wlr, &p.tw, &p.th);
    nlv = 0;
    struct collect cl = { .p = &p, .ob = ob };
    walk(&srv.scene->tree.node, 0, 0, 0, collect_one, &cl);
    for (int k = 0; k < nlv; k++) {                      /* (each level's glass, on the buffer) */
        pixman_box32_t *x = &lv[k].box;
        if (x->x1 < 0) x->x1 = 0;
        if (x->y1 < 0) x->y1 = 0;
        if (x->x2 > dst->width) x->x2 = dst->width;
        if (x->y2 > dst->height) x->y2 = dst->height;
        if (x->x2 <= x->x1 || x->y2 <= x->y1) *x = (pixman_box32_t){ 0, 0, 0, 0 };
    }
    return wlr_renderer_is_gles2(srv.renderer) ? gpu(o, &p, a, b, dst) : cpu(o, &p, a, b, dst);
}

void glass_fini(struct output *o)                       /* (the output goes; its textures with it) */
{
    struct glass_gl *g = o->glass;
    o->glass = 0;
    if (!g) return;
    if (g->tex[0] && wlr_renderer_is_gles2(srv.renderer) && gl_begin()) {
        glDeleteFramebuffers(2, g->fbo);
        glDeleteTextures(2, g->tex);
        gl_end();
    }
    free(g);
}
