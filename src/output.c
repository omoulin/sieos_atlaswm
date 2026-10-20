/*
 * output.c - The screens. With no glass in sight, the scene draws straight
 * to the screen. With glass, it draws two frames into buffers of our own:
 * the glass over black and over white (chrome.c); glass.c makes the
 * screen's frame from them and from what is behind each piece of glass.
 *
 * Screens are arranged by wlr-output-management (wlr-randr, kanshi), side
 * by side at first; wlr-output-power-management turns them off (swayidle).
 *
 * Part of atlas_wm. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <pixman.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include "atlas.h"

static struct wlr_output_manager_v1 *om;
static struct wl_listener om_apply, om_test, power_set;

struct output *output_primary(void)
{
    if (wl_list_empty(&srv.outputs)) return 0;
    struct output *o = wl_container_of(srv.outputs.next, o, link);
    return o;
}

struct output *output_of(struct wlr_output *wo)
{
    struct output *o;
    wl_list_for_each(o, &srv.outputs, link) if (o->wlr == wo) return o;
    return 0;
}
struct output *output_at(double lx, double ly)          /* (the screen there, or the nearest) */
{
    struct wlr_output *wo = wlr_output_layout_output_at(srv.layout, lx, ly);
    if (!wo) {
        double cx, cy;
        wlr_output_layout_closest_point(srv.layout, NULL, lx, ly, &cx, &cy);
        wo = wlr_output_layout_output_at(srv.layout, cx, cy);
    }
    return wo ? output_of(wo) : output_primary();
}
struct output *output_of_box(box_t b) { return output_at(L(dsc((b.x0 + b.x1) / 2)), L(dsc((b.y0 + b.y1) / 2))); }

/* ---- The glass's frames. Showing one or the other glass damages the scene
 * (and so asks for a frame) on every screen; but each frame of ours goes to
 * a swapchain of its own, which always sees the glass the same way, so
 * that damage is not news to any of them: it is taken back. */
#define MAXSO 16
static void glass_set(int glass)
{
    pixman_region32_t cur[MAXSO], pend[MAXSO];
    struct wlr_scene_output *so;
    int n = 0;
    wl_list_for_each(so, &srv.scene->outputs, link) {
        if (n == MAXSO) break;
        pixman_region32_init(&cur[n]); pixman_region32_copy(&cur[n], &so->damage_ring.current);
        pixman_region32_init(&pend[n]); pixman_region32_copy(&pend[n], &so->pending_commit_damage);
        n++;
    }
    chrome_glass(glass);
    int k = 0;
    wl_list_for_each(so, &srv.scene->outputs, link) {
        if (k == n) break;
        pixman_region32_copy(&so->damage_ring.current, &cur[k]); pixman_region32_fini(&cur[k]);
        pixman_region32_copy(&so->pending_commit_damage, &pend[k]); pixman_region32_fini(&pend[k]);
        k++;
    }
}

static void frame(struct wl_listener *l, void *data)
{
    (void)data;
    struct output *o = wl_container_of(l, o, frame);
    struct timespec now;
    struct wlr_box box;
    wlr_output_layout_get_box(srv.layout, o->wlr, &box);
    if (!chrome_glass_in(box)) {
        wlr_scene_output_commit(o->so, NULL);
        goto done;
    }
    if (!o->wlr->needs_frame && !pixman_region32_not_empty(&o->so->pending_commit_damage)) goto done;
    struct wlr_output_state st[2];
    static const int how[2] = { GLASS_TINT, GLASS_LIGHT };   /* A, B */
    int ok = 1;
    for (int k = 0; k < 2; k++) wlr_output_state_init(&st[k]);
    for (int k = 0; k < 2 && ok; k++) {
        glass_set(how[k]);
        struct wlr_scene_output_state_options opt = { .swapchain = 0 };
        ok = wlr_output_configure_primary_swapchain(o->wlr, NULL, &o->off[k]);
        opt.swapchain = o->off[k];
        ok = ok && wlr_scene_output_build_state(o->so, &st[k], &opt) && (st[k].committed & WLR_OUTPUT_STATE_BUFFER);
    }
    glass_set(GLASS_TINT);                               /* (as the scene shows it) */
    if (ok) {
        struct wlr_buffer *dst = 0;
        if (wlr_output_configure_primary_swapchain(o->wlr, &st[0], &o->wlr->swapchain))
            dst = wlr_swapchain_acquire(o->wlr->swapchain, NULL);
        if (dst) {
            if (glass_render(o, st[0].buffer, st[1].buffer, dst)) {
                pixman_region32_t all;                   /* (all of it is new: glass sees around what changed) */
                pixman_region32_init_rect(&all, 0, 0, dst->width, dst->height);
                wlr_output_state_set_damage(&st[0], &all);
                pixman_region32_fini(&all);
                wlr_output_state_set_buffer(&st[0], dst);
                wlr_output_commit_state(o->wlr, &st[0]);
            }
            wlr_buffer_unlock(dst);
        }
    }
    for (int k = 0; k < 2; k++) wlr_output_state_finish(&st[k]);
done:
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(o->so, &now);
}

static void request_state(struct wl_listener *l, void *data)   /* (a nested window resized) */
{
    struct output *o = wl_container_of(l, o, request_state);
    const struct wlr_output_event_request_state *e = data;
    wlr_output_commit_state(o->wlr, e->state);
    bar_layout();
}

static void destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct output *o = wl_container_of(l, o, destroy);
    wl_list_remove(&o->frame.link);
    wl_list_remove(&o->request_state.link);
    wl_list_remove(&o->destroy.link);
    wl_list_remove(&o->link);
    layers_output_gone(o);
    struct view *v;
    wl_list_for_each(v, &srv.views, link) if (v->ftl_out == o->wlr) v->ftl_out = 0;
    chrome_fini(&o->desk);
    chrome_fini(&o->lockc);
    if (o->lockbg) wlr_scene_node_destroy(&o->lockbg->node);
    for (int k = 0; k < 3; k++) if (o->off[k]) wlr_swapchain_destroy(o->off[k]);
    glass_fini(o);
    free(o);
    bar_layout();
}

static void new_output(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_output *wo = data;
    wlr_output_init_render(wo, srv.allocator, srv.renderer);
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    struct wlr_output_mode *m = wlr_output_preferred_mode(wo);
    if (m) wlr_output_state_set_mode(&st, m);
    settings_output(wo, &st);                            /* (the mode and scale kept for it) */
    if (!wlr_output_commit_state(wo, &st) && m) {        /* (a kept mode it cannot take: its own) */
        wlr_output_state_finish(&st);
        wlr_output_state_init(&st);
        wlr_output_state_set_enabled(&st, true);
        wlr_output_state_set_mode(&st, m);
        wlr_output_commit_state(wo, &st);
    }
    wlr_output_state_finish(&st);

    struct output *o = calloc(1, sizeof *o);
    o->wlr = wo;
    o->frame.notify = frame;
    wl_signal_add(&wo->events.frame, &o->frame);
    o->request_state.notify = request_state;
    wl_signal_add(&wo->events.request_state, &o->request_state);
    o->destroy.notify = destroy;
    wl_signal_add(&wo->events.destroy, &o->destroy);
    wl_list_insert(srv.outputs.prev, &o->link);
    chrome_init(&o->desk, srv.l_desk, CH_DESK, 0, 0);
    chrome_init(&o->lockc, srv.l_lock, CH_LOCK, 0, 0);
    o->so = wlr_scene_output_create(srv.scene, wo);
    struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(srv.layout, wo);
    wlr_scene_output_layout_add_output(srv.scene_layout, lo, o->so);
    say("output %s: %d x %d, scale %.2f", wo->name, wo->width, wo->height, wo->scale);
    wlr_output_layout_get_box(srv.layout, wo, &o->usable);
    lock_output(o);
    bar_layout();
}

void output_damage_all(void)
{
    struct output *o;
    wl_list_for_each(o, &srv.outputs, link) wlr_output_schedule_frame(o->wlr);
}

/* ---- wlr-output-management: the screens as they are, and a client's new
 * arrangement (modes, positions, which are on) applied. */
static void heads_update(void)
{
    struct wlr_output_configuration_v1 *c = wlr_output_configuration_v1_create();
    struct output *o;
    wl_list_for_each(o, &srv.outputs, link) {
        struct wlr_output_configuration_head_v1 *h = wlr_output_configuration_head_v1_create(c, o->wlr);
        struct wlr_box b;
        wlr_output_layout_get_box(srv.layout, o->wlr, &b);
        h->state.enabled = o->wlr->enabled && !wlr_box_empty(&b);
        h->state.x = b.x; h->state.y = b.y;
    }
    wlr_output_manager_v1_set_configuration(om, c);
}
static void om_do(struct wlr_output_configuration_v1 *c, int test)
{
    int ok = 1;
    struct wlr_output_configuration_head_v1 *h;
    wl_list_for_each(h, &c->heads, link) {
        struct wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_head_v1_state_apply(&h->state, &st);
        ok &= test ? wlr_output_test_state(h->state.output, &st) : wlr_output_commit_state(h->state.output, &st);
        wlr_output_state_finish(&st);
        if (test) continue;
        if (h->state.enabled) wlr_output_layout_add(srv.layout, h->state.output, h->state.x, h->state.y);
        else wlr_output_layout_remove(srv.layout, h->state.output);
    }
    if (ok) wlr_output_configuration_v1_send_succeeded(c);
    else wlr_output_configuration_v1_send_failed(c);
    wlr_output_configuration_v1_destroy(c);
    if (!test) { struct output *o; wl_list_for_each(o, &srv.outputs, link) layers_arrange(o); bar_layout(); }
}
static void on_om_apply(struct wl_listener *l, void *data) { (void)l; om_do(data, 0); }
static void on_om_test(struct wl_listener *l, void *data) { (void)l; om_do(data, 1); }

static void on_power(struct wl_listener *l, void *data)   /* (a screen off and on again: swayidle, wlopm) */
{
    (void)l;
    struct wlr_output_power_v1_set_mode_event *e = data;
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, e->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
    wlr_output_commit_state(e->output, &st);
    wlr_output_state_finish(&st);
}

static void layout_change(struct wl_listener *l, void *data)
{
    (void)l; (void)data;
    struct output *o;
    wl_list_for_each(o, &srv.outputs, link) layers_arrange(o);
    bar_layout();
    heads_update();
}

void output_init(void)
{
    wl_list_init(&srv.outputs);
    srv.layout = wlr_output_layout_create(srv.display);
    srv.scene_layout = wlr_scene_attach_output_layout(srv.scene, srv.layout);
    srv.new_output.notify = new_output;
    wl_signal_add(&srv.backend->events.new_output, &srv.new_output);
    srv.output_layout_change.notify = layout_change;
    wl_signal_add(&srv.layout->events.change, &srv.output_layout_change);
    om = wlr_output_manager_v1_create(srv.display);
    om_apply.notify = on_om_apply; wl_signal_add(&om->events.apply, &om_apply);
    om_test.notify = on_om_test; wl_signal_add(&om->events.test, &om_test);
    struct wlr_output_power_manager_v1 *pm = wlr_output_power_manager_v1_create(srv.display);
    power_set.notify = on_power; wl_signal_add(&pm->events.set_mode, &power_set);
}
