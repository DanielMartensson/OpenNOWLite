/*
 * opennow-lite.c - OpenNOWLite: its own Wayland window, fed by GStreamer, with
 * its own keyboard and pointer pipeline.
 *
 * The window is an xdg-shell toplevel created here, not by the video sink and
 * not by opennow-qt. waylandsink draws into that surface: the wl_display goes
 * to the sink through GstWlDisplayHandleContextType and the surface through the
 * GstVideoOverlay window handle - the same hand-off this layer uses for YtGst -
 * so the picture fills this program's window while this program keeps the
 * seat's keyboard and pointer events on the same connection.
 *
 * Stage 1 of this recipe played a test pattern, so the window and the input
 * path could be proven with no session at all; the same switches still do that.
 * With a launch id the media is the native streamer's: OpenNOWLite asks the
 * shipped opennow-core for the launch context, the streamer carries the NVST
 * connection, the input protocol and the encoded H.264 and Opus packets, which
 * are tapped out of it already encoded and fed to this program's own pipeline,
 * and the seat's input is submitted back through
 * opennow_streamer_submit_key / opennow_streamer_submit_mouse_*.
 *
 * By default the window asks for no system cursor: weston's desktop-shell
 * draws its arrow above every window and the streamed picture already carries
 * the game's own pointer, so the arrow was that pointer's laggier duplicate.
 * Asking for none maps the desktop's sprite away while the seat's pointer is
 * inside the game, which leaves exactly one pointer; --show-cursor keeps the
 * arrow for a screen that is meant to show both.
 *
 * Markers (grep -a):
 *   opennowlite: window ready
 *   opennowlite: pipeline playing                          (test pattern)
 *   opennowlite-input: key ...
 *   opennowlite-input: pointer ...
 *   opennowlite: pipeline error
 *   opennowlite: rpc <method> line=<n>                     (one per core RPC)
 *   opennowlite: reply for <id> in <n> ms                  (RPC round trip)
 *   opennowlite: reply for <id> not received within <n>s   (core went silent)
 *   opennowlite: launch <method> <code>                    (core RPC refusal)
 *   opennowlite: launch inspect reply=<n> decision=<n> scope=<n> status=<s>
 *   opennowlite: session create reply=<n> named=<0/1> phase=<s>
 *   opennowlite: poll attempt=<n> reply=<n> session=<0/1> status=<n> phase=<s> rtsps=<n>
 *   opennowlite: polling the allocation up to <n> times     (the wait budget)
 *   opennowlite: session ready for <id>                    (launch)
 *   opennowlite: engine started for <id>                   (streamer up)
 *   opennowlite: encoded video unit 1 / encoded audio packet 1
 *   opennowlite: video units=N queued=N / audio packets=N queued=N (media tally
 *                                                  with the appsrc's own queued
 *                                                  bytes, every 600 units /
 *                                                  1500 packets)
 *   opennowlite: video refused=N / audio refused=N                 (units the
 *                                                  appsrc would not take)
 *   opennowlite-input: pointer enter x= y= surface=         (the cursor reached
 *                                                    this window)
 *   opennowlite-input: pointer motion x= y=                (rate-limited)
 *   opennowlite-input: pointer button=<evdev> state=<0/1>
 *   opennowlite-input: pointer outside the window ignored  (the seat's cursor
 *                                                   was over another window)
 *   opennowlite-pilot: press N vk=0x.. at +Ns due=+Ns      (auto-pilot press)
 *   opennowlite-pilot: jiggle N x= y= at +Ns due=+Ns       (auto-pilot keep move)
 *   opennowlite: recording N s of picture into FILE        (--record on)
 *   opennowlite: recorded bytes=N units_left=N             (every 50 s of picture)
 *   opennowlite: record closed bytes=N                     (the witness file)
 *   opennowlite-audio: witness frames=N bytes=N             (--debug-audio)
 *   opennowlite-audio: witness closed frames=N             (the audio witness)
 *   opennowlite-input: <kind> submit no=N at +Nms took N us (--debug-input)
 *   opennowlite-input: key code=<n> evdev=<n> state=<0/1>   (each key the seat sends)
 *   opennowlite-input: <kind> submit refused (<n>)          (first refused input call)
 *   opennowlite: stopping keys=N motions=N submits=N refused=N skipped=N pilot=N
 */

#define OPENNOWLITE_FFI_LIBRARY "/usr/lib/opennowlite/libopennowlite_streamer_ffi.so"
#define OPENNOWLITE_CORE_PATH "/usr/bin/opennow-core"
#define OPENNOWLITE_SESSION_TIMEOUT 25
/* A reply is waited for as long as the thing asked for can take. The core's
 * own startup (credential store, Electron account migration) runs before it
 * reads its first request, and the catalog and launch methods are vendor
 * round trips measured at 15.9 s and 16.4 s on this board's LAN, so a
 * shorter wait is a LAN that got slow, not a client that is broken. Only the
 * session.poll status checks, which the core answers from its own state in
 * tens of milliseconds, keep the short one. */
#define OPENNOWLITE_RPC_TIMEOUT 60
#define OPENNOWLITE_CORE_PROTOCOL 6

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <wayland-client.h>

#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <gst/wayland/gstwlcontext.h>
#include <gst/app/gstappsrc.h>

#include "xdg-shell-client-protocol.h"

/* The streamer's types come from the header the library was generated from, so
 * the config layout the app fills and the layout the Rust side validates cannot
 * drift apart. opennow_streamer_* symbols are resolved by dlopen, not by the
 * link step, so this process never takes OpenNOW's own libopennow_streamer_ffi
 * SONAME and can never overwrite the library OpenNOW ships. */
#include "opennow_streamer_ffi.h"
#include <dlfcn.h>

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

struct lite {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_surface *pointer_surface;
    double pointer_x;
    double pointer_y;
    uint32_t enter_serial;
    bool configured;
    bool fullscreen;
    int width;
    int height;
    GstElement *pipeline;
    GstElement *sink;
    GstBus *bus;
    struct timespec last_motion;
    unsigned long keys;
    unsigned long motions;
    unsigned long submits;
    unsigned long input_refused;
    uint32_t modifier_mask;
    struct wl_buffer *window_buffer;
    void *window_pixels;
    size_t window_bytes;
    unsigned long pointer_skipped;
    bool auto_pilot;
    int pilot_times;
    int pilot_gap;
    bool pilot_click;
    int pilot_point_x;
    int pilot_point_y;
    unsigned long pilot_presses;
    unsigned long pilot_steps;
    unsigned long pilot_jiggles;
    struct timespec capture_started;
};

static struct lite app;

static void seat_key(uint32_t key_code, bool pressed);
static void seat_motion(struct lite *a, double x, double y);
static void seat_button(struct lite *a, uint32_t button, uint32_t state);
static void seat_wheel(struct lite *a, int32_t value120);
static void pilot_send(struct lite *a);

static long ms_since(const struct timespec *then)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - then->tv_sec) * 1000L +
           (now.tv_nsec - then->tv_nsec) / 1000000L;
}

/* Temporary input measurement (--debug-input). What the seat was complained
 * of is that a press of the pop-up's OK closed a few seconds later, and that
 * delay splits into three parts: how long the client held the press before
 * handing it over (the seat's event arrives and is submitted in the same
 * listener call, so this is the weston keyboard focus, already separate in
 * `opennowlite-input: key code=`), how long the streamer's own call took to
 * accept it, and how long the picture took to change afterwards. The first
 * two are measured here against the session clock, and the third one is read
 * off the video witness against the same clock, which is why both numbers are
 * printed in milliseconds since the capture started. The count is capped so
 * the log stays readable over an hour. */
static bool debug_input;
static unsigned long input_timed;

/* The seat's desktop arrow. weston's desktop-shell client draws this arrow
 * above every window, and a seat complaint was that the picture then carries
 * two pointers: the game's own, drawn inside the streamed pixels, and weston's
 * arrow on top of them - one cursor the game already publishes plus a
 * laggier duplicate of it. The streamer never draws over this window, so the
 * arrow is nobody's but the desktop's own: a client that asks for no cursor
 * over its surface (set_cursor with no surface, the CURSOR_BLANK path the
 * client library itself uses) maps the pointer's sprite away, which leaves
 * exactly the pointer the game drew. --show-cursor keeps the arrow for a
 * screen that is meant to show both. */
static bool show_cursor;
static long media_dead_seconds;
static guint64 media_watch_units;
static struct timespec media_watch_at;

static void note_submit(const char *kind, const struct timespec *at)
{
    if (!debug_input)
        return;
    ++input_timed;
    if (input_timed > 400u)
        return;
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    fprintf(stderr,
            "opennowlite-input: %s submit no=%lu at +%ldms took %ldus\n",
            kind, input_timed,
            (at->tv_sec - app.capture_started.tv_sec) * 1000L +
                (at->tv_nsec - app.capture_started.tv_nsec) / 1000000L,
            (until.tv_sec - at->tv_sec) * 1000000L +
                (until.tv_nsec - at->tv_nsec) / 1000L);
}

/* ---- xdg-shell ---- */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    struct lite *a = data;
    xdg_surface_ack_configure(surface, serial);
    a->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height, struct wl_array *states)
{
    (void)data; (void)toplevel; (void)width; (void)height; (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    struct lite *a = data;
    fprintf(stderr, "opennowlite: window close requested\n");
    running = 0;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

/* ---- keyboard ---- */

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)kb; (void)format; (void)size;
    if (fd >= 0)
        close(fd);
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *surface, struct wl_array *keys)
{
    (void)data; (void)kb; (void)serial; (void)surface; (void)keys;
    fprintf(stderr, "opennowlite-input: keyboard enter\n");
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)kb; (void)serial; (void)surface;
    fprintf(stderr, "opennowlite-input: keyboard leave\n");
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
                   uint32_t key, uint32_t state)
{
    (void)kb; (void)serial; (void)time;
    struct lite *a = data;
    a->keys++;
    /* Wayland key codes are evdev codes plus 8. */
    fprintf(stderr, "opennowlite-input: key code=%u evdev=%u state=%u\n",
            key, key >= 8 ? key - 8 : 0, state);
    seat_key(key, state == 1);
}

static void kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
                         uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    struct lite *a = data;
    /* weston sends the modifier mask on its own message; the bits that matter
     * for a game are the same ones GFN's modifier field carries. */
    a->modifier_mask = depressed;
}

static void kb_repeat(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
    (void)data; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat,
};

/* ---- pointer ---- */

static void ptr_enter(void *data, struct wl_pointer *ptr, uint32_t serial,
                      struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
    struct lite *a = data;
    a->enter_serial = serial;
    if (!show_cursor) {
        /* no cursor over this window: the desktop keeps its arrow on
         * surfaces this client does not speak for, and asking for none
         * here maps that sprite away for as long as the seat's pointer
         * is over the game's own picture */
        wl_pointer_set_cursor(ptr, serial, NULL, 0, 0);
        if (!a->pointer_surface)
            fprintf(stderr, "opennowlite-input: system cursor hidden over the window\n");
    }
    a->pointer_surface = surface;
    a->pointer_x = wl_fixed_to_double(sx);
    a->pointer_y = wl_fixed_to_double(sy);
    fprintf(stderr, "opennowlite-input: pointer enter x=%.1f y=%.1f surface=%p\n",
            a->pointer_x, a->pointer_y, (void *)surface);
    /* OpenNOW publishes the entry point too, so a cursor that re-enters the
     * window cannot leave the remote cursor on a stale position. */
    seat_motion(a, a->pointer_x, a->pointer_y);
}

static void ptr_leave(void *data, struct wl_pointer *ptr, uint32_t serial, struct wl_surface *surface)
{
    (void)ptr; (void)serial; (void)surface;
    struct lite *a = data;
    a->pointer_surface = NULL;
    fprintf(stderr, "opennowlite-input: pointer leave\n");
}

static void ptr_motion(void *data, struct wl_pointer *ptr, uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)ptr; (void)time;
    struct lite *a = data;
    a->pointer_x = wl_fixed_to_double(sx);
    a->pointer_y = wl_fixed_to_double(sy);
    a->motions++;
    seat_motion(a, a->pointer_x, a->pointer_y);
    /* Rate-limit the log; the position itself is kept on every event. */
    if (ms_since(&a->last_motion) >= 100) {
        clock_gettime(CLOCK_MONOTONIC, &a->last_motion);
        fprintf(stderr, "opennowlite-input: pointer motion x=%.1f y=%.1f\n",
                a->pointer_x, a->pointer_y);
    }
}

static void ptr_button(void *data, struct wl_pointer *ptr, uint32_t serial, uint32_t time,
                       uint32_t button, uint32_t state)
{
    (void)ptr; (void)serial; (void)time;
    struct lite *a = data;
    fprintf(stderr, "opennowlite-input: pointer button=%u state=%u\n",
            button, state);
    seat_button(a, button, state);
}

static void ptr_axis(void *data, struct wl_pointer *ptr, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)ptr; (void)time; (void)data;
    fprintf(stderr, "opennowlite-input: pointer axis=%u value=%.2f\n",
            axis, wl_fixed_to_double(value));
}

static void ptr_frame(void *data, struct wl_pointer *ptr)
{
    (void)data; (void)ptr;
}

static void ptr_axis_source(void *data, struct wl_pointer *ptr, uint32_t source)
{
    (void)data; (void)ptr; (void)source;
}

static void ptr_axis_stop(void *data, struct wl_pointer *ptr, uint32_t time, uint32_t axis)
{
    (void)data; (void)ptr; (void)time; (void)axis;
}

static void ptr_axis_discrete(void *data, struct wl_pointer *ptr, uint32_t axis, int32_t discrete)
{
    (void)data; (void)ptr; (void)axis; (void)discrete;
}

static void ptr_axis_value120(void *data, struct wl_pointer *ptr, uint32_t axis, int32_t value120)
{
    struct lite *a = data;
    (void)ptr; (void)axis;
    /* a wheel step is reported as 120 units of a turn; only vertical turns
     * reach the game. */
    if (axis == 1)
        seat_wheel(a, value120);
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
    .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
    .axis_value120 = ptr_axis_value120,
};

/* ---- seat ---- */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    struct lite *a = data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a->keyboard) {
        a->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(a->keyboard, &keyboard_listener, a);
        fprintf(stderr, "opennowlite: seat has a keyboard\n");
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && a->keyboard) {
        wl_keyboard_destroy(a->keyboard);
        a->keyboard = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->pointer) {
        a->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(a->pointer, &pointer_listener, a);
        fprintf(stderr, "opennowlite: seat has a pointer\n");
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && a->pointer) {
        wl_pointer_destroy(a->pointer);
        a->pointer = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat;
    fprintf(stderr, "opennowlite: seat name %s\n", name);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ---- registry ---- */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    struct lite *a = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        a->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                         version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        a->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        a->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(a->wm_base, &wm_base_listener, a);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        a->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                   version < 5 ? version : 5);
        wl_seat_add_listener(a->seat, &seat_listener, a);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---- window ---- */

/* The window's own frame. waylandsink does not draw into this surface: it
 * makes a subsurface of it (gst_wl_window_new_in_surface) and draws there, and
 * weston maps a view only when the surface itself has content. A bufferless
 * parent is therefore never mapped, and its subsurface is never shown either -
 * measured on the seat: with no parent buffer the media tallies advanced
 * (`video units=34800`, `/dev/video5` held open, dmabuf fds allocated, no
 * pipeline error) and the TV showed no window at all, while weston's own
 * `weston-flower` appeared on the same seat in the same second. So one
 * XRGB8888 frame the size of the window is committed after the first
 * configure, and the sink's subsurface renders on top of it. */
static int anonymous_file(size_t size)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    char name[256];
    snprintf(name, sizeof(name), "%s/opennowlite-frame-XXXXXX",
             dir && *dir ? dir : "/tmp");
    int fd = mkstemp(name);
    if (fd < 0)
        return -1;
    unlink(name);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool window_frame_commit(struct lite *a)
{
    const int stride = a->width * 4;
    const size_t size = (size_t)stride * (size_t)a->height;
    int fd = anonymous_file(size);
    if (fd < 0) {
        fprintf(stderr, "opennowlite: window frame file failed: %s\n",
                strerror(errno));
        return false;
    }
    void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        fprintf(stderr, "opennowlite: window frame map failed: %s\n",
                strerror(errno));
        close(fd);
        return false;
    }
    /* A dark slate, not black: a black window on this background is as
     * invisible as no window. XRGB8888 ignores the top byte. */
    uint32_t *words = pixels;
    for (size_t i = 0; i < size / 4u; ++i)
        words[i] = 0x00101820u;

    struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, fd, (int32_t)size);
    a->window_buffer = wl_shm_pool_create_buffer(pool, 0, a->width, a->height,
                                                 stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!a->window_buffer) {
        munmap(pixels, size);
        return false;
    }
    a->window_pixels = pixels;
    a->window_bytes = size;

    wl_surface_attach(a->surface, a->window_buffer, 0, 0);
    wl_surface_damage_buffer(a->surface, 0, 0, a->width, a->height);
    wl_surface_commit(a->surface);
    wl_display_flush(a->display);
    return true;
}

static bool create_window(struct lite *a)
{
    a->surface = wl_compositor_create_surface(a->compositor);
    if (!a->surface)
        return false;

    a->xdg_surface = xdg_wm_base_get_xdg_surface(a->wm_base, a->surface);
    if (!a->xdg_surface)
        return false;
    xdg_surface_add_listener(a->xdg_surface, &xdg_surface_listener, a);

    a->toplevel = xdg_surface_get_toplevel(a->xdg_surface);
    if (!a->toplevel)
        return false;
    xdg_toplevel_add_listener(a->toplevel, &toplevel_listener, a);
    xdg_toplevel_set_title(a->toplevel, "OpenNOWLite");
    xdg_toplevel_set_app_id(a->toplevel, "opennowlite");
    /* State the size before the first commit: stable xdg-shell has no
     * set_size request, the size is the buffer the surface attaches, and the
     * geometry the client declares here is what desktop-shell places. This
     * program is not a toolkit that declares one by itself. */
    xdg_surface_set_window_geometry(a->xdg_surface, 0, 0, a->width, a->height);
    if (a->fullscreen)
        xdg_toplevel_set_fullscreen(a->toplevel, NULL);

    /* No buffer in this commit: it only asks for the initial configure. */
    wl_surface_commit(a->surface);
    wl_display_flush(a->display);

    /* The sink needs a configured surface before it renders its first frame. */
    while (running && !a->configured) {
        if (wl_display_dispatch(a->display) < 0) {
            fprintf(stderr, "opennowlite: display dispatch failed: %s\n", strerror(errno));
            return false;
        }
    }
    if (!running)
        return false;

    /* From here the surface has content, so weston maps the window - with its
     * title bar and close button and a place on the desktop - while the
     * vendor queue is still being waited out. */
    if (!window_frame_commit(a))
        return false;

    fprintf(stderr, "opennowlite: window ready surface=%p app_id=opennowlite size=%dx%d\n",
            (void *)a->surface, a->width, a->height);
    return true;
}

/* ---- pipeline ---- */

static bool start_pipeline(struct lite *a, const char *description)
{
    GError *error = NULL;

    a->pipeline = gst_parse_launch(description, &error);
    if (!a->pipeline) {
        fprintf(stderr, "opennowlite: pipeline parse failed: %s\n",
                error ? error->message : "unknown");
        if (error)
            g_error_free(error);
        return false;
    }

    a->sink = gst_bin_get_by_name(GST_BIN(a->pipeline), "sink");
    if (!a->sink) {
        fprintf(stderr, "opennowlite: pipeline has no element named sink\n");
        return false;
    }
    if (!GST_IS_VIDEO_OVERLAY(a->sink)) {
        fprintf(stderr, "opennowlite: sink has no GstVideoOverlay interface\n");
        return false;
    }

    /* The display goes in first: without it the sink connects on its own and
     * the picture lands in a window this program does not own. */
    GstContext *context = gst_wl_display_handle_context_new(a->display);
    gst_element_set_context(a->sink, context);
    gst_context_unref(context);

    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(a->sink),
                                        (guintptr)a->surface);
    /* An external window has no size of its own: waylandsink refuses the first
     * frame with "Window has no size set" until the render rectangle says how
     * big the picture is. This also sets the toplevel's size, because the
     * sink's buffers are the only thing this surface ever attaches. */
    gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(a->sink),
                                           0, 0, a->width, a->height);

    a->bus = gst_element_get_bus(a->pipeline);
    if (gst_element_set_state(a->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "opennowlite: pipeline refused PLAYING\n");
        return false;
    }

    fprintf(stderr, "opennowlite: pipeline playing\n");
    return true;
}

static void stop_pipeline(struct lite *a)
{
    if (a->pipeline) {
        gst_element_set_state(a->pipeline, GST_STATE_NULL);
        gst_object_unref(a->pipeline);
        a->pipeline = NULL;
    }
    if (a->bus) {
        gst_object_unref(a->bus);
        a->bus = NULL;
    }
    if (a->sink) {
        gst_object_unref(a->sink);
        a->sink = NULL;
    }
}

static bool drain_bus(struct lite *a)
{
    if (!a->bus)
        return true;

    GstMessage *msg;
    while ((msg = gst_bus_pop(a->bus)) != NULL) {
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *error = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &error, &debug);
            fprintf(stderr, "opennowlite: pipeline error: %s\n",
                    error ? error->message : "unknown");
            if (debug)
                fprintf(stderr, "opennowlite: pipeline debug: %s\n", debug);
            g_clear_error(&error);
            g_free(debug);
            gst_message_unref(msg);
            return false;
        }
        case GST_MESSAGE_EOS:
            fprintf(stderr, "opennowlite: pipeline end of stream\n");
            gst_message_unref(msg);
            return false;
        default:
            break;
        }
        gst_message_unref(msg);
    }
    return true;
}

/* The audio pipeline has its own bus. Errors on it used to be invisible, so
 * this reports them the same way the video bus reports its own and names the
 * chain, because "pipeline error" alone would not say which one stopped.
 * It is defined with the media struct further down. */

/* ================= STAGE 2: session bootstrap + streamer + encoded media =================
 *
 * opennow-core (the shipped v1.0.4 binary) does sign-in, CloudMatch consent and
 * session negotiation over its JSON-lines RPC. Nothing here renders or decodes:
 * the reply's context is handed to the streamer, which only moves bytes, and
 * every media packet is put back into this program's own GStreamer pipeline.
 */

/* The replies are read by this file's own tiny scanner. It only knows what the
 * protocol needs: string values, whole sub-objects (copied byte-for-byte so the
 * scope/session/settings/shortcut objects reach the streamer exactly as the core
 * wrote them) and truthy booleans. It is not a general JSON parser. */
static const char *json_skip_ws(const char *at)
{
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r')
        ++at;
    return at;
}

/* One decoded character from a JSON string body, with the body advanced past
 * any escape. Returns '\0' when the body ended early. */
static char json_char(const char **body)
{
    const char *at = *body;
    if (*at != '\\') {
        *body = at + 1;
        return *at;
    }
    if (!at[1]) {
        *body = at + 1;
        return '\0';
    }
    *body = at + 2;
    switch (at[1]) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'u': return 'u';   /* tokens are ASCII; \u escapes stay as written */
    default: return at[1];
    }
}

/* Position just past the value for `key`. `key` is spelled as it appears in the
 * document, quotes included (e.g. "\"token\""). */
static const char *json_value_at(const char *doc, const char *key)
{
    const char *at = doc;
    size_t span = strlen(key);
    while ((at = strstr(at, key)) != NULL) {
        const char *after = json_skip_ws(at + span);
        if (*after != ':') {
            at = after;
            continue;
        }
        return json_skip_ws(after + 1);
    }
    return NULL;
}

/* End of a JSON value: matching brace/bracket depth with string bodies skipped,
 * or the closing quote of a string, or the next , } or ] of a bare word. */
static const char *json_value_end(const char *at)
{
    if (!*at)
        return NULL;
    if (*at == '"' && at[1]) {
        for (const char *body = at + 1; *body; ++body) {
            if (*body == '\\') {
                ++body;
                continue;
            }
            if (*body == '"')
                return body + 1;
        }
        return NULL;
    }
    if (*at == '{' || *at == '[') {
        char close = *at == '{' ? '}' : ']';
        int depth = 0;
        bool in_string = false;
        for (const char *at_value = at; *at_value; ++at_value) {
            if (in_string) {
                if (*at_value == '\\')
                    ++at_value;
                else if (*at_value == '"')
                    in_string = false;
                continue;
            }
            if (*at_value == '"')
                in_string = true;
            else if (*at_value == *at)
                ++depth;
            else if (*at_value == close && --depth == 0)
                return at_value + 1;
        }
        return NULL;
    }
    const char *at_value = at;
    while (*at && !strchr(",}]\n", *at))
        ++at;
    return at > at_value ? at : NULL;
}

/* The body advances by itself: json_char moves it past the character (or
 * escape) it returned, and walking it a second time per step is what cut
 * every other character of every value - "lite-hello" came back as "lt-el",
 * which is how the id match below failed and how the launch inspection was
 * asked for with a mangled appId, answered catalog_game_not_found. */
static bool json_string(const char *doc, const char *key, char *out,
                       size_t capacity)
{
    const char *at = json_value_at(doc, key);
    if (!at || *at != '"')
        return false;
    size_t written = 0;
    const char *body = at + 1;
    while (*body) {
        if (*body == '"')
            break;
        char value = json_char(&body);
        if (!value)
            return false;
        if (written + 1 >= capacity)
            return false;
        out[written++] = value;
    }
    out[written] = '\0';
    return true;
}

/* Copy a sub-object/array verbatim. A whole object ("scope", "settings") can
 * then be re-emitted into a request without being interpreted. */
static bool json_raw(const char *doc, const char *key, char *out,
                    size_t capacity)
{
    const char *at = json_value_at(doc, key);
    if (!at || (*at != '{' && *at != '['))
        return false;
    const char *end = json_value_end(at);
    if (!end)
        return false;
    size_t length = (size_t)(end - at);
    if (length + 1 > capacity)
        return false;
    memcpy(out, at, length);
    out[length] = '\0';
    return true;
}

static bool json_true(const char *doc, const char *key)
{
    const char *at = json_value_at(doc, key);
    return at && strncmp(at, "true", 4) == 0;
}

/* ---- the core's JSON-lines RPC ---- */

/* A reply line is one of the vendor's own documents: catalog.launch.inspect
 * carries the whole launch decision with the account's entitlement scope
 * inside it, and that scope is a grant over the account's library, so it has
 * no small fixed bound and was silently dropped at the old 32 KiB. The bound
 * here is what the read loop stops at, and it is generous; the request line
 * has no bound at all any more, since core_request sizes its own line. The
 * stack buffers of the callers keep the smaller bound below: none of the
 * params objects they build by hand comes close to it, and the one params
 * object that does carry a reply-sized value (the session.create scope) is
 * sized from the reply it came out of, in that caller. */
#define CORE_READ_MAX 262144
#define CORE_LINE_MAX 8192

typedef struct CoreRpc {
    int write_fd;
    int read_fd;
    pid_t child;
    char *buffer;
    size_t used;
} CoreRpc;

static struct CoreRpc *core_rpc;

/* The core is not an endpoint the shell connects to, it is a process the shell
 * pipes: src/main.rs reads requests with io::stdin().lines() and its writer
 * thread writes one reply per line to stdout, which is how opennow-qt's
 * CoreClient drives it through QProcess - two channels, one per direction.

 * One socket with two ends is not that shape, and the seat proved it: this
 * glibc's SOCK_STREAM socketpair puts both of its ends on one stream queue,
 * so the parent's next read handed back the request line it had just written
 * (a line whose "type" is "request", which the reply loop reads past) while
 * the core sat at its read(0 having received nothing, and the client waited
 * forever on a reply that could never come. Two pipes, one per direction,
 * give the core the stdin and the stdout it actually has, and a measured
 * child that echoes the line it read round-trips through that shape and
 * through it only.

 * Closing the request end when this program leaves is what hands the core an
 * end of input, so its Drop implementations stop and reap the out-of-process
 * native streamer and no orphan worker is left on the seat. */
static struct CoreRpc *core_start(const char *data_dir)
{
    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0)
        return NULL;
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return NULL;
    }

    pid_t child = fork();
    if (child < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return NULL;
    }
    if (child == 0) {
        /* The child keeps this program's stderr, so the core's own
         * diagnostics land in the run log. execv replaces the image, so
         * nothing of the window, the pipeline or the callbacks survives. */
        if (dup2(in_pipe[0], STDIN_FILENO) < 0 || dup2(out_pipe[1], STDOUT_FILENO) < 0)
            _exit(127);
        char *with_dir[3] = {
            (char *)"--data-dir", (char *)data_dir, NULL
        };
        char *none[1] = { NULL };
        execv(OPENNOWLITE_CORE_PATH, data_dir ? with_dir : none);
        _exit(127);
    }

    struct CoreRpc *rpc = calloc(1, sizeof(*rpc));
    if (!rpc) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return NULL;
    }
    rpc->write_fd = in_pipe[1];
    rpc->read_fd = out_pipe[0];
    rpc->child = child;
    rpc->buffer = malloc(CORE_READ_MAX);
    if (!rpc->buffer) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        free(rpc);
        return NULL;
    }
    /* The two ends the parent has no use for: the child's stdin and stdout
     * are the other two ends of these pipes and were dup2'd into place. */
    close(in_pipe[0]);
    close(out_pipe[1]);
    return rpc;
}

/* Leave the way opennow-qt leaves: close the request end so the core's
 * stdin sees end of input and cleans up, and only escalate once it had the
 * chance to do that. The core is waited for, so it is never left as a zombie
 * and no other process is touched.
 *
 * This C library has no waitpid_timeout extension, so the poll is done with
 * WNOHANG: it reaps the child the moment it has left the process table and
 * returns 0 while it is still running, which is the question asked here.
 * Nothing else is waited for, since the core's streamer is the core's own
 * child and its Drop implementations reap it. */
static void core_shutdown(struct CoreRpc *rpc)
{
    if (!rpc)
        return;
    close(rpc->write_fd);
    close(rpc->read_fd);
    if (rpc->child <= 0)
        return;
    int status = 0;
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (waitpid(rpc->child, &status, WNOHANG) > 0)
            return;
        struct timespec step = {0, 5000000L};
        nanosleep(&step, NULL);
    }
    kill(rpc->child, SIGTERM);
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (waitpid(rpc->child, &status, WNOHANG) > 0)
            return;
        struct timespec step = {0, 5000000L};
        nanosleep(&step, NULL);
    }
    kill(rpc->child, SIGKILL);
    waitpid(rpc->child, &status, WNOHANG);
}

static bool core_write_line(struct CoreRpc *rpc, const char *line)
{
    size_t length = strlen(line);
    size_t written = 0;
    while (written < length) {
        ssize_t out = write(rpc->write_fd, line + written, length - written);
        if (out < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (out == 0)
            return false;
        written += (size_t)out;
    }
    return true;
}

/* The core's own framing: one compact JSON object per line, whose type is
 * request, ack or cancel, and a request carries an id, a method and a params
 * object. There is no blank-line framing anywhere in that protocol.

 * The line is the size its own params need, not a fixed one: session.create
 * carries the whole account scope the launch inspection returned, and that
 * scope is a grant over the account's library, so a fixed line clipped it
 * silently and left a truncated JSON document on the wire, which the core
 * then never answered at all. The method and the line's size are logged
 * before it goes out, because a request the core never got is otherwise
 * indistinguishable from one the core refused. */
static bool core_request(struct CoreRpc *rpc, const char *id, const char *method,
                        const char *params)
{
    const char *body = (params && *params) ? params : "{}";
    int length = snprintf(NULL, 0,
        "{\"type\":\"request\",\"id\":\"%s\",\"method\":\"%s\","
        "\"params\":%s}\n", id, method, body);
    if (length <= 0)
        return false;
    fprintf(stderr, "opennowlite: rpc %s line=%d\n", method, length - 1);
    char *line = malloc((size_t)length + 1);
    if (!line)
        return false;
    snprintf(line, (size_t)length + 1,
        "{\"type\":\"request\",\"id\":\"%s\",\"method\":\"%s\","
        "\"params\":%s}\n", id, method, body);
    bool sent = core_write_line(rpc, line);
    free(line);
    if (!sent)
        fprintf(stderr, "opennowlite: rpc %s not sent: %s\n", method,
                errno == EPIPE ? "core closed its input" : strerror(errno));
    return sent;
}

/* A created cloud session is only kept if the shell acknowledges the create
 * request: the core holds it for ten seconds and closes the allocation when
 * nothing came back, which is what opennow-qt's own client does too. */
static bool core_ack(struct CoreRpc *rpc, const char *id)
{
    char line[256];
    int length = snprintf(line, sizeof(line),
        "{\"type\":\"ack\",\"id\":\"%s\"}\n", id);
    if (length < 0 || (size_t)length >= sizeof(line))
        return false;
    return core_write_line(rpc, line);
}

/* One reply = one JSON line. The core's own replies were measured on this
 * board: a hello answer is exactly {"id":"<request id>","ok":true,"result":
 * {...}} - it carries the request's id and an "ok" member but no "type" at
 * all, so a reply is recognised by those two and not by a type that is never
 * sent. The only lines that do carry a type are the ones this program writes
 * (request, ack, cancel) and the events the core publishes on its own (a
 * type of event), and those are read past rather than mistaken for a reply.
 * The copy is the caller's to free.

 * The wait for the next byte is bounded by the same timeout the caller asked
 * for: a bare read would sit in the kernel for as long as the core has
 * nothing to say, which is what a session that never got its request line
 * looked like - a live client and a core stopped at its own read, neither of
 * them logging anything, on a seat the user cannot reach. */
static char *core_reply(struct CoreRpc *rpc, const char *want, int timeout_s)
{
    struct timespec then;
    clock_gettime(CLOCK_MONOTONIC, &then);
    for (;;) {
        char *end = memchr(rpc->buffer, '\n', rpc->used);
        if (!end) {
            long elapsed = ms_since(&then);
            long remaining_ms = (long)timeout_s * 1000L - elapsed;
            if (remaining_ms <= 0) {
                fprintf(stderr, "opennowlite: reply for %s not received within %lds\n",
                        want, (long)timeout_s);
                return NULL;
            }
            if (rpc->used + 4096 >= CORE_READ_MAX) {
                fprintf(stderr, "opennowlite: reply for %s over %d bytes\n",
                        want, CORE_READ_MAX);
                return NULL;
            }
            struct pollfd waiting = { .fd = rpc->read_fd, .events = POLLIN };
            int ready = poll(&waiting, 1, (int)remaining_ms);
            if (ready == 0) {
                fprintf(stderr, "opennowlite: reply for %s not received within %lds\n",
                        want, (long)timeout_s);
                return NULL;
            }
            if (ready < 0 && errno != EINTR) {
                fprintf(stderr, "opennowlite: reply for %s not readable: %s\n",
                        want, strerror(errno));
                return NULL;
            }
            ssize_t got = read(rpc->read_fd, rpc->buffer + rpc->used, 4096);
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                return NULL;
            rpc->used += (size_t)got;
            continue;
        }
        size_t length = (size_t)(end - rpc->buffer);
        char *document = malloc(length + 1);
        if (!document)
            return NULL;
        memcpy(document, rpc->buffer, length);
        document[length] = '\0';
        rpc->used -= length + 1;
        if (rpc->used)
            memmove(rpc->buffer, end + 1, rpc->used);

        char type[16] = "";
        char got[64];
        json_string(document, "\"type\"", type, sizeof(type));
        bool framed = !strcmp(type, "event") || !strcmp(type, "request") ||
                      !strcmp(type, "ack") || !strcmp(type, "cancel");
        bool has_id = json_string(document, "\"id\"", got, sizeof(got));
        bool is_reply = !framed && json_value_at(document, "\"ok\"") != NULL;
        if (is_reply && (!want[0] || (has_id && !strcmp(got, want)))) {
            fprintf(stderr, "opennowlite: reply for %s in %ld ms\n", want,
                    ms_since(&then));
            return document;
        }
        free(document);
        if (ms_since(&then) > (long)timeout_s * 1000L) {
            fprintf(stderr, "opennowlite: reply for %s not received within %lds\n",
                    want, (long)timeout_s);
            return NULL;
        }
    }
}

/* The core answers a failed method with {"ok":false,"error":{"code":...}}.
 * Only the code is logged: the message is upstream prose and, like the rest
 * of the protocol's body, it never has to reach the run log. */
static bool core_refused(struct CoreRpc *rpc, const char *frame,
                        const char *method)
{
    if (frame && json_true(frame, "\"ok\""))
        return false;
    char code[64] = "no-reply";
    if (frame)
        json_string(frame, "\"code\"", code, sizeof(code));
    (void)rpc;
    fprintf(stderr, "opennowlite: launch %s %s\n", method, code);
    return true;
}
static char session_context[16384];

/* ---- streamer engine ---- */

/* The streamer is loaded by absolute path out of this recipe's own directory,
 * so this process has no DT_NEEDED entry for OpenNOW's SONAME and can never
 * load, shadow or replace the library OpenNOW ships. dlsym hands back an
 * untyped address; the function-pointer members below are the prototypes from
 * the header this library was generated against, so the call sites stay
 * type-checked. */
static struct Streamer {
    bool loaded;
    OpenNowStreamer *engine;
    bool capture_active;
    OpenNowStreamerStatus (*create)(
        const OpenNowStreamerConfig *config, OpenNowStreamer **output);
    OpenNowStreamerStatus (*destroy)(OpenNowStreamer *handle);
    OpenNowStreamerStatus (*send)(
        const OpenNowStreamer *handle, const uint8_t *bytes, size_t length);
    OpenNowStreamerStatus (*capture)(
        const OpenNowStreamer *handle, bool active, bool relative_mouse,
        uintptr_t window_handle, bool *raw_input_active);
    OpenNowStreamerStatus (*key)(
        const OpenNowStreamer *handle, uint16_t virtual_key, uint16_t modifiers,
        bool pressed);
    OpenNowStreamerStatus (*relative)(
        const OpenNowStreamer *handle, int16_t delta_x, int16_t delta_y);
    OpenNowStreamerStatus (*absolute)(
        const OpenNowStreamer *handle, uint16_t x, uint16_t y,
        uint16_t width, uint16_t height);
    OpenNowStreamerStatus (*button)(
        const OpenNowStreamer *handle, uint8_t button, bool pressed);
    OpenNowStreamerStatus (*wheel)(
        const OpenNowStreamer *handle, int16_t delta_x, int16_t delta_y);
    OpenNowStreamerStatus (*set_log)(const char *path);
} streamer;

/* dlsym returns void*, which no C type can name; casting to the exact
 * prototype is what lets the call sites stay checked. ISO C forbids this only
 * in pedantic mode, and glibc/musl return the callable address. */
typedef OpenNowStreamerStatus (*create_binding)(
    const OpenNowStreamerConfig *config, OpenNowStreamer **output);
typedef OpenNowStreamerStatus (*destroy_binding)(OpenNowStreamer *handle);
typedef OpenNowStreamerStatus (*send_binding)(
    const OpenNowStreamer *handle, const uint8_t *bytes, size_t length);
typedef OpenNowStreamerStatus (*capture_binding)(
    const OpenNowStreamer *handle, bool active, bool relative_mouse,
    uintptr_t window_handle, bool *raw_input_active);
typedef OpenNowStreamerStatus (*key_binding)(
    const OpenNowStreamer *handle, uint16_t virtual_key, uint16_t modifiers,
    bool pressed);
typedef OpenNowStreamerStatus (*relative_binding)(
    const OpenNowStreamer *handle, int16_t delta_x, int16_t delta_y);
typedef OpenNowStreamerStatus (*absolute_binding)(
    const OpenNowStreamer *handle, uint16_t x, uint16_t y,
    uint16_t width, uint16_t height);
typedef OpenNowStreamerStatus (*button_binding)(
    const OpenNowStreamer *handle, uint8_t button, bool pressed);
typedef OpenNowStreamerStatus (*wheel_binding)(
    const OpenNowStreamer *handle, int16_t delta_x, int16_t delta_y);
typedef OpenNowStreamerStatus (*log_binding)(const char *path);

static void streamer_load(void)
{
    void *library = dlopen(OPENNOWLITE_FFI_LIBRARY, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "opennowlite: streamer library missing: %s\n",
                dlerror());
        return;
    }
    streamer.create = (create_binding)dlsym(library, "opennow_streamer_create");
    streamer.destroy =
        (destroy_binding)dlsym(library, "opennow_streamer_destroy");
    streamer.send = (send_binding)dlsym(library, "opennow_streamer_send");
    streamer.capture =
        (capture_binding)dlsym(library, "opennow_streamer_set_capture_active");
    streamer.key = (key_binding)dlsym(library, "opennow_streamer_submit_key");
    streamer.relative = (relative_binding)dlsym(
        library, "opennow_streamer_submit_mouse_relative");
    streamer.absolute = (absolute_binding)dlsym(
        library, "opennow_streamer_submit_mouse_absolute");
    streamer.button = (button_binding)dlsym(
        library, "opennow_streamer_submit_mouse_button");
    streamer.wheel = (wheel_binding)dlsym(
        library, "opennow_streamer_submit_mouse_wheel");
    streamer.set_log = (log_binding)dlsym(
        library, "opennow_streamer_set_log_file");
    if (!streamer.create || !streamer.destroy || !streamer.send ||
        !streamer.capture || !streamer.key || !streamer.absolute ||
        !streamer.button || !streamer.wheel) {
        fprintf(stderr, "opennowlite: streamer library lacks an entry point\n");
        dlclose(library);
        return;
    }
    streamer.loaded = true;
}

/* The streamer replies with JSON lines that can carry the session's tokens, so
 * only the identifying fields are logged and the body never reaches the run
 * log. `bytes` is not NUL-terminated, hence the copy. */
static void on_stream_reply(const uint8_t *bytes, size_t length, void *unused)
{
    (void)unused;
    char id[64] = "";
    char type[32] = "";
    char code[64] = "";
    char *document = strndup((const char *)bytes, length);
    if (!document)
        return;
    json_string(document, "\"id\"", id, sizeof(id));
    json_string(document, "\"type\"", type, sizeof(type));
    json_string(document, "\"code\"", code, sizeof(code));
    if (type[0] || code[0])
        fprintf(stderr, "opennowlite: stream reply %s %s %s\n", id, type, code);
    fflush(stderr);
    free(document);
}

/* NVST notices (session timers, terminations, warnings) are reported but not
 * acted on yet; the callback still has to exist, otherwise the streamer has
 * nowhere to report them and a stuck stream would look healthy. */
static void on_stream_event(const uint8_t *bytes, size_t length, void *unused)
{
    (void)unused; (void)bytes; (void)length;
}

/* No frames are published to this client: the media pipeline decodes them. The
 * callback is still supplied so the streamer's frame publisher stays happy. */
static void on_frame_available(void *unused)
{
    (void)unused;
}

/* ---- key and pointer submission ---- */

/* The seat's keyboard reports evdev codes; GFN takes Windows virtual keys. This
 * is opennow's own mapping, where the X11 keycode (evdev + 8) selects the
 * physical key. A code that is not in the table submits nothing, so an unknown
 * key can never press the wrong one in the game. */
static uint16_t linux_virtual_key(uint32_t evdev)
{
    /* Upstream's en-US physical map, indexed by evdev code
     * (opennow-qt/src/streaming/PhysicalKeyMapData.h, Layout.virtualKeys[128]).
     * Its entries cover the character block only; the keys that have no
     * character come from upstream's own special-key switch
     * (StreamVideoItemInput.cpp) below. Measured on the seat: the table this
     * replaces had no Return entry at all, so pressing Enter on the vendor's
     * OK popup produced no virtual key and nothing was sent, and Escape came
     * out as Tab because the old code switched on the X11 keycode (evdev + 8)
     * with a table that was not shifted the same way. */
    static const uint16_t en_us[128] = {
        0x00, 0x00, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
        0x37, 0x38, 0x39, 0x30, 0xbd, 0xbb, 0x00, 0x00,
        0x51, 0x57, 0x45, 0x52, 0x54, 0x59, 0x55, 0x49,
        0x4f, 0x50, 0xdb, 0xdd, 0x00, 0x00, 0x41, 0x53,
        0x44, 0x46, 0x47, 0x48, 0x4a, 0x4b, 0x4c, 0xba,
        0xde, 0xc0, 0x00, 0xdc, 0x5a, 0x58, 0x43, 0x56,
        0x42, 0x4e, 0x4d, 0xbc, 0xbe, 0xbf, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe2, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    if (evdev < 128u && en_us[evdev])
        return en_us[evdev];
    switch (evdev) {
    case 1: return 0x1b;   /* Escape */
    case 14: return 0x08;  /* Backspace */
    case 15: return 0x09;  /* Tab */
    case 28: return 0x0d;  /* Return */
    case 29: return 0xa2;  /* Left Ctrl */
    case 42: return 0xa0;  /* Left Shift */
    case 54: return 0xa1;  /* Right Shift */
    case 56: return 0xa4;  /* Left Alt */
    case 58: return 0x14;  /* CapsLock */
    case 69: return 0x90;  /* NumLock */
    case 70: return 0x91;  /* ScrollLock */
    case 96: return 0x0d;  /* Keypad Enter */
    case 97: return 0xa3;  /* Right Ctrl */
    case 99: return 0x2a;  /* Print / SysRq */
    case 100: return 0xa5; /* AltGr (Right Alt) */
    case 102: return 0x24; /* Home */
    case 103: return 0x26; /* Up */
    case 104: return 0x21; /* Page Up */
    case 105: return 0x25; /* Left */
    case 106: return 0x27; /* Right */
    case 107: return 0x23; /* End */
    case 108: return 0x28; /* Down */
    case 109: return 0x22; /* Page Down */
    case 110: return 0x2d; /* Insert */
    case 111: return 0x2e; /* Delete */
    case 119: return 0x13; /* Pause */
    case 125: return 0x5b; /* Left Super */
    case 126: return 0x5c; /* Right Super */
    case 127: return 0x5d; /* Menu */
    default: break;
    }
    /* Function keys and keypad digits are not in the physical map either:
     * upstream maps F1..F24 to 0x70 + n and keypad digits to the plain
     * digits. */
    if (evdev >= 59u && evdev <= 68u)
        return (uint16_t)(0x70u + (evdev - 59u));   /* F1..F10 */
    if (evdev == 87u || evdev == 88u)
        return (uint16_t)(0x7au + (evdev - 87u));   /* F11, F12 */
    switch (evdev) {
    case 71: return 0x37;  /* Keypad 7 */
    case 72: return 0x38;  /* Keypad 8 */
    case 73: return 0x39;  /* Keypad 9 */
    case 75: return 0x34;  /* Keypad 4 */
    case 76: return 0x35;  /* Keypad 5 */
    case 77: return 0x36;  /* Keypad 6 */
    case 79: return 0x31;  /* Keypad 1 */
    case 80: return 0x32;  /* Keypad 2 */
    case 81: return 0x33;  /* Keypad 3 */
    case 82: return 0x30;  /* Keypad 0 */
    case 55: return 0x6a;  /* Keypad * */
    case 74: return 0x6d;  /* Keypad - */
    case 78: return 0x6b;  /* Keypad + */
    case 83: return 0x6e;  /* Keypad . */
    case 98: return 0x6f;  /* Keypad / */
    default: return 0;
    }
}

/* weston sends the modifier mask in its own message; GFN wants the modifier
 * bits that were down when the key moved. */
static uint16_t modifier_bits(uint32_t modifier_mask)
{
    /* The mask weston sends is an xkb modifier mask: bit 0 shift, bit 1
     * caps-lock, bit 2 control, bit 3 alt (Mod1), bit 6 super (Mod4). GFN's
     * own modifier field is 0x01 shift, 0x02 control, 0x04 alt, 0x08 super,
     * so control comes from bit 2 and alt from bit 3 - reading bit 1 as
     * control would have sent caps-lock as control and never sent control at
     * all. */
    uint16_t bits = 0;
    if (modifier_mask & (1u << 0))
        bits |= 0x01;
    if (modifier_mask & (1u << 2))
        bits |= 0x02;
    if (modifier_mask & (1u << 3))
        bits |= 0x04;
    if (modifier_mask & (1u << 6))
        bits |= 0x08;
    return bits;
}

/* ---- session bootstrap through the shipped core ---- */

/* opennow-core (the v1.0.4 binary this layer already ships) owns sign-in, the
 * catalog and CloudMatch consent. The lite client asks it for the NVST context
 * and never signs in or scrapes the catalog itself. */
/* A whole number value, for the CloudMatch status the vendor reports as an
 * int. Absent or non-numeric reads as -1. */
static int json_int_at(const char *doc, const char *key)
{
    const char *at = json_value_at(doc, key);
    if (!at || (*at < '0' && (*at != '-')) || *at > '9')
        return -1;
    return (int)strtol(at, NULL, 10);
}

/* ---- session bootstrap through the shipped core ---- */

/* The shipped opennow-core binary owns sign-in, the catalog and the
 * CloudMatch consent, so this client asks it for the launch context instead of
 * signing in or scraping anything itself. Nothing here renders or decodes: the
 * context is only handed to the streamer, whose packets this program puts back
 * into its own pipeline, and the input is submitted through the streamer too.
 * The core holds an exclusive lock on its data directory, so OpenNOW and
 * OpenNOWLite cannot hold a seat at the same time - see the layer README. */

/* How many times the allocation may be polled before the client gives up on
 * it. r1.4 tried 60 times 2 s apart and every round that lost the launch ran
 * out on the last try while the reply was still status=1 phase=preparing; the
 * timed-out reply had already grown from 1846 bytes carrying an empty rtsps
 * array (`rtsps=2`) to 2322 bytes carrying real endpoints (`rtsps=119`), so
 * the ready answer was one poll away and only the wait was too short. The
 * vendor's own shell leaves the same wait to the account's turn in the free-
 * tier queue, so this is overridable: --poll-attempts=N at 2 s apart, 300
 * (ten minutes) by default. */
static int session_poll_attempts = 300;

static bool session_open(const char *variant_id, const char *data_dir)
{
    core_rpc = core_start(data_dir);
    if (!core_rpc) {
        fprintf(stderr, "opennowlite: core process unavailable: %s\n",
                OPENNOWLITE_CORE_PATH);
        return false;
    }

    char params[CORE_LINE_MAX];
    char *frame;

    /* 1 · the core refuses every method until the shell's protocol version
     * matches its own; the shipped v1.0.4 speaks 6. */
    snprintf(params, sizeof(params),
        "{\"protocolVersion\":%d,\"shell\":\"opennowlite\","
        "\"shellVersion\":\"1.0.4-nightly.1515.1\"}",
        OPENNOWLITE_CORE_PROTOCOL);
    if (!core_request(core_rpc, "lite-hello", "core.hello", params))
        return false;
    frame = core_reply(core_rpc, "lite-hello", OPENNOWLITE_RPC_TIMEOUT);
    if (core_refused(core_rpc, frame, "core.hello")) {
        free(frame);
        return false;
    }
    free(frame);

    /* 2 · the catalog entry for the launch id. The id the vendor keys the
     * launch decision by is the parent game's, so the variant is looked up
     * first to find it. The reply's top level "id" is this request's own id,
     * so the game object is taken first and its id read out of that. */
    snprintf(params, sizeof(params), "{\"variantId\":\"%s\"}", variant_id);
    core_request(core_rpc, "lite-game", "catalog.game.get", params);
    frame = core_reply(core_rpc, "lite-game", OPENNOWLITE_RPC_TIMEOUT);
    if (core_refused(core_rpc, frame, "catalog.game.get")) {
        free(frame);
        return false;
    }
    char game[CORE_LINE_MAX];
    game[0] = '\0';
    (void)json_raw(frame, "\"game\"", game, sizeof(game));
    free(frame);
    char game_id[256] = "";
    if (game[0])
        json_string(game, "\"id\"", game_id, sizeof(game_id));
    if (!game_id[0]) {
        fprintf(stderr, "opennowlite: catalog game has no id\n");
        return false;
    }

    /* 3 · the vendor's launch decision for this variant, and the account
     * scope it was taken with. CloudMatch compares that scope byte for byte. */
    snprintf(params, sizeof(params), "{\"appId\":\"%s\",\"variantId\":\"%s\"}",
            game_id, variant_id);
    core_request(core_rpc, "lite-launch", "catalog.launch.inspect", params);
    frame = core_reply(core_rpc, "lite-launch", OPENNOWLITE_RPC_TIMEOUT);
    if (core_refused(core_rpc, frame, "catalog.launch.inspect")) {
        free(frame);
        return false;
    }
    /* The decision and the scope are taken at the reply's own size. The
     * scope is re-emitted into session.create byte for byte, which is what
     * CloudMatch compares, so a fixed buffer here would either drop a scope
     * that does not fit - and then no session is asked for at all, with
     * nothing on the log to say why - or send half of one. */
    size_t reply_len = strlen(frame);
    char *decision = malloc(reply_len + 1);
    char *scope = malloc(reply_len + 1);
    char launch_status[64] = "";
    if (!decision || !scope) {
        fprintf(stderr, "opennowlite: launch inspection reply=%zu bytes: out of memory\n",
                reply_len);
        free(frame);
        free(decision);
        free(scope);
        return false;
    }
    decision[0] = '\0';
    scope[0] = '\0';
    if (json_raw(frame, "\"decision\"", decision, reply_len + 1))
        json_string(decision, "\"status\"", launch_status,
                    sizeof(launch_status));
    json_raw(frame, "\"scope\"", scope, reply_len + 1);
    fprintf(stderr, "opennowlite: launch inspect reply=%zu decision=%zu scope=%zu status=%s\n",
            reply_len, decision[0] ? strlen(decision) : 0,
            scope[0] ? strlen(scope) : 0,
            launch_status[0] ? launch_status : "unknown");
    free(frame);
    if (strcmp(launch_status, "ready") != 0) {
        fprintf(stderr, "opennowlite: launch is not ready: %s\n",
                launch_status[0] ? launch_status : "unknown");
        free(decision);
        free(scope);
        return false;
    }
    if (!scope[0]) {
        fprintf(stderr, "opennowlite: launch inspection carried no account scope\n");
        free(decision);
        free(scope);
        return false;
    }
    free(decision);

    /* 4 · CloudMatch consent for this launch. The launch id is the variant,
     * the catalog id is the parent game, and the account scope is re-emitted
     * exactly as the core wrote it, since it is compared byte for byte. The
     * params object is the scope plus a little, so it is sized from the scope. */
    size_t params_len = strlen(scope) + 512;
    char *big_params = malloc(params_len);
    if (!big_params) {
        fprintf(stderr, "opennowlite: session.create params: out of memory\n");
        free(scope);
        return false;
    }
    snprintf(big_params, params_len,
        "{\"appId\":\"%s\",\"catalogAppId\":\"%s\",\"variantId\":\"%s\","
        "\"scope\":%s}",
        variant_id, game_id, variant_id, scope);
    free(scope);
    if (!core_request(core_rpc, "lite-create", "session.create", big_params)) {
        free(big_params);
        return false;
    }
    free(big_params);
    frame = core_reply(core_rpc, "lite-create", OPENNOWLITE_RPC_TIMEOUT);
    if (core_refused(core_rpc, frame, "session.create")) {
        free(frame);
        return false;
    }
    char session_id[256] = "";
    bool session_named =
        json_string(frame, "\"sessionId\"", session_id, sizeof(session_id));
    char created_phase[32] = "";
    (void)json_string(frame, "\"phase\"", created_phase, sizeof(created_phase));
    fprintf(stderr, "opennowlite: session create reply=%zu named=%d phase=%s\n",
            strlen(frame), session_named,
            created_phase[0] ? created_phase : "-");
    free(frame);
    if (!session_id[0]) {
        fprintf(stderr, "opennowlite: the allocated session has no id\n");
        return false;
    }
    /* the allocation is dropped unless the shell accepts it right away */
    if (!core_ack(core_rpc, "lite-create"))
        return false;

    /* 5 · wait until the vendor marks the session ready, then let the core's
     * own negotiator produce the NVST context. The core keeps the session, so
     * the id is enough to address it and the context comes back verbatim.
     *
     * The poll result is {"session":{...}} - the session info carries the
     * numeric status, a phase word, the rtsps endpoints and the artwork, so
     * like the launch decision it is taken at the reply's own size: with the
     * old fixed 8 KiB buffer a session info that came close to it was
     * dropped whole (json_raw refuses when the value does not fit), the
     * status parsed as 0 and the loop reported not-ready forever. */
    /* The vendor's own shell polls the same allocation with a 35 s request
     * timeout on a 2 s interval and leaves the queue wait to the account's
     * turn, which on a free-tier queue runs into minutes, so the wait here
     * is long too: giving up after eight tries stopped the client while the
     * allocation was still preparing, and the seat never got a picture. */
    bool prepared = false;
    int last_status = 0;
    char last_phase[32] = "";
    /* How long the allocation may stay preparing. The rounds at r1.4 that gave
     * up after 60 tries never got a picture, and the timed-out poll was
     * already holding the rtsps endpoints: the reply grew from 1846 bytes with
     * rtsps=2 (an empty array) to 2322 bytes with rtsps=119, so the ready
     * answer was one poll away and the wait was cut short, not refused. The
     * vendor's own shell leaves this wait to the account's turn in the free-
     * tier queue, which runs into minutes, so the default here is long and
     * overridable: --poll-attempts=<n> at 2 s apart. */
    int poll_limit = session_poll_attempts;
    fprintf(stderr, "opennowlite: polling the allocation up to %d times\n",
            poll_limit);
    for (int attempt = 0; attempt < poll_limit && !prepared; ++attempt) {
        snprintf(params, sizeof(params), "{\"sessionId\":\"%s\"}", session_id);
        core_request(core_rpc, "lite-poll", "session.poll", params);
        frame = core_reply(core_rpc, "lite-poll", OPENNOWLITE_SESSION_TIMEOUT);
        if (!frame) {
            fprintf(stderr, "opennowlite: launch session.poll no-reply\n");
            break;
        }
        bool ready = false;
        if (!core_refused(core_rpc, frame, "session.poll")) {
            char *poll = malloc(strlen(frame) + 1);
            if (!poll) {
                fprintf(stderr, "opennowlite: session poll: out of memory\n");
                free(frame);
                return false;
            }
            poll[0] = '\0';
            bool got_session = json_raw(frame, "\"session\"", poll,
                                        strlen(frame) + 1);
            int status = json_int_at(poll, "\"status\"");
            char phase[32] = "";
            json_string(poll, "\"phase\"", phase, sizeof(phase));
            char *rtsps = malloc(strlen(frame) + 1);
            rtsps[0] = '\0';
            (void)json_raw(poll, "\"rtspsEndpoints\"", rtsps,
                           strlen(frame) + 1);
            fprintf(stderr, "opennowlite: poll attempt=%d reply=%zu session=%d status=%d phase=%s rtsps=%zu\n",
                    attempt + 1, strlen(frame), got_session, status,
                    phase[0] ? phase : "-", rtsps[0] ? strlen(rtsps) : 0);
            free(rtsps);
            ready = status == 2 || status == 3;
            last_status = status;
            if (phase[0]) {
                strncpy(last_phase, phase, sizeof(last_phase) - 1);
                last_phase[sizeof(last_phase) - 1] = '\0';
            }
            free(poll);
        }
        free(frame);
        frame = NULL;
        if (!ready) {
            usleep(2000000u);
            continue;
        }

        snprintf(params, sizeof(params),
            "{\"session\":{\"sessionId\":\"%s\"}}", session_id);
        core_request(core_rpc, "lite-prepare", "streamer.prepare", params);
        frame = core_reply(core_rpc, "lite-prepare", OPENNOWLITE_RPC_TIMEOUT);
        if (core_refused(core_rpc, frame, "streamer.prepare")) {
            free(frame);
            frame = NULL;
            continue;
        }
        prepared = json_raw(frame, "\"context\"",
                            (char *)session_context, sizeof(session_context));
        free(frame);
        frame = NULL;
    }
    if (!prepared) {
        fprintf(stderr, "opennowlite: streamer context unavailable: last poll status=%d phase=%s\n",
                last_status, last_phase[0] ? last_phase : "-");
        return false;
    }
    fprintf(stderr, "opennowlite: session ready for %s\n", variant_id);
    return true;
}

/* ---- the encoded-media hand-off ---- */

/* The tap hands over what the streamer already assembled: one video packet is
 * one complete access unit (its own ingress log says so: "media-ingress
 * first assembled video bytes=31550 keyframe=true", "nvst-keyframe
 * assembled frame=35431 bytes=21556"), and one audio packet is one complete
 * Opus frame. So the bytes go straight into this program's pipeline: h264parse
 * is fed the continuous byte stream its own caps describe
 * (stream-format=byte-stream,alignment=au) and finds the unit boundaries
 * itself. An accumulator that only flushed on the keyframe flag was the
 * reason the picture stopped: the flag is set on a fraction of the units, the
 * accumulator filled its 16 MiB cap between two of them and reported
 * "video unit overflow", and the bytes it then pushed crossed unit boundaries
 * mid-picture. */
typedef struct Media {
    GstElement *video;
    GstElement *audio;
    GstBus *audio_bus;
    gboolean audio_ready;
    GstAppSrc *video_source;
    GstAppSrc *audio_source;
    /* Set on the main thread before the pipeline is torn down and read by the
     * streamer's media thread; the lock below makes that read safe. */
    volatile gboolean stopping;
    guint64 video_base;
    guint64 audio_base;
    guint64 video_units;
    guint64 audio_units;
    guint64 video_refused;
    guint64 audio_refused;
    FILE *record;
    long long record_units_left;
    long long record_from_left;
    gboolean record_window_open;
    guint64 recorded_bytes;
    FILE *audio_dump;
    long long audio_dump_left;
    guint64 audio_dump_frames;
    guint64 audio_dump_bytes;
    void *opus_lib;
    void *opus_dec;
    int (*opus_nb_samples)(const unsigned char *, int, int);
    int (*opus_decode)(void *, const unsigned char *, int, short *, int, int);
    guint64 audio_decoded;
    guint64 audio_failed;
    /* the decoded-level probe: what the client actually handed the sink, so a
     * silent monitor can be attributed to the decode or to the sink in one
     * round instead of two. */
    guint64 audio_peak;
    guint64 audio_nonzero;
    guint64 audio_samples;
    /* the decoded-frame probe: what reached the video sink, beside the tap's
     * own count, so a starved decoder reads as a low rate here while the tap
     * still counts its units. */
    guint64 video_presented;
    guint64 video_presented_at;
    long video_probe_at;
} Media;

static Media media;
/* Serializes the tap's pushes against the teardown. The encoded-media
 * callback runs on the streamer's own thread, and the pipeline is destroyed on
 * the main thread; without this, a push that arrives during shutdown reaches
 * an appsrc that stop_pipeline has already unref'd (r2.5's rounds ended with
 * `gst_app_src_push_internal: assertion 'GST_IS_APP_SRC (appsrc)' failed`). */
static GMutex media_lock;

/* Counts the buffers that reach the sink's sink pad. `video units` is what the
 * tap handed to appsrc; this is what the decoder produced and the sink was
 * given, so the two together separate a slow stream from a slow decoder. */
static GstPadProbeReturn on_video_frame(GstPad *pad, GstPadProbeInfo *info,
                                        gpointer user)
{
    (void)pad;
    (void)info;
    ((Media *)user)->video_presented++;
    return GST_PAD_PROBE_OK;
}

/* A session whose media stopped is not a session that is worth holding: measured
 * on the r1.9 round of Nov 8, the vendor kept the session alive and kept
 * answering input - the pilot logged jiggle 186 at +1142 s, `submit no=192 at
 * +1142019ms took 5us` - while its media had already stopped at
 * `NVST rx-stats elapsed=460.1s ... frames=26917` and `video units=26400 queued=0`,
 * so 680 further seconds of the round's budget carried one frame and nothing
 * else. A round like that is over: the pilot starts the next one, and the next
 * one pays its own vendor prepare and re-drives Steam's launch dialog, which is
 * what the hour needs. So the client watches its own hand-over count: once the
 * picture has started, `--media-dead=S` ends the session when the count has not
 * advanced for S seconds, and it does that with the client's own tally rather
 * than the streamer's, because the tally is the client's own state. */
static void media_watch(void)
{
    if (media_dead_seconds <= 0 || !media.video_units)
        return;

    /* What the decoder actually produced, once per five seconds, beside the
     * tap's own count: a healthy `units` with a low rate here means the
     * decoder or the sink is the slow part, not the stream. */
    const long now = ms_since(&app.capture_started);
    if (now - media.video_probe_at >= 5000) {
        const long span = now - media.video_probe_at;
        const guint64 got = media.video_presented - media.video_presented_at;
        fprintf(stderr,
                "opennowlite: video decoded=%llu in %ldms (%.1f fps) units=%llu at +%lds\n",
                (unsigned long long)got, span,
                span > 0 ? (double)got * 1000.0 / (double)span : 0.0,
                (unsigned long long)media.video_units, now / 1000L);
        media.video_presented_at = media.video_presented;
        media.video_probe_at = now;
    }

    if (media.video_units != media_watch_units) {
        media_watch_units = media.video_units;
        clock_gettime(CLOCK_MONOTONIC, &media_watch_at);
        return;
    }
    long idle = ms_since(&media_watch_at);
    if (idle < media_dead_seconds * 1000L)
        return;
    fprintf(stderr,
            "opennowlite: media stopped at units=%llu for %lds at +%lds -- ending the session\n",
            (unsigned long long)media.video_units, idle / 1000L,
            ms_since(&app.capture_started) / 1000L);
    running = 0;
}

/* The audio pipeline has its own bus. Errors on it used to be invisible, so
 * this reports them the same way the video bus reports its own and names the
 * chain, because "pipeline error" alone would not say which one stopped. Every
 * error message is logged with its source element: the first one read is often
 * the appsrc's "internal data stream error", which only says that something
 * downstream failed - the element that actually rejected the data (opusdec)
 * posts its own message right behind it. */
static bool drain_audio_bus(Media *owner)
{
    if (!owner->audio_bus)
        return true;

    bool healthy = true;
    GstMessage *msg;
    while ((msg = gst_bus_pop(owner->audio_bus)) != NULL) {
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *error = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &error, &debug);
            fprintf(stderr, "opennowlite: audio pipeline error from %s: %s\n",
                    GST_MESSAGE_SRC_NAME(msg),
                    error ? error->message : "unknown");
            if (debug)
                fprintf(stderr, "opennowlite: audio pipeline debug: %s\n",
                        debug);
            g_clear_error(&error);
            g_free(debug);
            healthy = false;
            break;
        }
        case GST_MESSAGE_EOS:
            fprintf(stderr, "opennowlite: audio pipeline end of stream\n");
            healthy = false;
            break;
        default:
            break;
        }
        gst_message_unref(msg);
    }
    return healthy;
}

/* An audio failure costs the sound, not the picture: the vendor queue may have
 * taken two minutes, and throwing the session away because opusdec disliked one
 * packet would make the user wait it out again. This stops the chain and
 * leaves the video pipeline running. The elements are not freed here: the
 * streamer's media thread may be inside gst_app_src_push_buffer right now, and
 * keeping the objects alive until the program exits is what makes that safe. */
static void media_audio_teardown(Media *owner)
{
    owner->audio_ready = false;
    if (owner->audio)
        gst_element_set_state((GstElement *)owner->audio, GST_STATE_NULL);
    fprintf(stderr, "opennowlite: audio chain disabled, video continues\n");
}

static void media_push_video_locked(Media *owner, const OpenNowStreamerEncodedMedia
                            *packet)
{
    const guint8 *data = packet->data;
    const gsize length = (gsize)packet->data_len;
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, length, NULL);
    if (!buffer)
        return;
    gst_buffer_fill(buffer, 0, data, length);

    guint32 rate = packet->clock_rate_hz ? packet->clock_rate_hz : 90000u;
    guint64 pts = (guint64)packet->timestamp * GST_SECOND / rate;
    if (!owner->video_base)
        owner->video_base = pts + GST_SECOND;
    GST_BUFFER_PTS(buffer) = pts + GST_SECOND - owner->video_base;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 60u;
    if (gst_app_src_push_buffer(owner->video_source, buffer)
        != GST_FLOW_OK) {
        /* appsrc owns the buffer it refused and releases it with the element,
         * so nothing here has to be freed - but the unit is gone, and saying
         * so on a clock is what keeps a slow decoder from reading as a
         * silently frozen picture. */
        ++owner->video_refused;
        if (owner->video_refused == 1u)
            fprintf(stderr, "opennowlite: video unit not queued\n");
        else if (owner->video_refused % 600u == 0u)
            fprintf(stderr, "opennowlite: video refused=%llu\n",
                    (unsigned long long)owner->video_refused);
        return;
    }
    ++owner->video_units;
    if (owner->record && owner->record_from_left > 0LL) {
        /* --record-from=S: the units before the window are counted and dropped,
         * which is what lets a round witness the part of a session after the
         * first few minutes. That is the part the r22 rounds never got: every
         * round of Nov 8 recorded its first 420 s while its media died at
         * +930 s, +1 080 s and +1 220 s of session (the deaths read off the
         * client's own tally: `video units=55154` while `rx-stats
         * elapsed=930.2s frames=55152`), so the six minutes the game had to
         * act in - the pilot's acts at +602s, +702s and +802s and the media's
         * own death at +930s - were unwitnessed and only the loading screen at
         * +240 s to +420 s was ever readable. The window is written as a raw
         * stream that starts mid-GOP, so the first frames up to the next
         * keyframe are not decodable and say so when the storyboard is read.
         * The pair of branches is one chain and not two: the r23 round of Nov
         * 8 armed --record-from=420 with the skip as its own if and the write
         * as a second one, so a skipped unit fell through to the write and the
         * round recorded the first 25200 units of picture anyway, printing
         * `recorded bytes=225291447 units_left=0` BEFORE its
         * `witness window opens at media unit 25200 at +439s` line, while the
         * window it was meant to witness wrote nothing. */
        --owner->record_from_left;
        if (owner->record_from_left == 0LL) {
            owner->record_window_open = true;
            fprintf(stderr,
                    "opennowlite: witness window opens at media unit %llu at +%lds\n",
                    (unsigned long long)owner->video_units,
                    ms_since(&app.capture_started) / 1000L);
        }
    }
    if (owner->record && owner->record_from_left == 0LL
        && owner->record_units_left > 0LL) {
        /* the witness: the tap's units are whole H.264 access units with their
         * start codes, so writing them out in order gives a raw stream ffmpeg
         * can be pointed at, which is how a screen nobody is looking at can
         * still be read. Bounded by seconds of picture, not by the session. */
        if (fwrite(data, 1, length, owner->record) == length) {
            owner->record_units_left -= 1LL;
            owner->recorded_bytes += (guint64)length;
            if (owner->record_units_left % 3000LL == 0LL)
                fprintf(stderr, "opennowlite: recorded bytes=%llu units_left=%lld\n",
                        (unsigned long long)owner->recorded_bytes,
                        owner->record_units_left);
        } else if (owner->recorded_bytes == 0u) {
            fprintf(stderr, "opennowlite: record write failed: %s\n",
                    strerror(errno));
            owner->record_units_left = 0LL;
        }
    }
    if (owner->video_units == 1u) {
        fprintf(stderr, "opennowlite: encoded video unit 1\n");
    } else if (owner->video_units % 600u == 0u) {
        /* the queued number is the appsrc's own queue depth in bytes
         * (`current-level-bytes`): at 60 units a second a chain that keeps up
         * stays near one unit of picture (~30 KiB), and a chain that has
         * fallen behind grows. `gst_app_src_get_size` is not this number - it
         * answers the stream size the client declared, which this program
         * never does, and it printed 18446744073709551615 (uint64 -1) on the
         * seat. */
        fprintf(stderr, "opennowlite: video units=%llu queued=%llu\n",
                (unsigned long long)owner->video_units,
                (unsigned long long)gst_app_src_get_current_level_bytes(owner->video_source));
    }
}

/* The tap's entry point: the lock is held for the whole push so the teardown
 * cannot clear the source under it. */
static void media_push_video(Media *owner,
                             const OpenNowStreamerEncodedMedia *packet)
{
    g_mutex_lock(&media_lock);
    if (!owner->stopping && owner->video_source)
        media_push_video_locked(owner, packet);
    g_mutex_unlock(&media_lock);
}

static void media_push_audio_locked(Media *owner,
                            const OpenNowStreamerEncodedMedia *packet)
{
    guint32 rate = packet->clock_rate_hz ? packet->clock_rate_hz : 48000u;
    guint64 pts = (guint64)packet->timestamp * GST_SECOND / rate;
    if (!owner->audio_base)
        owner->audio_base = pts + GST_SECOND;

    GstBuffer *buffer = NULL;
    guint64 duration = GST_SECOND / 200u;
    if (owner->opus_dec) {
        /* the raw path: libopus decodes the frame here, and the sink is handed
         * the shape the seat's own audio test proved audible. The call shapes
         * are opusprobe's: each packet's own length is asked first, because
         * opus_decode answers -1 for a frame_size of 0. */
        short pcm[5760 * 2];
        int ns = owner->opus_nb_samples(packet->data, (int)packet->data_len,
                                        (int)rate);
        if (ns <= 0 || ns > 5760)
            ns = 240;
        int got = owner->opus_decode(owner->opus_dec, packet->data,
                                     (int)packet->data_len, pcm, ns, 0);
        if (got <= 0) {
            ++owner->audio_failed;
            if (owner->audio_failed == 1u)
                fprintf(stderr,
                        "opennowlite: audio decode failed (%d) on %zu bytes\n",
                        got, packet->data_len);
            else if (owner->audio_failed % 1500u == 0u)
                fprintf(stderr, "opennowlite: audio decode failed=%llu\n",
                        (unsigned long long)owner->audio_failed);
            return;
        }
        for (int i = 0; i < got * 2; ++i) {
            int v = pcm[i];
            int a = v < 0 ? -v : v;
            if ((guint64)a > owner->audio_peak)
                owner->audio_peak = (guint64)a;
            if (v)
                ++owner->audio_nonzero;
        }
        owner->audio_samples += (guint64)got * 2u;
        if (owner->audio_units == 0u)
            fprintf(stderr,
                    "opennowlite: audio first frame got=%d s0..7=%d %d %d %d %d %d %d %d\n",
                    got, pcm[0], pcm[1], pcm[2], pcm[3], pcm[4], pcm[5], pcm[6],
                    pcm[7]);
        buffer = gst_buffer_new_allocate(NULL, (gsize)got * 2u * 2u, NULL);
        if (!buffer)
            return;
        gst_buffer_fill(buffer, 0, pcm, (gsize)got * 2u * 2u);
        duration = GST_SECOND * (guint64)got / rate;
    } else {
        buffer = gst_buffer_new_allocate(NULL, packet->data_len, NULL);
        if (!buffer)
            return;
        gst_buffer_fill(buffer, 0, packet->data, packet->data_len);
    }
    GST_BUFFER_PTS(buffer) = pts + GST_SECOND - owner->audio_base;
    /* 5 ms, not 20: the tap delivers ~200 packets a second against 60 video
     * units a second, and the opennow layer's own audio queue says why
     * ("the play queue holds 12 packets of 5 ms"), so an Opus frame off this
     * wire is 5 ms of audio. Naming 20 ms made the audio chain run 4x faster
     * than the clock it was handed. */
    GST_BUFFER_DURATION(buffer) = duration;
    if (gst_app_src_push_buffer(owner->audio_source, buffer)
        != GST_FLOW_OK) {
        ++owner->audio_refused;
        if (owner->audio_refused == 1u)
            fprintf(stderr, "opennowlite: audio packet not queued\n");
        else if (owner->audio_refused % 1500u == 0u)
            fprintf(stderr, "opennowlite: audio refused=%llu\n",
                    (unsigned long long)owner->audio_refused);
        return;
    }
    ++owner->audio_units;
    if (owner->audio_dump && owner->audio_dump_left > 0LL) {
        /* the audio witness: the tap's Opus frames with a little-endian length
         * in front of each, so the bytes an offline mux turns into an Ogg Opus
         * file are exactly the bytes the seat's decoders were handed. Writing
         * them here costs the session nothing: no second pipeline, no decode,
         * a bounded count of frames rather than of seconds. */
        guint32 le_len = (guint32)packet->data_len;
        unsigned char hdr[4] = {
            (unsigned char)(le_len & 0xffu),
            (unsigned char)((le_len >> 8) & 0xffu),
            (unsigned char)((le_len >> 16) & 0xffu),
            (unsigned char)((le_len >> 24) & 0xffu),
        };
        if (fwrite(hdr, 1, sizeof(hdr), owner->audio_dump) == sizeof(hdr)
            && fwrite(packet->data, 1, packet->data_len, owner->audio_dump)
                == (size_t)packet->data_len) {
            owner->audio_dump_left -= 1LL;
            owner->audio_dump_frames += 1u;
            owner->audio_dump_bytes += (guint64)packet->data_len;
            if (owner->audio_dump_frames % 6000u == 0u)
                fprintf(stderr,
                        "opennowlite-audio: witness frames=%llu bytes=%llu at +%ldms\n",
                        (unsigned long long)owner->audio_dump_frames,
                        (unsigned long long)owner->audio_dump_bytes,
                        ms_since(&app.capture_started));
        } else if (owner->audio_dump_frames == 0u) {
            fprintf(stderr, "opennowlite-audio: witness write failed: %s\n",
                    strerror(errno));
            owner->audio_dump_left = 0LL;
        }
    }
    if (owner->audio_units == 1u) {
        fprintf(stderr,
                "opennowlite: encoded audio packet 1 bytes=%zu channels=%u "
                "rate=%u ts=%llu head=%02x %02x %02x %02x\n",
                packet->data_len, packet->channels, packet->clock_rate_hz,
                (unsigned long long)packet->timestamp,
                packet->data_len > 0 ? packet->data[0] : 0,
                packet->data_len > 1 ? packet->data[1] : 0,
                packet->data_len > 2 ? packet->data[2] : 0,
                packet->data_len > 3 ? packet->data[3] : 0);
        /* One real packet, kept for an offline decode test: the same bytes
         * through opusdec on this seat either decode or reproduce the failure
         * without waiting out the vendor queue again. */
        FILE *dump = fopen("/tmp/opennowlite-audio1.bin", "wb");
        if (dump) {
            fwrite(packet->data, 1, packet->data_len, dump);
            fclose(dump);
        }
    } else if (owner->audio_units % 1500u == 0u) {
        GstState audio_now = GST_STATE_VOID_PENDING;
        gst_element_get_state(owner->audio, &audio_now, NULL, 0);
        fprintf(stderr,
                "opennowlite: audio packets=%llu queued=%llu decoded_samples=%llu peak=%llu nonzero=%llu state=%d\n",
                (unsigned long long)owner->audio_units,
                (unsigned long long)gst_app_src_get_current_level_bytes(owner->audio_source),
                (unsigned long long)owner->audio_samples,
                (unsigned long long)owner->audio_peak,
                (unsigned long long)owner->audio_nonzero,
                (int)audio_now);
        owner->audio_peak = 0;
        owner->audio_nonzero = 0;
        owner->audio_samples = 0;
    }
}

/* The tap's entry point: the lock is held for the whole push so the teardown
 * cannot clear the source under it. */
static void media_push_audio(Media *owner,
                             const OpenNowStreamerEncodedMedia *packet)
{
    g_mutex_lock(&media_lock);
    if (!owner->stopping && owner->audio_source)
        media_push_audio_locked(owner, packet);
    g_mutex_unlock(&media_lock);
}

/* This is the hand-off: the streamer does not decode or present anything when
 * media_passthrough is set, so these bytes are all the window ever sees. They
 * go into this program's own pipeline, which decodes with v4l2slh264dec and
 * shows the frames through waylandsink. */
static void on_encoded_media(const OpenNowStreamerEncodedMedia *packet, void *unused)
{
    (void)unused;
    switch (packet->kind) {
    case OPENNOW_STREAMER_ENCODED_KIND_VIDEO:
        media_push_video((Media *)unused, packet);
        break;
    case OPENNOW_STREAMER_ENCODED_KIND_AUDIO:
        if (media.audio_ready)
            media_push_audio((Media *)unused, packet);
        break;
    default:
        fprintf(stderr, "opennowlite: unexpected encoded codec %u\n",
                packet->codec);
        break;
    }
}

/* ---- the window's own media pipeline ---- */

/* The same decoder hand-off this layer proved for YtGst: the H.264 the streamer
 * forwards goes through h264parse into the V4L2 soft decoder and straight into
 * waylandsink, with no videoconvert between them. */
static const char *video_caps_string =
    "video/x-h264,stream-format=byte-stream,alignment=au";
/* The tap's Opus is raw (the transport already unwrapped RFC 2198 RED), so
 * opusdec takes it directly. Family 0 is the plain mono/stereo mapping, and the
 * channel count is left to the packet: pinning `channels=2` made the caps
 * disagree with a mono stream, and opusdec reports that as a stream error on
 * the first packet. Rate is 48 kHz by definition and comes from opusdec. */
static const char *audio_caps_string =
    "audio/x-opus,channel-mapping-family=0";

/* appsrc with a live, non-seeking feed: `is-live` keeps the feed from ever
 * claiming end-of-stream, `do-timestamp=false` leaves the presentation
 * timestamps put on each pushed buffer intact (1.22's `format` property is
 * about segment events, not about this), and `max-bytes` bounds the internal
 * queue. `block` stays false, and that is the one thing here that must not be
 * guessed at: with it true, a push past the bound blocks - and the push is
 * made from the streamer's own media-callback thread, so a full queue stops
 * the feed for the rest of the session. Measured on the r0.3 session that ran
 * with it true: the picture froze after 7 min, the thread named
 * `opennow-media-c` sat in a `g_cond_wait` inside `libgstapp-1.0.so`
 * (`gdb` on the live process), the tallies stayed at
 * `video units=25200`/`audio packets=85500` for the next 40 min, and the
 * streamer's own `NVST rx-stats` line kept counting 35808 frames per 600 s -
 * the vendor kept sending and the window never got it. A refused push is
 * counted instead, and the queue's level is logged beside the tally so a
 * decoder that falls behind reads as a growing `queued=` number and a stream
 * of refusals rather than as a frozen picture. */
static bool media_source_new(Media *owner, const char *caps_string,
                            guint max_bytes, gboolean is_audio)
{
    GstElement *element = gst_element_factory_make("appsrc", NULL);
    if (!element)
        return false;
    GstAppSrc *source = GST_APP_SRC_CAST(element);
    GstCaps *caps = gst_caps_from_string(caps_string);
    if (!caps) {
        gst_object_unref(element);
        return false;
    }
    gst_app_src_set_caps(source, caps);
    gst_caps_unref(caps);
    /* `format` is the one that mattered: an appsrc defaults to GST_FORMAT_BYTES,
     * so its segment event says "bytes" and GstAudioDecoder ignores that
     * segment and returns an error - measured on the seat: the same 89-byte
     * packet decoded fine through opusdec when pushed with a time segment, and
     * produced `appsrc1: Internal data stream error` (streaming stopped,
     * reason error -5) with the default. The PTS on these buffers is already
     * nanoseconds, so time is also the honest format. */
    g_object_set(element, "is-live", TRUE, "do-timestamp", FALSE,
                 "block", FALSE, "max-bytes", (guint64)max_bytes,
                 "format", GST_FORMAT_TIME, NULL);
    if (is_audio) {
        owner->audio_source = source;
        return true;
    }
    owner->video_source = source;
    return true;
}

/* libpulse names the server through $XDG_RUNTIME_DIR/pulse/native, and this
 * window runs with XDG_RUNTIME_DIR=/home/weston because that is where weston
 * keeps its Wayland socket on this seat, so the default lookup finds nothing:
 * the seat's live PipeWire-Pulse socket is in /run/user/<uid>/pulse/native.
 * Measured on the r26 round of Nov 8, the audio chain came up and every Opus
 * packet was carried and counted (`audio packets=219000 queued=313`), yet the
 * seat's only sink, `alsa_output.platform-sound.hdmi-stereo`, stayed
 * `SUSPENDED` for the whole session and the user heard nothing at all, while
 * the same session's picture ran 59 s of frames a second. The client's own log
 * has no complaint about it because the failure is a warning on the audio bus
 * far downstream of the tap; running `audiotestsrc ! autoaudiosink` in the
 * window's exact environment printed `Failed to connect: Connection refused`
 * from that same autoaudiosink, and in `/run/user/1000` it made the sink
 * `RUNNING` and the beeps were heard, so the seat plays audio and only this
 * window's sink was pointed at a socket that is not there. Naming the live one
 * fixes it from the environment the window already has: PULSE_SERVER=unix:/run/
 * user/<uid>/pulse/native connected, which is also what `pactl` on the seat
 * resolves to. Only done when neither PULSE_SERVER nor PULSE_SOCKET was given,
 * and only when the default socket really is missing. */
static void pulse_server_default(void)
{
    if (getenv("PULSE_SERVER") || getenv("PULSE_SOCKET"))
        return;
    const char *dir = getenv("XDG_RUNTIME_DIR");
    char path[256];
    struct stat st;
    snprintf(path, sizeof(path), "%s/pulse/native", dir && *dir ? dir : "/tmp");
    if (stat(path, &st) == 0)
        return;
    snprintf(path, sizeof(path), "/run/user/%u/pulse/native",
             (unsigned)getuid());
    if (stat(path, &st) != 0)
        return;
    char server[320];
    snprintf(server, sizeof(server), "unix:/run/user/%u/pulse/native",
             (unsigned)getuid());
    g_setenv("PULSE_SERVER", server, FALSE);
    fprintf(stderr, "opennowlite: pulse server set to %s\n", server);
}

/* The audio leg, decoded here through the seat's own libopus instead of through
 * the `opusdec` element. Measured on the r28 round of Nov 8: the sink said it
 * was RUNNING next to the client, while the card's own PipeWire monitor
 * recorded exact zeros for twelve seconds (`nonzero=0 peak=0`), against
 * `peak=-8.0 dBFS rms=-11.2 dBFS` for an `audiotestsrc ! pulsesink` test in the
 * same place, which the user heard. So every Opus frame the client handed the
 * chain (`audio packets=70500 queued=147`, refused=0) never came out of
 * opusdec: the sink sat in preroll, the appsrc queue grew to 11.4 MB over
 * sixteen minutes, and the seat's speakers got silence. The stage-2 client fed
 * opusdec through an `opusparse` element, but that element is not built on this
 * seat (`gst-inspect-1.0` lists opusdec and opusenc only), and the parser was
 * dropped in r1.4 on the belief that one packet per buffer made it unnecessary.
 * The frames themselves are sound: the offline decode of this same tap read
 * 106101 of 106101 packets with 0 failures. So decode them here, with the same
 * libopus and the same call shapes opusprobe proved (ask each packet's length
 * first - opus_decode answers -1 for a frame_size of 0), and hand the sink the
 * raw shape the seat's own audio test proved audible: S16LE, 2 channels,
 * 48 kHz. */
static bool audio_opus_prepare(Media *owner)
{
    if (owner->opus_dec)
        return true;
    if (!owner->opus_lib) {
        owner->opus_lib = dlopen("libopus.so.0", RTLD_NOW | RTLD_GLOBAL);
        if (!owner->opus_lib) {
            fprintf(stderr, "opennowlite: audio decode unavailable: %s\n",
                    dlerror());
            return false;
        }
    }
    /* the return type is a pointer: declaring it `int` truncated it to 32 bits
     * (harmless on this non-PIE build, whose heap is below 4 GiB, but undefined
     * behaviour everywhere else). */
    void *(*create)(int, int, int *) =
        (void *(*)(int, int, int *))dlsym(owner->opus_lib,
                                          "opus_decoder_create");
    owner->opus_nb_samples = (int (*)(const unsigned char *, int, int))
        dlsym(owner->opus_lib, "opus_packet_get_nb_samples");
    owner->opus_decode = (int (*)(void *, const unsigned char *, int, short *,
                                  int, int))
        dlsym(owner->opus_lib, "opus_decode");
    if (!create || !owner->opus_nb_samples || !owner->opus_decode) {
        fprintf(stderr,
                "opennowlite: audio decode unavailable: libopus.so.0 is missing a symbol\n");
        return false;
    }
    int err = 0;
    owner->opus_dec = create(48000, 2, &err);
    if (!owner->opus_dec || err != 0) {
        fprintf(stderr,
                "opennowlite: audio decode unavailable: opus_decoder_create err=%d\n",
                err);
        owner->opus_dec = NULL;
        return false;
    }
    return true;
}

/* Elements are probed rather than assumed: an absent Opus element means this
 * window simply has no audio, and saying so in the log is better than
 * refusing to show the game. This seat's libgstopus.so (the
 * gstreamer1.0-plugins-base-opus split package) ships only the decoder: the
 * parser element upstream pairs with it is not built here, and is not needed,
 * because every NVST audio packet is already one complete Opus frame and the
 * appsrc hands them to opusdec one at a time. */
static bool stream_new(struct lite *a)
{
    GstElementFactory *opusdec_factory = gst_element_factory_find("opusdec");
    media.audio_ready = opusdec_factory != NULL;
    if (opusdec_factory)
        gst_object_unref(opusdec_factory);
    if (!media.audio_ready)
        fprintf(stderr, "opennowlite: audio pipeline unavailable (no opusdec)\n");

    GstElement *vqueue = gst_element_factory_make("queue", NULL);
    GstElement *vparse = gst_element_factory_make("h264parse", NULL);
    GstElement *vdecode = gst_element_factory_make("v4l2slh264dec", NULL);
    GstElement *vsink = gst_element_factory_make("waylandsink", NULL);
    if (!media_source_new(&media, video_caps_string, 16u << 20, FALSE) ||
        !vqueue || !vparse || !vdecode || !vsink) {
        if (vqueue)
            gst_object_unref(vqueue);
        if (vparse)
            gst_object_unref(vparse);
        if (vdecode)
            gst_object_unref(vdecode);
        if (vsink)
            gst_object_unref(vsink);
        fprintf(stderr, "opennowlite: video chain could not be built\n");
        return false;
    }

    /* `sync` is the fix for the frozen picture, and the measurement is the
     * client's own feed: the window and its pipeline go PLAYING when the
     * session is ready, but the vendor's prepare then runs for one to two
     * minutes before the first encoded unit is handed over, so the sink's
     * clock started long before the first frame existed. With the default
     * sync=true, a gap of 1 s between PLAYING and the first push leaves the
     * decoder at 3 output pictures while the tap keeps feeding it 60 units a
     * second, and a gap of 3 s or 10 s leaves it at 2; the same feed with
     * `sync` off renders 239 of the 240 units it was given. The sink reads
     * every buffer as minutes late, and its QoS drops then starve the
     * stateless decoder. The audio sink is off the clock for the same reason
     * (its first buffer is ~100 s late), and a live stream is paced by the
     * wire, not by a clock that started before the first frame existed. */
    g_object_set(vsink, "sync", FALSE, "qos", FALSE, NULL);
    media.video = gst_pipeline_new("opennowlite-video");
    if (!media.video) {
        gst_object_unref(media.video_source);
        media.video_source = NULL;
        gst_object_unref(vqueue);
        gst_object_unref(vparse);
        gst_object_unref(vdecode);
        gst_object_unref(vsink);
        return false;
    }
    gst_bin_add_many(GST_BIN(media.video), (GstElement *)media.video_source,
                     vqueue, vparse, vdecode, vsink, NULL);
    if (!gst_element_link_many((GstElement *)media.video_source, vqueue, vparse,
                               vdecode, vsink, NULL)) {
        fprintf(stderr, "opennowlite: video chain could not be linked\n");
        gst_element_set_state(media.video, GST_STATE_NULL);
        gst_object_unref(media.video);
        media.video = NULL;
        media.video_source = NULL;
        return false;
    }

    /* The window is attached before PLAYING: the sink needs a configured
     * surface and a render rectangle, otherwise the picture gets its own
     * window and this program's surface never gets a buffer. */
    GstContext *context = gst_wl_display_handle_context_new(a->display);
    gst_element_set_context(vsink, context);
    gst_context_unref(context);
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(vsink),
                                        (guintptr)a->surface);
    gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(vsink), 0, 0,
                                           a->width, a->height);
    /* Count what the decoder hands the sink: the tap counts units, this counts
     * presented buffers, so a starved decoder is visible without a recording. */
    GstPad *video_pad = gst_element_get_static_pad(vsink, "sink");
    if (video_pad) {
        gst_pad_add_probe(video_pad, GST_PAD_PROBE_TYPE_BUFFER, on_video_frame,
                          &media, NULL);
        gst_object_unref(video_pad);
    }
    a->bus = gst_element_get_bus(media.video);
    /* stage 1's teardown already nulls and unrefs a->pipeline, so it is
     * kept as the same object here. The sink belongs to the pipeline's bin
     * and must never be freed on its own, so a->sink stays null. */
    a->pipeline = (GstElement *)media.video;
    a->sink = NULL;

    if (gst_element_set_state(media.video, GST_STATE_PLAYING)
        == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "opennowlite: video chain refused PLAYING\n");
        return false;
    }

    if (!media.audio_ready)
        return true;

    pulse_server_default();

    const bool audio_raw = audio_opus_prepare(&media);
    /* Raw S16LE is the shape the seat's own `audiotestsrc ! pulsesink` test
     * proved audible: the card's monitor carried peak -8.0 dBFS for it and
     * exact zeros for the opusdec chain. The Opus caps stay for the fallback
     * where libopus is not loadable. */
    const char *audio_in_caps = audio_raw
        ? "audio/x-raw,format=S16LE,layout=interleaved,channels=2,rate=48000"
        : audio_caps_string;
    if (!media_source_new(&media, audio_in_caps, 4u << 20, TRUE)) {
        media.audio_ready = false;
        return true;
    }
    GstElement *decode = audio_raw
        ? NULL : gst_element_factory_make("opusdec", NULL);
    /* `pulsesink` by name, not through `autoaudiosink`: the seat's own working
     * audio test is `audiotestsrc ! ... ! audioconvert ! pulsesink`, and this
     * window's first buffer carries a PTS of zero roughly a hundred seconds
     * after the pipeline went PLAYING (the vendor's prepare happens after the
     * window is built), so a clock-synced sink sees every buffer as late. A
     * live audio stream should be paced by the device, not by a clock the first
     * buffer is a hundred seconds behind, so `sync` is off. */
    GstElement *audiosink = gst_element_factory_make("pulsesink", NULL);
    if (!audiosink)
        audiosink = gst_element_factory_make("autoaudiosink", NULL);
    if (audiosink)
        g_object_set(audiosink, "sync", FALSE, NULL);
    GstElement *aconv = gst_element_factory_make("audioconvert", NULL);
    /* A bounded queue in front of the decoder: the tap pushes one Opus frame
     * every 5 ms, and if the sink ever stops consuming (preroll that does not
     * finish, a device that disappears) the appsrc's own queue grew without
     * bound - measured at 11.4 MB after 16 minutes while the sink sat in
     * gst_base_sink_wait_preroll. A downstream-leaky queue keeps at most 200
     * frames (one second of audio) and drops old ones instead of growing, so a
     * broken sink costs one second of sound, not the process's memory. */
    GstElement *queue = gst_element_factory_make("queue", NULL);
    if (queue)
        g_object_set(queue, "leaky", 2, "max-size-buffers", 200u,
                     "max-size-bytes", (guint64)0,
                     "max-size-time", (guint64)0, NULL);
    if (!audiosink || !aconv || !queue || (!audio_raw && !decode)) {
        if (decode)
            gst_object_unref(decode);
        if (audiosink)
            gst_object_unref(audiosink);
        if (aconv)
            gst_object_unref(aconv);
        if (queue)
            gst_object_unref(queue);
        gst_object_unref(media.audio_source);
        media.audio_source = NULL;
        media.audio_ready = false;
        return true;
    }
    media.audio = gst_pipeline_new("opennowlite-audio");
    if (!media.audio) {
        if (decode)
            gst_object_unref(decode);
        gst_object_unref(audiosink);
        gst_object_unref(aconv);
        gst_object_unref(queue);
        gst_object_unref(media.audio_source);
        media.audio_source = NULL;
        media.audio_ready = false;
        return true;
    }
    if (audio_raw) {
        gst_bin_add_many(GST_BIN(media.audio), (GstElement *)media.audio_source,
                         queue, aconv, audiosink, NULL);
    } else {
        gst_bin_add_many(GST_BIN(media.audio), (GstElement *)media.audio_source,
                         queue, decode, aconv, audiosink, NULL);
    }
    const gboolean audio_linked = audio_raw
        ? gst_element_link_many((GstElement *)media.audio_source, queue, aconv,
                                audiosink, NULL)
        : gst_element_link_many((GstElement *)media.audio_source, queue, decode,
                                aconv, audiosink, NULL);
    if (!audio_linked) {
        fprintf(stderr, "opennowlite: audio chain could not be linked\n");
        gst_element_set_state(media.audio, GST_STATE_NULL);
        gst_object_unref(media.audio);
        media.audio = NULL;
        media.audio_source = NULL;
        media.audio_ready = false;
        return true;
    }
    /* The bus is kept so the main loop can report the audio chain's errors.
     * Before this the audio pipeline's bus was never read, so a caps
     * negotiation failure on opusdec looked exactly like a sink that never
     * prerolled: no message, no sound, a growing appsrc. */
    media.audio_bus = gst_element_get_bus(media.audio);
    GstStateChangeReturn audio_change =
        gst_element_set_state(media.audio, GST_STATE_PLAYING);
    GstState audio_state = GST_STATE_VOID_PENDING;
    GstStateChangeReturn audio_reached =
        gst_element_get_state(media.audio, &audio_state, NULL, 2 * GST_SECOND);
    fprintf(stderr,
            "opennowlite: audio chain %s, pipeline ready set=%d reached=%d state=%d\n",
            audio_raw ? "libopus->S16LE/2ch/48000" : "opusdec",
            (int)audio_change, (int)audio_reached, (int)audio_state);
    return true;
}

/* ---- starting the engine ---- */

static bool engine_start(const char *variant_id)
{
    streamer_load();
    if (!streamer.loaded)
        return false;

    OpenNowStreamerConfig config;
    memset(&config, 0, sizeof(config));
    config.abi_version = OPENNOW_STREAMER_FFI_ABI_VERSION;
    config.struct_size = sizeof(config);
    /* The start line carries the whole NVST context, so the command buffer has
     * to be far larger than a plain RPC line. */
    config.command_queue_capacity = 64u;
    config.response_queue_capacity = 64u;
    config.event_queue_capacity = 64u;
    config.max_command_bytes = 256u << 10;
    config.response_callback = on_stream_reply;
    config.event_callback = on_stream_event;
    config.frame_available_callback = on_frame_available;
    config.user_data = &media;
    /* No Vulkan device: nothing is rendered here. The media pipeline owns the
     * picture, so the streamer must not start its own decoder either. */
    config.vulkan_device = NULL;
    config.windows_adapter_luid = 0u;
    config.media_passthrough = true;
    config.encoded_media_callback = on_encoded_media;

    OpenNowStreamerStatus status = streamer.create(&config, &streamer.engine);
    if (status != OPENNOW_STREAMER_OK) {
        fprintf(stderr, "opennowlite: engine refused (%d)\n", (int)status);
        streamer.engine = NULL;
        return false;
    }

    /* The NVST handshake is the streamer's own RPC: hello first, then start,
     * whose context is exactly what the core negotiated for this session. */
    char *line = malloc(1u << 16);
    if (!line)
        return false;
    snprintf(line, 1u << 16,
        "{\"id\":\"lite-hello\",\"type\":\"hello\",\"protocolVersion\":8}");
    status = streamer.send(streamer.engine, (const uint8_t *)line,
                          strlen(line));
    if (status != OPENNOW_STREAMER_OK) {
        fprintf(stderr, "opennowlite: nvst hello refused (%d)\n", (int)status);
        free(line);
        return false;
    }
    snprintf(line, 1u << 16,
        "{\"id\":\"lite-start\",\"type\":\"start\",\"context\":%s}",
        session_context);
    status = streamer.send(streamer.engine, (const uint8_t *)line,
                          strlen(line));
    free(line);
    if (status != OPENNOW_STREAMER_OK) {
        fprintf(stderr, "opennowlite: nvst start refused (%d)\n", (int)status);
        return false;
    }

    /* Capture is this program's: pointer lock is done here over the seat's own
     * wl_pointer, so the streamer must not take over the raw X11 path, which
     * the seat has no use for. */
    bool raw_input = false;
    status = streamer.capture(streamer.engine, true, false, 0u, &raw_input);
    if (status != OPENNOW_STREAMER_OK) {
        fprintf(stderr, "opennowlite: capture refused (%d)\n", (int)status);
        return false;
    }
    streamer.capture_active = true;
    clock_gettime(CLOCK_MONOTONIC, &app.capture_started);
    fprintf(stderr, "opennowlite: engine started for %s\n", variant_id);
    return true;
}

static void engine_stop(void)
{
    if (!streamer.engine)
        return;
    static const char stop[] = "{\"id\":\"lite-stop\",\"type\":\"stop\"}";
    streamer.send(streamer.engine, (const uint8_t *)stop, strlen(stop));
    OpenNowStreamerStatus status = streamer.destroy(streamer.engine);
    if (status != OPENNOW_STREAMER_OK)
        fprintf(stderr, "opennowlite: engine teardown %d\n", (int)status);
    streamer.engine = NULL;
    streamer.capture_active = false;
    fprintf(stderr, "opennowlite: engine stopped\n");
}

/* ---- pointer state and the seat's input adapters ---- */

/* The pointer is absolute, which is what OpenNOW itself does by default. The
 * remote tells the client so: the streamer logs a cursor notification
 * (`NVST cursor capture tx: command=0x0308 ... reason=cursor-notification`),
 * and OpenNOW only switches to relative motion through its opt-in `relativeMouse`
 * QML setting. The previous round of this client sent relative deltas, and the
 * measured result on the seat was conclusive: 100 pointer buttons and 28 keys
 * reached the FFI with no refusal, the seat's own arrow moved, and nothing in
 * the stream ever did - the picture already carries its own remote cursor,
 * whose place is a coordinate rather than a distance. Upstream's absolute path
 * publishes a position on hover, on every button transition and on wheel, so
 * the vendor's hit-testing and its clicks agree. */

/* weston's wl_pointer reports the cursor inside the surface it hovers, so the
 * numbers only mean something while that surface is this window. Their extent
 * travels in the same packet as the coordinate, which is what the vendor
 * normalises by, and this window keeps the video's aspect ratio, so window
 * pixels need no rescaling. The FFI rejects a zero extent. */
static struct Pointer {
    double last_x;
    double last_y;
    int32_t wheel;
} pointer_state;

static void pointer_submit(struct lite *a, double x, double y)
{
    if (!streamer.engine || !streamer.capture_active)
        return;
    if (a->width < 1 || a->height < 1)
        return;
    uint32_t extent_x = (uint32_t)a->width;
    uint32_t extent_y = (uint32_t)a->height;
    if (x < 0.0)
        x = 0.0;
    if (y < 0.0)
        y = 0.0;
    if (x > (double)(extent_x - 1u))
        x = (double)(extent_x - 1u);
    if (y > (double)(extent_y - 1u))
        y = (double)(extent_y - 1u);
    /* u16 coordinates, so an oversized window cannot silently wrap around. */
    if (extent_x > 65535u)
        extent_x = 65535u;
    if (extent_y > 65535u)
        extent_y = 65535u;
    struct timespec at;
    bool timed = debug_input && input_timed < 400u;
    if (timed)
        clock_gettime(CLOCK_MONOTONIC, &at);
    OpenNowStreamerStatus status =
        streamer.absolute(streamer.engine,
                          (uint16_t)(x + 0.5), (uint16_t)(y + 0.5),
                          (uint16_t)extent_x, (uint16_t)extent_y);
    if (timed)
        note_submit("position", &at);
    if (status == OPENNOW_STREAMER_OK) {
        ++a->submits;
    } else if (a->input_refused++ == 0u) {
        fprintf(stderr, "opennowlite-input: position submit refused (%d)\n",
                (int)status);
    }
}

static void pointer_scroll(struct lite *a)
{
    if (!streamer.engine || !streamer.capture_active)
        return;
    if (!pointer_state.wheel)
        return;
    /* A wheel step is 120 units of a turn on weston, on Windows (WHEEL_DELTA)
     * and in GFN's packet: the vendor's Windows raw path forwards usButtonData
     * straight through and its QML path passes Qt's angleDelta unchanged. The
     * old round divided by 120 and negated the result, which made the scroll
     * invisible in the stream. */
    int32_t value = pointer_state.wheel;
    pointer_state.wheel = 0;
    if (value > 32767)
        value = 32767;
    if (value < -32767)
        value = -32767;
    OpenNowStreamerStatus status =
        streamer.wheel(streamer.engine, 0, (int16_t)value);
    if (status == OPENNOW_STREAMER_OK) {
        ++a->submits;
    } else if (a->input_refused++ == 0u) {
        fprintf(stderr, "opennowlite-input: wheel submit refused (%d)\n",
                (int)status);
    }
}

static bool quit_key(uint32_t evdev, uint32_t modifier_mask)
{
    /* Ctrl+Alt+Shift: xkb bits 0 (shift), 2 (control) and 3 (alt). The old
     * mask 0x7 asked for shift, caps-lock and control, so the exit shortcut
     * could never fire. */
    return evdev == 16 && (modifier_mask & 0x0du) == 0x0du;
}

static void seat_key(uint32_t key_code, bool pressed)
{
    uint32_t evdev = key_code >= 8 ? key_code - 8 : 0;
    if (quit_key(evdev, app.modifier_mask)) {
        running = 0;
        return;
    }
    if (!streamer.engine || !streamer.capture_active)
        return;
    uint16_t virtual_key = linux_virtual_key(evdev);
    if (!virtual_key) {
        if (app.input_refused++ == 0u)
            fprintf(stderr, "opennowlite-input: no virtual key for evdev=%u\n",
                    evdev);
        return;
    }
    struct timespec at;
    bool timed = debug_input && input_timed < 400u;
    if (timed)
        clock_gettime(CLOCK_MONOTONIC, &at);
    OpenNowStreamerStatus status = streamer.key(streamer.engine, virtual_key,
                                                modifier_bits(app.modifier_mask),
                                                pressed);
    if (timed)
        note_submit("key", &at);
    if (status == OPENNOW_STREAMER_OK) {
        ++app.submits;
    } else if (app.input_refused++ == 0u) {
        fprintf(stderr, "opennowlite-input: key submit refused (%d)\n",
                (int)status);
    }
}

/* The seat is a TV on a wall: when nobody is at a keyboard, the client can
 * drive the cloud game's own notices. The measured shape of that chain is not
 * one press a few seconds apart but one press per notice, ~135 s apart, and a
 * clock that presses more than once per notice closes the host's own windows
 * and leaves a bare desktop. The offsets this client arms by default are the
 * measured ones: the consent notice answers a Return at +63 s and the splash's
 * PRESS ENTER TO PLAY at +212 s. Keeping the host's ~14 minute idle timeout
 * down with one Enter a minute was measured to fail - on the r1.3 round of Nov
 * 7 the Escape armed for the SIGN UP notice opened Destiny 2's own `ARE YOU
 * SURE? ... quit your current session and exit to desktop` over the character
 * screen, what confirmed it put the session back on the Steam desktop, and the
 * vendor then sent no media at all for the following ten minutes (`NVST rx-
 * stats elapsed=320.1s ... frames=18298` was the last line) while the session
 * stayed alive and kept answering input - a window holding one frame reads as
 * no picture rather than as a slow one. So every event after the notice list
 * is a pointer jiggle, which is activity the host counts and that no menu can
 * act on. Return is VK 0x0d and Escape 0x1b from upstream's specials (the ad-
 * hoc evdev table this client used to have had no Return at all, so Enter was
 * silently dropped); one step of a press runs per pass of the main loop, so a
 * hold lasts a second rather than zero time. The list is sized for a dense keep
 * clock rather than the old once-a-minute one: a free-tier hour is 3600 s, so
 * keeping the host's idle timer down every 5 s of that takes 720 events on top
 * of the two notice answers, and a keep clock that thin (88 positions over the
 * r1.4 round of Nov 7) was measured to lose the picture after 1720 s of media
 * while the r1.2 round's 1734 positions held a full hour of it. */
static long pilot_press_at[1200];
static uint16_t pilot_press_key[1200];
static unsigned long pilot_press_count;

static void pilot_send(struct lite *a)
{
    if (!a->auto_pilot || !streamer.engine || !streamer.capture_active)
        return;
    if (pilot_press_count == 0u)
        return;
    long elapsed = ms_since(&a->capture_started) / 1000L;
    unsigned long press = a->pilot_steps / 2u;
    if (press >= pilot_press_count) {
        if (!a->pilot_click || a->pilot_steps > 2u * pilot_press_count + 2u)
            return;
        /* then, if asked for, one left click at a stated point: in absolute
         * mode a click cannot land without a position, so the position goes
         * first and the vendor keeps the order. */
        unsigned long extra = a->pilot_steps - 2u * pilot_press_count;
        long due = pilot_press_at[pilot_press_count - 1u] + 3L
                 + (long)extra;
        if (elapsed < due)
            return;
        if (extra == 0u) {
            double point_x = (double)a->pilot_point_x;
            double point_y = (double)a->pilot_point_y;
            if (point_x <= 0.0)
                point_x = (double)a->width / 2.0;
            if (point_y <= 0.0)
                point_y = (double)a->height / 2.0;
            pointer_submit(a, point_x, point_y);
        } else {
            bool pressed = extra == 1u;
            struct timespec click_at;
            bool click_timed = debug_input && input_timed < 400u;
            if (click_timed)
                clock_gettime(CLOCK_MONOTONIC, &click_at);
            OpenNowStreamerStatus status =
                streamer.button(streamer.engine, 1u, pressed);
            if (click_timed)
                note_submit("click", &click_at);
            if (status == OPENNOW_STREAMER_OK)
                ++a->submits;
            else if (a->input_refused++ == 0u)
                fprintf(stderr, "opennowlite-pilot: click refused (%d)\n",
                        (int)status);
        }
        ++a->pilot_steps;
        return;
    }
    bool pressed = (a->pilot_steps % 2u) == 0u;
    long due = pilot_press_at[press] + (pressed ? 0L : 1L);
    if (elapsed < due)
        return;
    if (pilot_press_key[press] == 0u) {
        /* A keep event is a pointer move, not a key. Measured on the r1.3 round
         * of Nov 7: the Escape the SIGN UP notice was answered with opened
         * Destiny 2's own `ARE YOU SURE? ... quit your current session and exit
         * to desktop` on the character screen, and what confirmed it sent the
         * session back to the Steam desktop (the witness reads the store page at
         * media +299 s), after which the vendor's media stopped altogether
         * (`NVST rx-stats elapsed=320.1s ... frames=18298` was the last one)
         * while the session stayed alive and kept answering input. A window that
         * holds one frame for ten minutes reads as no picture at all, which is
         * what it looked like on the TV. A pointer move counts as activity for
         * the host's idle timer and lands on nothing that a menu acts on, so the
         * keep clock moves the cursor a few pixels either side of the middle. */
        if (pressed) {
            double x = (double)(a->width / 2 + (press % 2u ? 40 : -40));
            double y = (double)(a->height / 2);
            pointer_submit(a, x, y);
            ++a->pilot_jiggles;
            fprintf(stderr, "opennowlite-pilot: jiggle %lu x=%.0f y=%.0f at +%lds due=+%lds\n",
                    a->pilot_jiggles, x, y, elapsed, due);
        }
        ++a->pilot_steps;
        return;
    }
    if (pilot_press_key[press] == 0x100u) {
        /* an entry armed with `:c` is a left click rather than a key: what the
         * character screen of the r20 round acted on was the platform's Select
         * and a click, and in absolute mode a click cannot land without a
         * position, so the position goes first and the vendor keeps the order.
         * --pilot-click's single click at the end of the list already does this
         * pairing; the list just lets it happen at a stated second. */
        double point_x = (double)a->pilot_point_x;
        double point_y = (double)a->pilot_point_y;
        if (point_x <= 0.0)
            point_x = (double)a->width / 2.0;
        if (point_y <= 0.0)
            point_y = (double)a->height / 2.0;
        if (pressed)
            pointer_submit(a, point_x, point_y);
        struct timespec at;
        bool timed = debug_input && input_timed < 400u;
        if (timed)
            clock_gettime(CLOCK_MONOTONIC, &at);
        OpenNowStreamerStatus status =
            streamer.button(streamer.engine, 1u, pressed);
        if (timed)
            note_submit("click", &at);
        if (status == OPENNOW_STREAMER_OK)
            ++a->submits;
        else if (a->input_refused++ == 0u)
            fprintf(stderr, "opennowlite-pilot: click refused (%d)\n",
                    (int)status);
        ++a->pilot_steps;
        if (!pressed)
            return;
        ++a->pilot_presses;
        fprintf(stderr,
                "opennowlite-pilot: click %lu x=%.0f y=%.0f at +%lds due=+%lds\n",
                a->pilot_presses, point_x, point_y, elapsed, due);
        return;
    }
    struct timespec at;
    bool timed = debug_input && input_timed < 400u;
    if (timed)
        clock_gettime(CLOCK_MONOTONIC, &at);
    OpenNowStreamerStatus status =
        streamer.key(streamer.engine, pilot_press_key[press], 0u, pressed);
    if (timed)
        note_submit("press", &at);
    if (status == OPENNOW_STREAMER_OK)
        ++a->submits;
    else if (a->input_refused++ == 0u)
        fprintf(stderr, "opennowlite-pilot: press refused (%d)\n",
                (int)status);
    ++a->pilot_steps;
    if (!pressed)
        return;
    ++a->pilot_presses;
    fprintf(stderr, "opennowlite-pilot: press %lu vk=0x%02x at +%lds due=+%lds\n",
            a->pilot_presses, (unsigned)pilot_press_key[press], elapsed, due);
}

/* Nothing is swallowed on the first click any more: absolute mode has no lock
 * to negotiate, and the seat's own arrow stays visible because it is the thing
 * the user is actually pointing with. */
static void seat_motion(struct lite *a, double x, double y)
{
    pointer_state.last_x = x;
    pointer_state.last_y = y;
    if (a->pointer_surface != a->surface) {
        /* The seat said the cursor is over some other window or the desktop,
         * where its coordinates belong to that other surface. */
        if (a->pointer_skipped++ == 0u)
            fprintf(stderr, "opennowlite-input: pointer outside the window ignored\n");
        return;
    }
    pointer_submit(a, x, y);
}

/* weston numbers buttons by their evdev code, not 1/2/3: a wl_pointer.button
 * event carries BTN_LEFT 0x110 (272), BTN_RIGHT 0x111 (273) and BTN_MIDDLE
 * 0x112 (274). The streamer then wants X11 numbers, not GFN's zero-based field:
 * its own Windows raw-input path maps left to 1, middle to 2 and right to 3,
 * and its FFI rejects anything outside 1..=5 with INVALID_CONFIG - measured on
 * the seat: sending 0 for the left button produced `button submit refused (2)`. */
static void seat_button(struct lite *a, uint32_t button, uint32_t state)
{
    bool pressed = state == 1;
    uint8_t mapped;
    switch (button) {
    case 272: mapped = 1u; break;   /* BTN_LEFT   -> X11/OpenNOW 1 */
    case 274: mapped = 2u; break;   /* BTN_MIDDLE -> 2 */
    case 273: mapped = 3u; break;   /* BTN_RIGHT  -> 3 */
    default:
        return;
    }
    if (!streamer.engine || !streamer.capture_active)
        return;
    /* Upstream sends the position and then the button, on a release as well as
     * a press, because no move event is guaranteed before a click and both
     * have to keep their queue order. */
    if (a->pointer_surface == a->surface)
        pointer_submit(a, pointer_state.last_x, pointer_state.last_y);
    struct timespec at;
    bool timed = debug_input && input_timed < 400u;
    if (timed)
        clock_gettime(CLOCK_MONOTONIC, &at);
    OpenNowStreamerStatus status =
        streamer.button(streamer.engine, mapped, pressed);
    if (timed)
        note_submit("button", &at);
    if (status == OPENNOW_STREAMER_OK) {
        ++a->submits;
    } else if (a->input_refused++ == 0u) {
        fprintf(stderr, "opennowlite-input: button submit refused (%d)\n",
                (int)status);
    }
}

static void seat_wheel(struct lite *a, int32_t value120)
{
    pointer_state.wheel += value120;
    if (a->pointer_surface == a->surface)
        pointer_submit(a, pointer_state.last_x, pointer_state.last_y);
    pointer_scroll(a);
}

/* The notice answers are one list of seconds, each optionally followed by the
 * key that notice names: `r` Return, `e` Escape. The SIGN UP notice the PLAY
 * press opens reads `Esc Not Now` beside `Sign Me Up`, and the other option
 * opens Bungie.net in the host's browser over the picture - measured with the
 * Enter clock, which is why the default list ends in `e`. */
static unsigned long pilot_list_parse(const char *list)
{
    unsigned long count = 0u;
    while (*list && count < 1200u) {
        long value = strtol(list, NULL, 10);
        if (value < 0)
            value = 0;
        const char *comma = strchr(list, ',');
        const char *stop = comma ? comma : list + strlen(list);
        /* an entry with no suffix is a pointer move, which is what a keep
         * clock wants: measured on the r20 round of Nov 8, an entry armed as
         * a plain second presses Return, and once the game is at its
         * character screen Return is the game's own text chat, so 250 of them
         * every --pilot-gap seconds typed newlines into a chat box instead of
         * driving anything. */
        uint16_t key = 0x00u;
        const char *colon = strchr(list, ':');
        if (colon && colon < stop) {
            switch (colon[1]) {
            case 'e':
                key = 0x1bu;
                break;
            case 'r':
                key = 0x0du;
                break;
            /* measured on the r20 round of Nov 8: Destiny 2's title screen
             * says `PRESS [Return] TO PLAY`, but once the game is at its
             * character screen Return is the game's own text chat (`Press
             * enter to open text chat`), so the Return armed at +217s and
             * every 5 s after typed into chat and the character screen sat
             * there from +220 s to +300 s of session. What that screen acts
             * on is Space, the platform's Select, and a click. */
            case 's':
                key = 0x20u;
                break;
            /* not a key: the marker pilot_send turns into a left click at
             * --pilot-point, the middle by default. */
            case 'c':
                key = 0x100u;
                break;
            default:
                return 0u;
            }
        }
        pilot_press_at[count] = value;
        pilot_press_key[count] = key;
        ++count;
        if (!comma)
            break;
        list = comma + 1;
    }
    return count;
}

/* ---- main ---- */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--launch-app-id=ID] [--data-dir DIR] [--fullscreen]\n"
            "       [--show-cursor] [--width N] [--height N] [--pipeline DESC] [--test-pattern]\n"
            "       [--auto-pilot] [--pilot-at=S] [--pilot-times=N] [--pilot-gap=S]\n"
            "       [--pilot-click] [--pilot-point=WxH]\n"
            "       [--record=FILE] [--record-seconds=S] [--record-from=S] [--poll-attempts=N]\n"
            "       [--debug-input] [--debug-audio=FILE] [--debug-audio-seconds=S]\n"
            "       [--media-dead=S]\n"
            "  the default launch opens the seat's signed-in session and streams\n"
            "  the game into the app's own Wayland window; --test-pattern, or\n"
            "  --pipeline, plays a test pattern in that same window with no\n"
            "  session at all, which is how the window and the input path are\n"
            "  proven. --data-dir only points the shipped core at another copy\n"
            "  of its data directory; by default it uses this seat's. The seat's\n"
            "  pointer is sent as an absolute position inside the window.\n"
            "  By default the seat's desktop arrow is not drawn over the window:\n"
            "  the picture already carries the game's own pointer, and weston's\n"
            "  arrow on top of the picture was a second, laggier duplicate. A\n"
            "  client that asks for no cursor over its surface maps the\n"
            "  desktop's sprite away while the seat's pointer is inside the\n"
            "  game, leaving exactly the pointer the game drew; --show-cursor\n"
            "  keeps the arrow for a screen meant to show both.\n"
            "  --poll-attempts=N caps how many times, 2 s apart, the allocated\n"
            "  session is polled while the vendor still reports it preparing;\n"
            "  300 (ten minutes) by default, since a free-tier queue wait was\n"
            "  measured past the 120 s r1.4 spent before it gave up on a launch\n"
            "  whose last poll had already started carrying rtsp endpoints.\n"
            "  --debug-input stamps every input submit with its offset inside\n"
            "  the session and the time the streamer's own call took, which is\n"
            "  how the complaint that a press of a pop-up's OK closed seconds\n"
            "  later splits: what the client held, what the accept cost, and\n"
            "  what the picture answered on the same clock the witness runs on.\n"
            "  --debug-audio also writes the first --debug-audio-seconds (60\n"
            "  by default) of the stream's Opus frames into FILE as length-\n"
            "  prefixed packets, which an offline mux turns into an Ogg Opus\n"
            "  file: sound nobody is listening to can still be measured for\n"
            "  level, pacing and gaps.\n"
            "  With --auto-pilot the app drives the cloud game's own notices: it\n"
            "  presses once per notice at --pilot-at, a comma list of seconds\n"
            "  after the stream starts, each optionally followed by the key the\n"
            "  notice names (`63:r,212:r` by default, r is Return, e Escape,\n"
            "  s Space and c a left click at --pilot-point; an entry with no\n"
            "  suffix is a pointer move).\n"
            "  A clock that presses twice per notice closes the host's own\n"
            "  windows, and keys after the notice list are measured to fail:\n"
            "  the Escape armed for the SIGN UP notice opened Destiny 2's own\n"
            "  `ARE YOU SURE? ... quit your current session and exit to\n"
            "  desktop`, and what confirmed it left the Steam desktop showing\n"
            "  and the vendor sending no media at all. So every event after the\n"
            "  list is a pointer jiggle one --pilot-gap seconds apart (60 s by\n"
            "  default), --pilot-times of them (64 by default, which is past a\n"
            "  free-tier hour from the first one at +272 s), activity the host\n"
            "  counts for its ~14 minute idle timeout and that no menu can act\n"
            "  on. With --pilot-click it also clicks once at --pilot-point, the\n"
            "  middle by default, 3 s after the last event. --record also writes\n"
            "  the first --record-seconds of picture into FILE as a raw H.264\n"
            "  stream, which is how a screen nobody is looking at can still be\n"
            "  checked afterwards, and --record-from=S starts that window S\n"
            "  seconds into the session rather than at its start. Measured on\n"
            "  the r22 rounds of Nov 8, the window that matters is not the first\n"
            "  one: every round recorded its first 420 s and showed Destiny 2's\n"
            "  loading screen from +240 s to +420 s, while its media died at\n"
            "  +930 s, +1 080 s and +1 220 s of session and its acts ran at\n"
            "  +602s, +702s and +802s - a window no witness ever covered. The\n"
            "  window is a raw stream that starts mid-GOP, so the frames up to\n"
            "  its next keyframe are not decodable.\n",
            argv0);
}

int main(int argc, char **argv)
{
    g_mutex_init(&media_lock);
    bool test_pattern = false;
    bool fullscreen = false;
    int width = 1920;
    int height = 1080;
    const char *variant_id = NULL;
    const char *data_dir = NULL;
    bool auto_pilot = false;
    bool pilot_at_given = false;
    int pilot_times = 64;
    int pilot_gap = 60;
    bool pilot_click = false;
    int pilot_point_x = 0;
    int pilot_point_y = 0;
    const char *record_path = NULL;
    int record_seconds = 120;
    int record_from_seconds = 0;
    const char *audio_dump_path = NULL;
    int audio_dump_seconds = 60;
    const char *pipeline =
        "videotestsrc is-live=true ! videoconvert ! waylandsink name=sink "
        "sync=false";

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--launch-app-id=", 16) == 0) {
            variant_id = argv[i] + 16;
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (strcmp(argv[i], "--test-pattern") == 0) {
            test_pattern = true;
        } else if (strcmp(argv[i], "--auto-pilot") == 0) {
            /* no seat input: this client drives Steam's launch dialog itself */
            auto_pilot = true;
        } else if (strncmp(argv[i], "--pilot-at=", 11) == 0) {
            pilot_press_count = pilot_list_parse(argv[i] + 11);
            if (pilot_press_count == 0u) {
                fprintf(stderr, "opennowlite: --pilot-at wants a comma list of seconds\n");
                usage(argv[0]);
                return 2;
            }
            pilot_at_given = true;
        } else if (strncmp(argv[i], "--pilot-times=", 14) == 0) {
            pilot_times = atoi(argv[i] + 14);
        } else if (strncmp(argv[i], "--pilot-gap=", 12) == 0) {
            pilot_gap = atoi(argv[i] + 12);
        } else if (strcmp(argv[i], "--pilot-click") == 0) {
            pilot_click = true;
        } else if (strncmp(argv[i], "--record=", 9) == 0) {
            /* the picture is also written out so a screen nobody is looking
             * at can still be checked afterwards */
            record_path = argv[i] + 9;
        } else if (strncmp(argv[i], "--record-seconds=", 17) == 0) {
            record_seconds = atoi(argv[i] + 17);
        } else if (strncmp(argv[i], "--record-from=", 14) == 0) {
            record_from_seconds = atoi(argv[i] + 14);
        } else if (strncmp(argv[i], "--pilot-point=", 14) == 0) {
            if (sscanf(argv[i] + 14, "%dx%d", &pilot_point_x,
                       &pilot_point_y) != 2) {
                fprintf(stderr, "opennowlite: --pilot-point wants WxH\n");
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            fullscreen = true;
        } else if (strcmp(argv[i], "--show-cursor") == 0) {
            /* keep the seat's desktop arrow over the window; by default the
             * window asks for no cursor and the game's own pointer inside
             * the picture is the only one the seat then shows */
            show_cursor = true;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pipeline") == 0 && i + 1 < argc) {
            /* only the test pattern has no session to feed the window */
            pipeline = argv[++i];
            test_pattern = true;
        } else if (strncmp(argv[i], "--poll-attempts=", 16) == 0) {
            session_poll_attempts = atoi(argv[i] + 16);
            if (session_poll_attempts < 1) {
                fprintf(stderr,
                        "opennowlite: --poll-attempts wants a positive count\n");
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--debug-input") == 0) {
            debug_input = true;
        } else if (strncmp(argv[i], "--debug-audio-seconds=", 22) == 0) {
            audio_dump_seconds = atoi(argv[i] + 22);
        } else if (strncmp(argv[i], "--debug-audio=", 14) == 0) {
            /* the sound of the stream, kept so it can be analysed after the
             * session instead of listened for during one */
            audio_dump_path = argv[i] + 14;
        } else if (strncmp(argv[i], "--media-dead=", 13) == 0) {
            /* a launch that stopped carrying media ends its own session */
            media_dead_seconds = atoi(argv[i] + 13);
            if (media_dead_seconds < 0 || media_dead_seconds > 900) {
                fprintf(stderr,
                        "opennowlite: --media-dead wants 0..900 seconds\n");
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "opennowlite: unknown argument %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (!test_pattern && !variant_id) {
        fprintf(stderr, "opennowlite: --launch-app-id is required\n");
        usage(argv[0]);
        return 2;
    }
    if (width < 64 || height < 64) {
        fprintf(stderr, "opennowlite: refusing a %dx%d window\n", width, height);
        return 2;
    }
    if (auto_pilot) {
        if (!pilot_at_given)
            pilot_press_count = pilot_list_parse("63:r,212:r");
        for (int i = 0; i < pilot_times; i++) {
            if (pilot_press_count >= 1200u)
                break;
            long last = pilot_press_at[pilot_press_count - 1u];
            pilot_press_at[pilot_press_count] = last + (long)pilot_gap;
            pilot_press_key[pilot_press_count] = 0u;
            ++pilot_press_count;
        }
        for (unsigned long i = 1u; i < pilot_press_count; i++) {
            if (pilot_press_at[i] <= pilot_press_at[i - 1u]) {
                fprintf(stderr, "opennowlite: the pilot's seconds must rise in order\n");
                return 2;
            }
        }
        fprintf(stderr, "opennowlite: pilot armed with %lu presses, first at +%lds\n",
                pilot_press_count, pilot_press_at[0u]);
    }    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    clock_gettime(CLOCK_MONOTONIC, &app.last_motion);

    gst_init(&argc, &argv);

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        fprintf(stderr, "opennowlite: no Wayland display (WAYLAND_DISPLAY=%s)\n",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
        return 1;
    }
    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);
    if (wl_display_roundtrip(app.display) < 0) {
        fprintf(stderr, "opennowlite: registry roundtrip failed\n");
        return 1;
    }
    if (!app.compositor || !app.wm_base) {
        fprintf(stderr, "opennowlite: compositor has no xdg-shell\n");
        return 1;
    }
    app.fullscreen = fullscreen;
    app.width = width;
    app.height = height;
    app.auto_pilot = auto_pilot;
    app.pilot_times = pilot_times;
    app.pilot_gap = pilot_gap;
    app.pilot_click = pilot_click;
    app.pilot_point_x = pilot_point_x;
    app.pilot_point_y = pilot_point_y;

    if (record_path) {
        if (record_seconds < 1 || record_seconds > 7200)
            record_seconds = 120;
        if (record_from_seconds < 0 || record_from_seconds > 7200)
            record_from_seconds = 0;
        media.record = fopen(record_path, "wb");
        if (!media.record) {
            fprintf(stderr, "opennowlite: cannot open the record file %s: %s\n",
                    record_path, strerror(errno));
            return 1;
        }
        media.record_units_left = (long long)record_seconds * 60LL;
        media.record_from_left = (long long)record_from_seconds * 60LL;
        if (record_from_seconds > 0)
            fprintf(stderr,
                    "opennowlite: recording %d s of picture from +%d s into %s\n",
                    record_seconds, record_from_seconds, record_path);
        else
            fprintf(stderr, "opennowlite: recording %d s of picture into %s\n",
                    record_seconds, record_path);
    }

    if (audio_dump_path) {
        if (audio_dump_seconds < 1 || audio_dump_seconds > 600)
            audio_dump_seconds = 60;
        media.audio_dump = fopen(audio_dump_path, "wb");
        if (!media.audio_dump) {
            fprintf(stderr,
                    "opennowlite-audio: cannot open the audio witness %s: %s\n",
                    audio_dump_path, strerror(errno));
            return 1;
        }
        media.audio_dump_left = (long long)audio_dump_seconds * 200LL;
        fprintf(stderr, "opennowlite-audio: witnessing %d s of sound into %s\n",
                audio_dump_seconds, audio_dump_path);
    }

    if (!create_window(&app))
        return 1;
    if (test_pattern) {
        if (!start_pipeline(&app, pipeline))
            return 1;
    } else {
        if (!session_open(variant_id, data_dir)) {
            fprintf(stderr, "opennowlite: session failed\n");
            core_shutdown(core_rpc);
            return 1;
        }
        if (!stream_new(&app) || !engine_start(variant_id)) {
            fprintf(stderr, "opennowlite: media path failed\n");
            core_shutdown(core_rpc);
            return 1;
        }
    }

    while (running) {
        if (!drain_bus(&app))
            break;
        if (!drain_audio_bus(&media))
            media_audio_teardown(&media);
        pilot_send(&app);
        media_watch();
        wl_display_dispatch_pending(app.display);
        wl_display_flush(app.display);

        struct pollfd pfd = {
            .fd = wl_display_get_fd(app.display),
            .events = POLLIN,
        };
        int ready = poll(&pfd, 1, 50);
        if (ready < 0 && errno != EINTR)
            break;
        if (ready > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_prepare_read(app.display) == 0) {
                wl_display_read_events(app.display);
                wl_display_dispatch_pending(app.display);
            } else {
                wl_display_dispatch_pending(app.display);
            }
        }
    }

    fprintf(stderr, "opennowlite: stopping keys=%lu motions=%lu submits=%lu refused=%lu skipped=%lu pilot=%lu jiggle=%lu\n",
            app.keys, app.motions, app.submits, app.input_refused,
            app.pointer_skipped, app.pilot_presses, app.pilot_jiggles);
    if (media.record) {
        const int record_failed = fclose(media.record) != 0;
        media.record = NULL;
        fprintf(stderr, "opennowlite: record closed bytes=%llu%s\n",
                (unsigned long long)media.recorded_bytes,
                record_failed ? " (fclose failed)" : "");
    }
    if (media.audio_dump) {
        const int dump_failed = fclose(media.audio_dump) != 0;
        fprintf(stderr,
                "opennowlite-audio: witness closed frames=%llu bytes=%llu%s\n",
                (unsigned long long)media.audio_dump_frames,
                (unsigned long long)media.audio_dump_bytes,
                dump_failed ? " (fclose failed)" : "");
        media.audio_dump = NULL;
    }
    /* The media pipeline and the Wayland connection go first. The vendor
     * teardown below can block for many seconds, and the pilot's SIGKILL then
     * left weston holding the decoder's dma-bufs (measured Nov 8: 120 leaked
     * videobuf2 buffers, 439 MB of the 512 MB CMA). waylandsink only destroys
     * its wl_surface and releases those buffers while the display is still
     * connected, so the sink must reach NULL before wl_display_disconnect and
     * both before the vendor teardown. */
    /* The tap runs on the streamer's thread and can still be inside a push
     * when the window is torn down; r2.5's rounds ended with
     * `gst_app_src_push_internal: assertion 'GST_IS_APP_SRC (appsrc)' failed`
     * and `video unit not queued` because the tap pushed into an appsrc that
     * stop_pipeline had already unref'd. Taking the lock and clearing both
     * sources here means the wrappers cannot enter a push once this point is
     * reached, so no push can touch an appsrc that is being destroyed. */
    g_mutex_lock(&media_lock);
    media.stopping = TRUE;
    media.video_source = NULL;
    media.audio_source = NULL;
    g_mutex_unlock(&media_lock);
    if (media.audio) {
        gst_element_set_state((GstElement *)media.audio, GST_STATE_NULL);
        if (media.audio_bus) {
            gst_object_unref(media.audio_bus);
            media.audio_bus = NULL;
        }
        gst_object_unref(media.audio);
        media.audio = NULL;
    }
    stop_pipeline(&app);
    wl_display_disconnect(app.display);
    engine_stop();
    core_shutdown(core_rpc);
    return 0;
}
