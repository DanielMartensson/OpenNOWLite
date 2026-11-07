/*
 * opennow-lite.c - OpenNOWLite stage 1: its own Wayland window, fed by
 * GStreamer, with its own keyboard and pointer pipeline.
 *
 * The window is an xdg-shell toplevel created here, not by the video sink and
 * not by opennow-qt. waylandsink draws into that surface: the wl_display goes
 * to the sink through GstWlDisplayHandleContextType and the surface through the
 * GstVideoOverlay window handle - the same hand-off this layer uses for YtGst -
 * so the picture fills this program's window while this program keeps the
 * seat's keyboard and pointer events on the same connection.
 *
 * Stage 1 plays a test pattern, so the window and the input path can be proven
 * with no session at all. Stage 2 replaces videotestsrc with the native
 * streamer's encoded frames and submits the input through
 * opennow_streamer_submit_key / opennow_streamer_submit_mouse_*.
 *
 * Markers (grep -a):
 *   opennowlite: window ready
 *   opennowlite: pipeline playing
 *   opennowlite-input: key ...
 *   opennowlite-input: pointer ...
 *   opennowlite: pipeline error
 *   opennowlite: session <ready|failed>            (stage 2)
 *   opennowlite: launch <method> <error>           (stage 2, RPC failures)
 *   opennowlite: engine <ok|status>                (stage 2)
 *   opennowlite: encoded video|audio <bytes>       (stage 2, first packet)
 *   opennowlite: input submitted <n>               (stage 2)
 *   opennowlite: media path failed                 (stage 2, media setup)
 */

#define OPENNOWLITE_FFI_LIBRARY "/usr/lib/opennowlite/libopennowlite_streamer_ffi.so"
#define OPENNOWLITE_CORE_PATH "/usr/bin/opennow-core"
#define OPENNOWLITE_SESSION_TIMEOUT 25
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
    uint32_t modifier_mask;
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

    if (a->fullscreen)
        xdg_toplevel_set_fullscreen(a->toplevel, NULL);

    fprintf(stderr, "opennowlite: window ready surface=%p app_id=opennowlite\n",
            (void *)a->surface);
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

static bool json_string(const char *doc, const char *key, char *out,
                       size_t capacity)
{
    const char *at = json_value_at(doc, key);
    if (!at || *at != '"')
        return false;
    size_t written = 0;
    for (const char *body = at + 1; *body; ++body) {
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

#define CORE_READ_MAX 32768
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
 * CoreClient drives it through QProcess. A socketpair gives the parent two
 * ends of one socket: the write end becomes the child's stdin, the read end
 * its stdout. Closing the write end when this program leaves is what hands
 * the core an end of input, so its Drop implementations stop and reap the
 * out-of-process native streamer and no orphan worker is left on the seat. */
static struct CoreRpc *core_start(const char *data_dir)
{
    int ends[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0)
        return NULL;

    pid_t child = fork();
    if (child < 0) {
        close(ends[0]);
        close(ends[1]);
        return NULL;
    }
    if (child == 0) {
        /* The child keeps this program's stderr, so the core's own
         * diagnostics land in the run log. execv replaces the image, so
         * nothing of the window, the pipeline or the callbacks survives. */
        if (dup2(ends[0], STDIN_FILENO) < 0 || dup2(ends[1], STDOUT_FILENO) < 0)
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
        close(ends[0]);
        close(ends[1]);
        return NULL;
    }
    rpc->write_fd = ends[0];
    rpc->read_fd = ends[1];
    rpc->child = child;
    rpc->buffer = malloc(CORE_READ_MAX);
    if (!rpc->buffer) {
        close(ends[0]);
        close(ends[1]);
        free(rpc);
        return NULL;
    }
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
 * object. There is no blank-line framing anywhere in that protocol. */
static bool core_request(struct CoreRpc *rpc, const char *id, const char *method,
                        const char *params)
{
    char line[CORE_LINE_MAX];
    int length = snprintf(line, sizeof(line),
        "{\"type\":\"request\",\"id\":\"%s\",\"method\":\"%s\","
        "\"params\":%s}\n", id, method,
        params && *params ? params : "{}");
    if (length < 0 || (size_t)length >= sizeof(line))
        return false;
    return core_write_line(rpc, line);
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

/* One reply = one JSON line. Responses carry the request's id, events
 * (session.changed, settings.changed, ...) do not, so they are read past
 * rather than dropped, and only a response for the id asked for is returned.
 * The copy is the caller's to free. */
static char *core_reply(struct CoreRpc *rpc, const char *want, int timeout_s)
{
    struct timespec then;
    clock_gettime(CLOCK_MONOTONIC, &then);
    for (;;) {
        char *end = memchr(rpc->buffer, '\n', rpc->used);
        if (!end) {
            if (rpc->used + 4096 >= CORE_READ_MAX)
                return NULL;
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
        bool response = !strcmp(type, "response");
        bool has_id = json_string(document, "\"id\"", got, sizeof(got));
        if (response && (!want[0] || (has_id && !strcmp(got, want))))
            return document;
        free(document);
        if (ms_since(&then) > (long)timeout_s * 1000L)
            return NULL;
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
    switch (evdev + 8) {
    case 10: return 0x31; case 11: return 0x32; case 12: return 0x33;
    case 13: return 0x34; case 14: return 0x35; case 15: return 0x36;
    case 16: return 0x37; case 17: return 0x38; case 18: return 0x39;
    case 19: return 0x30;
    case 20: return 0xbd; case 21: return 0xbb;
    case 24: return 0x51; case 25: return 0x57; case 26: return 0x45;
    case 27: return 0x52; case 28: return 0x54; case 29: return 0x59;
    case 30: return 0x55; case 31: return 0x49; case 32: return 0x4f;
    case 33: return 0x50; case 34: return 0xdb; case 35: return 0xdd;
    case 37: return 0xa2; case 38: return 0x41; case 39: return 0x53;
    case 40: return 0x44; case 41: return 0x46; case 42: return 0x47;
    case 43: return 0x48; case 44: return 0x4a; case 45: return 0x4b;
    case 46: return 0x4c; case 47: return 0xba; case 48: return 0xde;
    case 50: return 0xa0; case 51: return 0xdc; case 52: return 0x5a;
    case 53: return 0x58; case 54: return 0x43; case 55: return 0x56;
    case 56: return 0x42; case 57: return 0x4e; case 58: return 0x4d;
    case 59: return 0xbc; case 60: return 0xbe; case 61: return 0xbf;
    case 62: return 0xa1; case 64: return 0xa4; case 65: return 0x20;
    case 95: return 0xa5; case 99: return 0x5b; case 102: return 0x5c;
    case 103: return 0x26; case 105: return 0x27; case 106: return 0x25;
    case 108: return 0x28;
    case 110: return 0x22; case 111: return 0x23; case 112: return 0x24;
    case 113: return 0x2e; case 114: return 0x2d;
    case 1: return 0x1b; case 9: return 0x09;
    default: return 0;
    }
}

/* weston sends the modifier mask in its own message; GFN wants the modifier
 * bits that were down when the key moved. */
static uint16_t modifier_bits(uint32_t modifier_mask)
{
    uint16_t bits = 0;
    if (modifier_mask & (1u << 0))
        bits |= 0x01;
    if (modifier_mask & (1u << 1))
        bits |= 0x02;
    if (modifier_mask & (1u << 2))
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
    frame = core_reply(core_rpc, "lite-hello", 10);
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
    frame = core_reply(core_rpc, "lite-game", 25);
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
    frame = core_reply(core_rpc, "lite-launch", 25);
    if (core_refused(core_rpc, frame, "catalog.launch.inspect")) {
        free(frame);
        return false;
    }
    char decision[1024];
    decision[0] = '\0';
    char scope[4096] = "";
    char launch_status[64] = "";
    if (json_raw(frame, "\"decision\"", decision, sizeof(decision)))
        json_string(decision, "\"status\"", launch_status,
                    sizeof(launch_status));
    json_raw(frame, "\"scope\"", scope, sizeof(scope));
    free(frame);
    if (strcmp(launch_status, "ready") != 0) {
        fprintf(stderr, "opennowlite: launch is not ready: %s\n",
                launch_status[0] ? launch_status : "unknown");
        return false;
    }
    if (!scope[0]) {
        fprintf(stderr, "opennowlite: launch inspection carried no account scope\n");
        return false;
    }

    /* 4 · CloudMatch consent for this launch. The launch id is the variant,
     * the catalog id is the parent game, and the account scope is re-emitted
     * exactly as the core wrote it, since it is compared byte for byte. */
    snprintf(params, sizeof(params),
        "{\"appId\":\"%s\",\"catalogAppId\":\"%s\",\"variantId\":\"%s\","
        "\"scope\":%s}",
        variant_id, game_id, variant_id, scope);
    core_request(core_rpc, "lite-create", "session.create", params);
    frame = core_reply(core_rpc, "lite-create", OPENNOWLITE_SESSION_TIMEOUT);
    if (core_refused(core_rpc, frame, "session.create")) {
        free(frame);
        return false;
    }
    char session_id[256] = "";
    json_string(frame, "\"sessionId\"", session_id, sizeof(session_id));
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
     * the id is enough to address it and the context comes back verbatim. */
    bool prepared = false;
    for (int attempt = 0; attempt < 8 && !prepared; ++attempt) {
        snprintf(params, sizeof(params), "{\"sessionId\":\"%s\"}", session_id);
        core_request(core_rpc, "lite-poll", "session.poll", params);
        frame = core_reply(core_rpc, "lite-poll", OPENNOWLITE_SESSION_TIMEOUT);
        if (!frame) {
            fprintf(stderr, "opennowlite: launch session.poll no-reply\n");
            break;
        }
        bool ready = false;
        if (!core_refused(core_rpc, frame, "session.poll")) {
            char poll[CORE_LINE_MAX];
            poll[0] = '\0';
            (void)json_raw(frame, "\"session\"", poll, sizeof(poll));
            int status = json_int_at(poll, "\"status\"");
            ready = status == 2 || status == 3;
        }
        free(frame);
        frame = NULL;
        if (!ready)
            continue;

        snprintf(params, sizeof(params),
            "{\"session\":{\"sessionId\":\"%s\"}}", session_id);
        core_request(core_rpc, "lite-prepare", "streamer.prepare", params);
        frame = core_reply(core_rpc, "lite-prepare", 25);
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
        fprintf(stderr, "opennowlite: streamer context unavailable\n");
        return false;
    }
    fprintf(stderr, "opennowlite: session ready for %s\n", variant_id);
    return true;
}

/* ---- the encoded-media hand-off ---- */

/* The streamer sends one NAL unit per video packet, while h264parse is fed one
 * complete access unit at a time - the shape the proven yt-gst file hand-off
 * uses. So units are reassembled here: parameter sets stay in the buffer until
 * the picture's last slice closes it. */
typedef struct Media {
    GstElement *video;
    GstElement *audio;
    gboolean audio_ready;
    GstAppSrc *video_source;
    GstAppSrc *audio_source;
    guint8 *video_bytes;
    gsize video_used;
    gsize video_cap;
    guint64 video_base;
    guint64 audio_base;
    guint64 video_units;
    guint64 audio_units;
} Media;

static Media media;

static void media_push_video(Media *owner, const OpenNowStreamerEncodedMedia
                            *packet)
{
    const guint8 *data = packet->data;
    const size_t length = packet->data_len;
    if (owner->video_used + length > owner->video_cap) {
        fprintf(stderr, "opennowlite: video unit overflow: %zu\n", length);
        owner->video_used = 0;
        return;
    }
    memcpy(owner->video_bytes + owner->video_used, data, length);
    owner->video_used += length;
    if (!packet->keyframe)
        return;

    GstBuffer *buffer = gst_buffer_new_wrapped(owner->video_bytes,
                                              owner->video_used);
    owner->video_used = 0;

    guint32 rate = packet->clock_rate_hz ? packet->clock_rate_hz : 90000u;
    guint64 pts = (guint64)packet->timestamp * GST_SECOND / rate;
    if (!owner->video_base)
        owner->video_base = pts + GST_SECOND;
    GST_BUFFER_PTS(buffer) = pts + GST_SECOND - owner->video_base;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 60u;
    if (gst_app_src_push_buffer(owner->video_source, buffer)
        != GST_FLOW_OK) {
        /* appsrc owns the buffer it refused; it frees it when the element is
         * torn down, so nothing here has to be released. */
        fprintf(stderr, "opennowlite: video unit not queued\n");
        return;
    }
    ++owner->video_units;
    if (owner->video_units == 1)
        fprintf(stderr, "opennowlite: encoded video unit 1\n");
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
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 50u;   /* 20 ms per Opus frame */
    if (gst_app_src_push_buffer(owner->audio_source, buffer)
        != GST_FLOW_OK) {
        fprintf(stderr, "opennowlite: audio packet not queued\n");
        return;
    }
    ++owner->audio_units;
    if (owner->audio_units == 1)
        fprintf(stderr, "opennowlite: encoded audio packet 1\n");
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
static const char *audio_caps_string =
    "audio/x-opus,rate=48000,channels=2,channel-mapping-family=0";

/* appsrc with a live, non-seeking feed: `is-live` keeps the feed from ever
 * claiming end-of-stream, `do-timestamp=false` leaves the presentation
 * timestamps put on each pushed buffer intact (1.22's `format` property is
 * about segment events, not about this), and `block=true` with `max-bytes`
 * bounds the internal queue so a stalled feed cannot grow it. */
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
    g_object_set(element, "is-live", TRUE, "do-timestamp", FALSE,
                 "block", TRUE, "max-bytes", (guint64)max_bytes, NULL);
    if (is_audio) {
        owner->audio_source = source;
        return true;
    }
    owner->video_source = source;
    owner->video_cap = max_bytes;
    owner->video_bytes = malloc(max_bytes);
    if (!owner->video_bytes) {
        gst_object_unref(element);
        owner->video_source = NULL;
        return false;
    }
    return true;
}

/* Elements are probed rather than assumed: an absent Opus element means this
 * window simply has no audio, and saying so in the log is better than
 * refusing to show the game. */
static bool stream_new(struct lite *a)
{
    GstElementFactory *opusparse_factory = gst_element_factory_find("opusparse");
    GstElementFactory *opusdec_factory = gst_element_factory_find("opusdec");
    media.audio_ready = opusparse_factory && opusdec_factory;
    if (opusparse_factory)
        gst_object_unref(opusparse_factory);
    if (opusdec_factory)
        gst_object_unref(opusdec_factory);
    if (!media.audio_ready)
        fprintf(stderr, "opennowlite: audio pipeline unavailable\n");

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
    GstElement *sparse = gst_element_factory_make("opusparse", NULL);
    GstElement *decode = gst_element_factory_make("opusdec", NULL);
    GstElement *audiosink = gst_element_factory_make("autoaudiosink", NULL);
    if (!sparse || !decode || !audiosink) {
        if (sparse)
            gst_object_unref(sparse);
        if (decode)
            gst_object_unref(decode);
        if (audiosink)
            gst_object_unref(audiosink);
        gst_object_unref(media.audio_source);
        media.audio_source = NULL;
        media.audio_ready = false;
        return true;
    }
    media.audio = gst_pipeline_new("opennowlite-audio");
    if (!media.audio) {
        gst_object_unref(sparse);
        gst_object_unref(decode);
        gst_object_unref(audiosink);
        gst_object_unref(media.audio_source);
        media.audio_source = NULL;
        media.audio_ready = false;
        return true;
    }
    gst_bin_add_many(GST_BIN(media.audio), (GstElement *)media.audio_source,
                     sparse, decode, audiosink, NULL);
    if (!gst_element_link_many((GstElement *)media.audio_source, sparse, decode,
                               audiosink, NULL)) {
        fprintf(stderr, "opennowlite: audio chain could not be linked\n");
        gst_element_set_state(media.audio, GST_STATE_NULL);
        gst_object_unref(media.audio);
        media.audio = NULL;
        media.audio_source = NULL;
        media.audio_ready = false;
        return true;
    }
    gst_element_set_state(media.audio, GST_STATE_PLAYING);
    fprintf(stderr, "opennowlite: audio pipeline ready\n");
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
        if (streamer.relative(streamer.engine, dx, dy) == OPENNOW_STREAMER_OK)
            ++app.submits;
    }
    if (pointer_state.wheel) {
        /* weston reports a wheel step as 120 units of a turn; GFN steps by one,
         * and scrolling away from the viewer is a positive step. */
        int16_t steps = (int16_t)(pointer_state.wheel / 120);
        pointer_state.wheel = 0;
        if (steps && streamer.wheel(streamer.engine, 0, -steps)
                        == OPENNOW_STREAMER_OK)
            ++app.submits;
    }
}

static bool quit_key(uint32_t evdev, uint32_t modifier_mask)
{
    return evdev == 16 && (modifier_mask & 0x7u) == 0x7u;
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
    if (!virtual_key)
        return;
    if (streamer.key(streamer.engine, virtual_key,
                    modifier_bits(app.modifier_mask), pressed)
        == OPENNOW_STREAMER_OK)
        ++app.submits;
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

/* weston numbers buttons 1 left, 2 right, 3 middle; GFN's button field is
 * 0 left, 1 right, 2 middle. */
static void seat_button(uint32_t button, uint32_t state)
{
    bool pressed = state == 1;
    if (button == 1 && pressed && !pointer_state.locked) {
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
    uint8_t mapped = button == 1 ? 0u : button == 2 ? 1u : 2u;
    if (streamer.button(streamer.engine, mapped, pressed)
        == OPENNOW_STREAMER_OK)
        ++app.submits;
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

    fprintf(stderr, "opennowlite: stopping keys=%lu motions=%lu submits=%lu\n",
            app.keys, app.motions, app.submits);
    if (pointer_state.locked)
        pointer_cursor_show();
    engine_stop();
    core_shutdown(core_rpc);
    if (media.audio) {
        gst_element_set_state((GstElement *)media.audio, GST_STATE_NULL);
        gst_object_unref(media.audio);
        media.audio = NULL;
    }
    stop_pipeline(&app);
    wl_display_disconnect(app.display);
    return 0;
}
