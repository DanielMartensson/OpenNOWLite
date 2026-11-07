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
 *   opennowlite: session ready for <id>                    (launch)
 *   opennowlite: engine started for <id>                   (streamer up)
 *   opennowlite: encoded video unit 1 / encoded audio packet 1
 *   opennowlite: video units=N queued=N / audio packets=N queued=N (media tally
 *                                                  with the appsrc's own queued
 *                                                  bytes, every 600 units /
 *                                                  1500 packets)
 *   opennowlite: video refused=N / audio refused=N                 (units the
 *                                                  appsrc would not take)
 *   opennowlite: pointer locked                            (mouse capture on)
 *   opennowlite-input: key code=<n> evdev=<n> state=<0/1>   (each key the seat sends)
 *   opennowlite-input: <kind> submit refused (<n>)          (first refused input call)
 *   opennowlite: stopping keys=N motions=N submits=N refused=N     (exit tally)
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
};

static struct lite app;

static void seat_key(uint32_t key_code, bool pressed);
static void seat_motion(double x, double y);
static void seat_button(uint32_t button, uint32_t state);
static void seat_wheel(int32_t value120);
static void pointer_cursor_hide(void);

static long ms_since(const struct timespec *then)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - then->tv_sec) * 1000L +
           (now.tv_nsec - then->tv_nsec) / 1000000L;
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
    (void)ptr;
    struct lite *a = data;
    a->enter_serial = serial;
    a->pointer_surface = surface;
    a->pointer_x = wl_fixed_to_double(sx);
    a->pointer_y = wl_fixed_to_double(sy);
    fprintf(stderr, "opennowlite-input: pointer enter x=%.1f y=%.1f\n",
            a->pointer_x, a->pointer_y);
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
    seat_motion(a->pointer_x, a->pointer_y);
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
    fprintf(stderr, "opennowlite-input: pointer button=%u state=%u\n",
            button, state);
    seat_button(button, state);
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
        seat_wheel(value120);
    (void)a;
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
    streamer.button = (button_binding)dlsym(
        library, "opennow_streamer_submit_mouse_button");
    streamer.wheel = (wheel_binding)dlsym(
        library, "opennow_streamer_submit_mouse_wheel");
    streamer.set_log = (log_binding)dlsym(
        library, "opennow_streamer_set_log_file");
    if (!streamer.create || !streamer.destroy || !streamer.send ||
        !streamer.capture || !streamer.key || !streamer.relative ||
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
    for (int attempt = 0; attempt < 60 && !prepared; ++attempt) {
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
    guint64 video_base;
    guint64 audio_base;
    guint64 video_units;
    guint64 audio_units;
    guint64 video_refused;
    guint64 audio_refused;
} Media;

static Media media;

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

static void media_push_video(Media *owner, const OpenNowStreamerEncodedMedia
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

static void media_push_audio(Media *owner,
                            const OpenNowStreamerEncodedMedia *packet)
{
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, packet->data_len, NULL);
    if (!buffer)
        return;
    gst_buffer_fill(buffer, 0, packet->data, packet->data_len);

    guint32 rate = packet->clock_rate_hz ? packet->clock_rate_hz : 48000u;
    guint64 pts = (guint64)packet->timestamp * GST_SECOND / rate;
    if (!owner->audio_base)
        owner->audio_base = pts + GST_SECOND;
    GST_BUFFER_PTS(buffer) = pts + GST_SECOND - owner->audio_base;
    /* 5 ms, not 20: the tap delivers ~200 packets a second against 60 video
     * units a second, and the opennow layer's own audio queue says why
     * ("the play queue holds 12 packets of 5 ms"), so an Opus frame off this
     * wire is 5 ms of audio. Naming 20 ms made the audio chain run 4x faster
     * than the clock it was handed. */
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 200u;
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
        fprintf(stderr, "opennowlite: audio packets=%llu queued=%llu\n",
                (unsigned long long)owner->audio_units,
                (unsigned long long)gst_app_src_get_current_level_bytes(owner->audio_source));
    }
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

    if (!media_source_new(&media, audio_caps_string, 4u << 20, TRUE)) {
        media.audio_ready = false;
        return true;
    }
    GstElement *decode = gst_element_factory_make("opusdec", NULL);
    GstElement *audiosink = gst_element_factory_make("autoaudiosink", NULL);
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
    if (!decode || !audiosink || !queue) {
        if (decode)
            gst_object_unref(decode);
        if (audiosink)
            gst_object_unref(audiosink);
        if (queue)
            gst_object_unref(queue);
        gst_object_unref(media.audio_source);
        media.audio_source = NULL;
        media.audio_ready = false;
        return true;
    }
    media.audio = gst_pipeline_new("opennowlite-audio");
    if (!media.audio) {
        gst_object_unref(decode);
        gst_object_unref(audiosink);
        gst_object_unref(queue);
        gst_object_unref(media.audio_source);
        media.audio_source = NULL;
        media.audio_ready = false;
        return true;
    }
    gst_bin_add_many(GST_BIN(media.audio), (GstElement *)media.audio_source,
                     queue, decode, audiosink, NULL);
    if (!gst_element_link_many((GstElement *)media.audio_source, queue, decode,
                               audiosink, NULL)) {
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
    fprintf(stderr, "opennowlite: audio pipeline ready set=%d reached=%d state=%d\n",
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

/* The seat's mouse is a physical pointer: weston moves an absolute cursor. For
 * a game GFN wants relative motion, so this client locks the pointer the way
 * OpenNOW does - the first left click locks and is not forwarded, so it cannot
 * also act inside the game - and afterwards turns weston's own motions into
 * relative deltas. Ctrl+Alt+Shift+Q releases the lock and closes the window. */
static struct Pointer {
    bool locked;
    double last_x;
    double last_y;
    int32_t dx;
    int32_t dy;
    int32_t wheel;
} pointer_state;

static void pointer_submit(void)
{
    if (!streamer.engine || !streamer.capture_active)
        return;
    if (pointer_state.dx || pointer_state.dy) {
        int16_t dx = (int16_t)pointer_state.dx;
        int16_t dy = (int16_t)pointer_state.dy;
        pointer_state.dx = 0;
        pointer_state.dy = 0;
        OpenNowStreamerStatus status =
            streamer.relative(streamer.engine, dx, dy);
        if (status == OPENNOW_STREAMER_OK) {
            ++app.submits;
        } else if (app.input_refused++ == 0u) {
            fprintf(stderr, "opennowlite-input: motion submit refused (%d)\n",
                    (int)status);
        }
    }
    if (pointer_state.wheel) {
        /* weston reports a wheel step as 120 units of a turn; GFN steps by one,
         * and scrolling away from the viewer is a positive step. */
        int16_t steps = (int16_t)(pointer_state.wheel / 120);
        pointer_state.wheel = 0;
        if (steps) {
            OpenNowStreamerStatus status =
                streamer.wheel(streamer.engine, 0, -steps);
            if (status == OPENNOW_STREAMER_OK) {
                ++app.submits;
            } else if (app.input_refused++ == 0u) {
                fprintf(stderr, "opennowlite-input: wheel submit refused (%d)\n",
                        (int)status);
            }
        }
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
    OpenNowStreamerStatus status = streamer.key(streamer.engine, virtual_key,
                                                modifier_bits(app.modifier_mask),
                                                pressed);
    if (status == OPENNOW_STREAMER_OK) {
        ++app.submits;
    } else if (app.input_refused++ == 0u) {
        fprintf(stderr, "opennowlite-input: key submit refused (%d)\n",
                (int)status);
    }
}

/* The absolute cursor is tracked either way so the first locked motion cannot
 * jump the view by the whole distance the cursor had travelled unlocked. */
static void seat_motion(double x, double y)
{
    if (pointer_state.locked) {
        pointer_state.dx += (int32_t)(x - pointer_state.last_x);
        pointer_state.dy += (int32_t)(y - pointer_state.last_y);
        pointer_submit();
    }
    pointer_state.last_x = x;
    pointer_state.last_y = y;
}

/* weston numbers buttons by their evdev code, not 1/2/3: a wl_pointer.button
 * event carries BTN_LEFT 0x110 (272), BTN_RIGHT 0x111 (273) and BTN_MIDDLE
 * 0x112 (274). Comparing the event against 1 meant the pointer never locked
 * and every press was dropped by the `!locked` guard - measured on the seat:
 * the log showed `pointer button=272 state=1` and never `pointer locked`, so
 * the game received no click at all. The streamer then wants X11 numbers, not
 * GFN's zero-based field: its own Windows raw-input path maps left to 1,
 * middle to 2 and right to 3, and its FFI rejects anything outside 1..=5 with
 * INVALID_CONFIG - measured: sending 0 for the left button produced
 * `button submit refused (2)` on the first click after the lock. */
static void seat_button(uint32_t button, uint32_t state)
{
    bool pressed = state == 1;
    if (button == 272 && pressed && !pointer_state.locked) {
        pointer_state.locked = true;
        pointer_cursor_hide();
        /* The click that locks the pointer is not forwarded, so locking cannot
         * also fire in the game. */
        fprintf(stderr, "opennowlite-input: pointer locked\n");
        return;
    }
    if (!pointer_state.locked && pressed)
        return;
    if (!streamer.engine || !streamer.capture_active)
        return;
    uint8_t mapped;
    switch (button) {
    case 272: mapped = 1u; break;   /* BTN_LEFT   -> X11/OpenNOW 1 */
    case 274: mapped = 2u; break;   /* BTN_MIDDLE -> 2 */
    case 273: mapped = 3u; break;   /* BTN_RIGHT  -> 3 */
    default:
        return;
    }
    OpenNowStreamerStatus status =
        streamer.button(streamer.engine, mapped, pressed);
    if (status == OPENNOW_STREAMER_OK) {
        ++app.submits;
    } else if (app.input_refused++ == 0u) {
        fprintf(stderr, "opennowlite-input: button submit refused (%d)\n",
                (int)status);
    }
}

static void seat_wheel(int32_t value120)
{
    pointer_state.wheel += value120;
    pointer_submit();
}

/* ---- the seat's cursor while the pointer is captured ---- */

/* While the pointer is captured for the game, weston's own arrow is a second
 * pointer into the same scene, so it is hidden. The serial has to be the one
 * the seat last sent with wl_pointer.enter or weston ignores the request, and
 * a NULL surface is what hides the cursor. */
static void pointer_cursor_hide(void)
{
    if (!app.pointer)
        return;
    wl_pointer_set_cursor(app.pointer, app.enter_serial, NULL, 0, 0);
    wl_display_flush(app.display);
}

/* No set_cursor of this program can put weston's own arrow back, so the way
 * out is the seat's: releasing tells the compositor this client is done with
 * the pointer interface, which is what makes it paint its default cursor
 * again, and the seat hands back a fresh proxy so the listeners below keep
 * working. Releasing already destroys the proxy, so nothing here destroys it
 * a second time. */
static void pointer_cursor_show(void)
{
    if (!app.pointer || !app.seat)
        return;
    struct wl_pointer *pointer = app.pointer;
    app.pointer = NULL;
    wl_pointer_release(pointer);
    app.pointer = wl_seat_get_pointer(app.seat);
    if (!app.pointer)
        return;
    /* The seat drives the interface version itself, which is what the
     * listener below expects, so the version is never claimed here. */
    wl_pointer_add_listener(app.pointer, &pointer_listener, &app);
    wl_display_flush(app.display);
}

/* ---- main ---- */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--launch-app-id=ID] [--data-dir DIR] [--fullscreen]\n"
            "       [--width N] [--height N] [--pipeline DESC] [--test-pattern]\n"
            "  the default launch opens the seat's signed-in session and streams\n"
            "  the game into the app's own Wayland window; --test-pattern, or\n"
            "  --pipeline, plays a test pattern in that same window with no\n"
            "  session at all, which is how the window and the input path are\n"
            "  proven. --data-dir only points the shipped core at another copy\n"
            "  of its data directory; by default it uses this seat's.\n",
            argv0);
}

int main(int argc, char **argv)
{
    bool test_pattern = false;
    bool fullscreen = false;
    int width = 1920;
    int height = 1080;
    const char *variant_id = NULL;
    const char *data_dir = NULL;
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
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            fullscreen = true;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pipeline") == 0 && i + 1 < argc) {
            /* only the test pattern has no session to feed the window */
            pipeline = argv[++i];
            test_pattern = true;
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

    signal(SIGINT, on_signal);
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

    fprintf(stderr, "opennowlite: stopping keys=%lu motions=%lu submits=%lu refused=%lu\n",
            app.keys, app.motions, app.submits, app.input_refused);
    if (pointer_state.locked)
        pointer_cursor_show();
    engine_stop();
    core_shutdown(core_rpc);
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
    return 0;
}
