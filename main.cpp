#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <wayland-client.h>
#include <cstring>
#include <xkbcommon/xkbcommon.h>
#include <iostream>
#include "shm_tools.h"
#include "xdg-shell-client-protocol.h"

/// --------- touch events

enum touch_event_mask {
    TOUCH_EVENT_DOWN = 1 << 0,
    TOUCH_EVENT_UP = 1 << 1,
    TOUCH_EVENT_MOTION = 1 << 2,
    TOUCH_EVENT_CANCEL = 1 << 3,
    TOUCH_EVENT_SHAPE = 1 << 4,
    TOUCH_EVENT_ORIENTATION = 1 << 5,
};

struct touch_point {
    bool valid;
    int32_t id;
    uint32_t event_mask;
    wl_fixed_t surface_x, surface_y;
    wl_fixed_t major, minor;
    wl_fixed_t orientation;
};

struct touch_event {
    uint32_t event_mask;
    uint32_t time;
    uint32_t serial;
    struct touch_point points[10];
};

/// --------- pointer events

enum pointer_event_mask {
    POINTER_EVENT_ENTER = 1 << 0,
    POINTER_EVENT_LEAVE = 1 << 1,
    POINTER_EVENT_MOTION = 1 << 2,
    POINTER_EVENT_BUTTON = 1 << 3,
    POINTER_EVENT_AXIS = 1 << 4,
    POINTER_EVENT_AXIS_SOURCE = 1 << 5,
    POINTER_EVENT_AXIS_STOP = 1 << 6,
    POINTER_EVENT_AXIS_DISCRETE = 1 << 7,
};

struct pointer_event {
    uint32_t event_mask;
    wl_fixed_t surface_x, surface_y;
    uint32_t button, state;
    uint32_t time;
    uint32_t serial;
    struct {
        bool valid;
        wl_fixed_t value;
        int32_t discrete;
    } axes[2];
    uint32_t axis_source;
};

/// --------- client state

struct my_buffer {
    int width, height;
    uint32_t *data;
    struct wl_buffer *wl_buffer;
};

struct client_state {
    /* Globals */
    struct wl_display *wl_display;
    struct wl_registry *wl_registry;
    struct wl_shm *wl_shm;
    struct wl_compositor *wl_compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_seat *wl_seat;
    /* Objects */
    struct wl_surface *wl_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_keyboard *wl_keyboard;
    struct wl_pointer *wl_pointer;
    struct wl_touch *wl_touch;
    /* State */
    float offset;
    uint32_t last_frame;
    int width, height;
    bool closed;
    struct my_buffer my_buffer;
    int x, y; // pinter last x and last y
    bool button_pressed;
    struct pointer_event pointer_event;
    struct xkb_state *xkb_state;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct touch_event touch_event;
};


/// --------- other shm tool

static int
set_cloexec_or_close(int fd) {
    long flags;

    if (fd == -1)
        return -1;

    flags = fcntl(fd, F_GETFD);
    if (flags == -1)
        goto err;

    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
        goto err;

    return fd;

    err:
    close(fd);
    return -1;
}

static int
create_tmpfile_cloexec(char *tmpname) {
    int fd;

#ifdef HAVE_MKOSTEMP
    fd = mkostemp(tmpname, O_CLOEXEC);
        if (fd >= 0)
                unlink(tmpname);
#else
    fd = mkstemp(tmpname);
    if (fd >= 0) {
        fd = set_cloexec_or_close(fd);
        unlink(tmpname);
    }
#endif

    return fd;
}

/*
 * Create a new, unique, anonymous file of the given size, and
 * return the file descriptor for it. The file descriptor is set
 * CLOEXEC. The file is immediately suitable for mmap()'ing
 * the given size at offset zero.
 *
 * The file should not have a permanent backing store like a disk,
 * but may have if XDG_RUNTIME_DIR is not properly implemented in OS.
 *
 * The file name is deleted from the file system.
 *
 * The file is suitable for buffer sharing between processes by
 * transmitting the file descriptor over Unix sockets using the
 * SCM_RIGHTS methods.
 */
int
os_create_anonymous_file(off_t size) {
    static const char template1[] = "/weston-shared-XXXXXX";
    const char *path;
    char *name;
    int fd;

    path = getenv("XDG_RUNTIME_DIR");
    if (!path) {
        errno = ENOENT;
        return -1;
    }

    name = static_cast<char *>(malloc(strlen(path) + sizeof(template1)));
    if (!name)
        return -1;
    strcpy(name, path);
    strcat(name,
           template1);

    fd = create_tmpfile_cloexec(name);

    free(name);

    if (fd < 0)
        return -1;

    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/// --------- wl registry listener

static void
registry_handle_global(void *data, struct wl_registry *wl_registry,
                       uint32_t name, const char *interface, uint32_t version) {
    printf("interface: '%s', version: %d, name: %d\n",
           interface, version, name);

    struct client_state *state = static_cast<client_state *>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->wl_compositor = static_cast<wl_compositor *>(wl_registry_bind(
                wl_registry, name, &wl_compositor_interface, 5));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->wl_shm = static_cast<wl_shm *>(wl_registry_bind(
                wl_registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->xdg_wm_base = static_cast<xdg_wm_base *>(wl_registry_bind(
                wl_registry, name, &xdg_wm_base_interface, 1));
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->wl_seat = static_cast<wl_seat *>(wl_registry_bind(
                wl_registry, name, &wl_seat_interface, 5));
    }
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
                              uint32_t name) {
    // This space deliberately left blank
}

static const struct wl_registry_listener
        registry_listener = {
        .global = registry_handle_global,
        .global_remove = registry_handle_global_remove,
};

/// --------- wl buffer listener

static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
    /* Sent by the compositor when it's no longer using this buffer */
    wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
        .release = wl_buffer_release,
};

/// --------- xdg surface listener

static struct wl_buffer *
        create_buffer(struct client_state *state) {
    int width = state->width, height = state->height;
    int stride = width * 4;
    int size = stride * height;

    int fd = allocate_shm_file(size);
    if (fd == -1) {
        return NULL;
    }

    state->my_buffer.data = static_cast<uint32_t *>(mmap(NULL, size,
                                                  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (state->my_buffer.data == MAP_FAILED) {
        close(fd);
        state->my_buffer.wl_buffer = nullptr;
        return NULL;
    }
    state->my_buffer.width = width;
    state->my_buffer.height = height;

    struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, fd, size);
    state->my_buffer.wl_buffer = wl_shm_pool_create_buffer(pool, 0,
                                                         width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    return state->my_buffer.wl_buffer;
}
static void
unmap_buffer(struct client_state *state) {
    my_buffer& my_buffer = state->my_buffer;
    int width = my_buffer.width, height = my_buffer.height;
    int stride = width * 4;
    int size = stride * height;

    munmap(my_buffer.data, size);
    wl_buffer_add_listener(my_buffer.wl_buffer, &wl_buffer_listener, NULL);
}

//static struct wl_buffer *
static void
draw_frame(struct client_state *state) {

    my_buffer& my_buffer = state->my_buffer;
    int width = my_buffer.width, height = my_buffer.height;

    /* Draw checkerboxed background */
    int offset = (int) state->offset % 8;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
//            if ((x + y / 8 * 8) % 16 < 8)
            if (((x + offset) + (y + offset) / 8 * 8) % 16 < 8)
//                my_buffer.data[y * width + x] = 0xFF666666;
                my_buffer.data[y * width + x] = 0xFFffffff;
            else
//                my_buffer.data[y * width + x] = 0xFFaaEEaa;
                my_buffer.data[y * width + x] = 0xFFffffff;
        }
    }

}

static void
draw_point(struct client_state *state, int x, int y, uint32_t color) {
    my_buffer& my_buffer = state->my_buffer;
    int width = my_buffer.width, height = my_buffer.height;
//    std::cout<<"point "<<x<<", "<<y<<std::endl;
    for (int dy = 0; dy < 4; ++dy) {
        for (int dx = 0; dx < 4; ++dx) {
            my_buffer.data[(y+dy) * width + (x+dx)] = color;
        }
    }
}


static void
xdg_toplevel_configure(void *data,
                       struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height,
                       struct wl_array *states) {
    struct client_state *state = static_cast<client_state *>(data);
    if (width == 0 || height == 0) {
        /* Compositor is deferring to us */
        return;
    }
    state->width = width;
    state->height = height;
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    struct client_state *state = static_cast<client_state *>(data);
    unmap_buffer(state);
    state->closed = true;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
        .configure = xdg_toplevel_configure,
        .close = xdg_toplevel_close,
};

static void
xdg_surface_configure(void *data,
                      struct xdg_surface *xdg_surface, uint32_t serial) {
    struct client_state *state = static_cast<client_state *>(data);
    xdg_surface_ack_configure(xdg_surface, serial);

    struct wl_buffer *buffer = create_buffer(state);
    if (! buffer) {
        return;
    }
//    struct wl_buffer *buffer =
    draw_frame(state);

    wl_surface_attach(state->wl_surface, buffer, 0, 0);
    wl_surface_commit(state->wl_surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
        .configure = xdg_surface_configure,
};

/// --------- xdg surface frame listener

extern /*static*/ const struct wl_callback_listener wl_surface_frame_listener;

static void
wl_surface_frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    /* Destroy this callback */
    wl_callback_destroy(cb);

    /* Request another frame */
    struct client_state *state = static_cast<client_state *>(data);
    cb = wl_surface_frame(state->wl_surface);
    wl_callback_add_listener(cb, &wl_surface_frame_listener, state);

    /* Update scroll amount at 24 pixels per second */
    if (state->last_frame != 0) {
        int elapsed = time - state->last_frame;
        state->offset += elapsed / 1000.0 * 24;
    }

    /* Submit a frame for this event */
//    struct wl_buffer *buffer =
    draw_frame(state);
    wl_surface_attach(state->wl_surface, state->my_buffer.wl_buffer, 0, 0);
//	wl_surface_damage_buffer(state->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_damage_buffer(state->wl_surface, 0, 0, 20, INT32_MAX);
    wl_surface_commit(state->wl_surface);

    state->last_frame = time;
}

/*static*/ const struct wl_callback_listener wl_surface_frame_listener = {
        .done = wl_surface_frame_done,
};


/// --------- wl pointer listener

static void
wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface,
                 wl_fixed_t surface_x, wl_fixed_t surface_y) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    client_state->pointer_event.event_mask |= POINTER_EVENT_ENTER;
    client_state->pointer_event.serial = serial;
    client_state->pointer_event.surface_x = surface_x,
            client_state->pointer_event.surface_y = surface_y;
    client_state->x = wl_fixed_to_int(surface_x);
    client_state->y = wl_fixed_to_int(surface_y);
}

static void
wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    client_state->pointer_event.serial = serial;
    client_state->pointer_event.event_mask |= POINTER_EVENT_LEAVE;
}

static void
wl_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
                  wl_fixed_t surface_x, wl_fixed_t surface_y) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    client_state->pointer_event.event_mask |= POINTER_EVENT_MOTION;
    client_state->pointer_event.time = time;
    client_state->pointer_event.surface_x = surface_x,
            client_state->pointer_event.surface_y = surface_y;
    client_state->x = wl_fixed_to_int(surface_x);
    client_state->y = wl_fixed_to_int(surface_y);

    if(client_state->button_pressed) {
        draw_point(client_state,
                   client_state->x,
                   client_state->y,
                   0xFF00EE33
        );
        wl_surface_attach(client_state->wl_surface, client_state->my_buffer.wl_buffer, 0, 0);
//	wl_surface_damage_buffer(state->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_damage_buffer(client_state->wl_surface, client_state->x, client_state->y,
                                 INT32_MAX, INT32_MAX);
        wl_surface_commit(client_state->wl_surface);
    }
}

static void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
                  uint32_t time, uint32_t button, uint32_t state) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    client_state->pointer_event.event_mask |= POINTER_EVENT_BUTTON;
    client_state->pointer_event.time = time;
    client_state->pointer_event.serial = serial;
    client_state->pointer_event.button = button,
            client_state->pointer_event.state = state;

    client_state->button_pressed = state == WL_POINTER_BUTTON_STATE_RELEASED ? false : true;

    draw_point(client_state,
               client_state->x,
               client_state->y,
               0xFF00EE33
               );
    wl_surface_attach(client_state->wl_surface, client_state->my_buffer.wl_buffer, 0, 0);
//	wl_surface_damage_buffer(state->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_damage_buffer(client_state->wl_surface, client_state->x, client_state->y,
                             INT32_MAX, INT32_MAX);
    wl_surface_commit(client_state->wl_surface);


}

static void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
                uint32_t axis, wl_fixed_t value) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    client_state->pointer_event.event_mask |= POINTER_EVENT_AXIS;
    client_state->pointer_event.time = time;
    client_state->pointer_event.axes[axis].valid = true;
    client_state->pointer_event.axes[axis].value = value;
}

static void
wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
                       uint32_t axis_source) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    client_state->pointer_event.event_mask |= POINTER_EVENT_AXIS_SOURCE;
    client_state->pointer_event.axis_source = axis_source;
}

static void
wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
                     uint32_t time, uint32_t axis) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    client_state->pointer_event.time = time;
    client_state->pointer_event.event_mask |= POINTER_EVENT_AXIS_STOP;
    client_state->pointer_event.axes[axis].valid = true;
}

static void
wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                         uint32_t axis, int32_t discrete) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    client_state->pointer_event.event_mask |= POINTER_EVENT_AXIS_DISCRETE;
    client_state->pointer_event.axes[axis].valid = true;
    client_state->pointer_event.axes[axis].discrete = discrete;
}

static void
wl_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    struct pointer_event *event = &client_state->pointer_event;
    fprintf(stdout, "pointer frame @ %d: ", event->time);

    if (event->event_mask & POINTER_EVENT_ENTER) {
        fprintf(stdout, "entered %f, %f ",
                wl_fixed_to_double(event->surface_x),
                wl_fixed_to_double(event->surface_y));
    }

    if (event->event_mask & POINTER_EVENT_LEAVE) {
        fprintf(stdout, "leave");
    }

    if (event->event_mask & POINTER_EVENT_MOTION) {
        fprintf(stdout, "motion %f, %f ",
                wl_fixed_to_double(event->surface_x),
                wl_fixed_to_double(event->surface_y));
    }

    if (event->event_mask & POINTER_EVENT_BUTTON) {
        char *state = const_cast<char *>(event->state == WL_POINTER_BUTTON_STATE_RELEASED ?
                                         "released" : "pressed");
        fprintf(stdout, "button %d %s ", event->button, state);
    }

    uint32_t axis_events = POINTER_EVENT_AXIS
                           | POINTER_EVENT_AXIS_SOURCE
                           | POINTER_EVENT_AXIS_STOP
                           | POINTER_EVENT_AXIS_DISCRETE;
    char *axis_name[2] = {
            [WL_POINTER_AXIS_VERTICAL_SCROLL] = "vertical",
            [WL_POINTER_AXIS_HORIZONTAL_SCROLL] = "horizontal",
    };
    char *axis_source[4] = {
            [WL_POINTER_AXIS_SOURCE_WHEEL] = "wheel",
            [WL_POINTER_AXIS_SOURCE_FINGER] = "finger",
            [WL_POINTER_AXIS_SOURCE_CONTINUOUS] = "continuous",
            [WL_POINTER_AXIS_SOURCE_WHEEL_TILT] = "wheel tilt",
    };
    if (event->event_mask & axis_events) {
        for (size_t i = 0; i < 2; ++i) {
            if (!event->axes[i].valid) {
                continue;
            }
            fprintf(stdout, "%s axis ", axis_name[i]);
            if (event->event_mask & POINTER_EVENT_AXIS) {
                fprintf(stdout, "value %f ", wl_fixed_to_double(
                        event->axes[i].value));
            }
            if (event->event_mask & POINTER_EVENT_AXIS_DISCRETE) {
                fprintf(stdout, "discrete %d ",
                        event->axes[i].discrete);
            }
            if (event->event_mask & POINTER_EVENT_AXIS_SOURCE) {
                fprintf(stdout, "via %s ",
                        axis_source[event->axis_source]);
            }
            if (event->event_mask & POINTER_EVENT_AXIS_STOP) {
                fprintf(stdout, "(stopped) ");
            }
        }
    }

    fprintf(stdout, "\n");
    memset(event, 0, sizeof(*event));
}

static const struct wl_pointer_listener wl_pointer_listener = {
        .enter = wl_pointer_enter,
        .leave = wl_pointer_leave,
        .motion = wl_pointer_motion,
        .button = wl_pointer_button,
        .axis = wl_pointer_axis,
        .frame = wl_pointer_frame,
        .axis_source = wl_pointer_axis_source,
        .axis_stop = wl_pointer_axis_stop,
        .axis_discrete = wl_pointer_axis_discrete,
};

/// --------- wl keyboard listener

static void
wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                   uint32_t format, int32_t fd, uint32_t size) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);

    char *map_shm = static_cast<char *>(mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0));
    assert(map_shm != MAP_FAILED);

    struct xkb_keymap *xkb_keymap = xkb_keymap_new_from_string(
            client_state->xkb_context, map_shm,
            XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_shm, size);
    close(fd);

    struct xkb_state *xkb_state = xkb_state_new(xkb_keymap);
    xkb_keymap_unref(client_state->xkb_keymap);
    xkb_state_unref(client_state->xkb_state);
    client_state->xkb_keymap = xkb_keymap;
    client_state->xkb_state = xkb_state;
}

static void
wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
                  uint32_t serial, struct wl_surface *surface,
                  struct wl_array *keys) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    fprintf(stdout, "keyboard enter; keys pressed are:\n");
    uint32_t *key;
//    wl_array_for_each(key, keys)
    for (key = static_cast<uint32_t *>((keys)->data); \
         (const char *) key < ((const char *) (keys)->data + (keys)->size); \
         (key)++) {
        char buf[128];
        xkb_keysym_t sym = xkb_state_key_get_one_sym(
                client_state->xkb_state, *key + 8);
        xkb_keysym_get_name(sym, buf, sizeof(buf));
        fprintf(stdout, "sym: %-12s (%d), ", buf, sym);
        xkb_state_key_get_utf8(client_state->xkb_state,
                               *key + 8, buf, sizeof(buf));
        fprintf(stdout, "utf8: '%s'\n", buf);
    }
}

static void
wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
                uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    char buf[128];
    uint32_t keycode = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(
            client_state->xkb_state, keycode);
    xkb_keysym_get_name(sym, buf, sizeof(buf));
    const char *action =
            state == WL_KEYBOARD_KEY_STATE_PRESSED ? "press" : "release";
    fprintf(stdout, "key %s: sym: %-12s (%d), ", action, buf, sym);
    xkb_state_key_get_utf8(client_state->xkb_state, keycode,
                           buf, sizeof(buf));
    fprintf(stdout, "utf8: '%s'\n", buf);
}

static void
wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
                  uint32_t serial, struct wl_surface *surface) {
    fprintf(stdout, "keyboard leave\n");
}

static void
wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
                      uint32_t serial, uint32_t mods_depressed,
                      uint32_t mods_latched, uint32_t mods_locked,
                      uint32_t group) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    xkb_state_update_mask(client_state->xkb_state,
                          mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

static void
wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
                        int32_t rate, int32_t delay) {
    /* Left as an exercise for the reader */
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
        .keymap = wl_keyboard_keymap,
        .enter = wl_keyboard_enter,
        .leave = wl_keyboard_leave,
        .key = wl_keyboard_key,
        .modifiers = wl_keyboard_modifiers,
        .repeat_info = wl_keyboard_repeat_info,
};

/// --------- wl touch listener

static struct touch_point *
get_touch_point(struct client_state *client_state, int32_t id) {
    struct touch_event *touch = &client_state->touch_event;
    const size_t nmemb = sizeof(touch->points) / sizeof(struct touch_point);
    int invalid = -1;
    for (size_t i = 0; i < nmemb; ++i) {
        if (touch->points[i].id == id) {
            return &touch->points[i];
        }
        if (invalid == -1 && !touch->points[i].valid) {
            invalid = i;
        }
    }
    if (invalid == -1) {
        return NULL;
    }
    touch->points[invalid].valid = true;
    touch->points[invalid].id = id;
    return &touch->points[invalid];
}

static void
wl_touch_down(void *data, struct wl_touch *wl_touch, uint32_t serial,
              uint32_t time, struct wl_surface *surface, int32_t id,
              wl_fixed_t x, wl_fixed_t y) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    struct touch_point *point = get_touch_point(client_state, id);
    if (point == NULL) {
        return;
    }
    point->event_mask |= TOUCH_EVENT_UP;
    point->surface_x = wl_fixed_to_double(x),
            point->surface_y = wl_fixed_to_double(y);
    client_state->touch_event.time = time;
    client_state->touch_event.serial = serial;
}

static void
wl_touch_up(void *data, struct wl_touch *wl_touch, uint32_t serial,
            uint32_t time, int32_t id) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    struct touch_point *point = get_touch_point(client_state, id);
    if (point == NULL) {
        return;
    }
    point->event_mask |= TOUCH_EVENT_UP;
}

static void
wl_touch_motion(void *data, struct wl_touch *wl_touch, uint32_t time,
                int32_t id, wl_fixed_t x, wl_fixed_t y) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    struct touch_point *point = get_touch_point(client_state, id);
    if (point == NULL) {
        return;
    }
    point->event_mask |= TOUCH_EVENT_MOTION;
    point->surface_x = x, point->surface_y = y;
    client_state->touch_event.time = time;
}

static void
wl_touch_cancel(void *data, struct wl_touch *wl_touch) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    client_state->touch_event.event_mask |= TOUCH_EVENT_CANCEL;
}

static void
wl_touch_shape(void *data, struct wl_touch *wl_touch,
               int32_t id, wl_fixed_t major, wl_fixed_t minor) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    struct touch_point *point = get_touch_point(client_state, id);
    if (point == NULL) {
        return;
    }
    point->event_mask |= TOUCH_EVENT_SHAPE;
    point->major = major, point->minor = minor;
}

static void
wl_touch_orientation(void *data, struct wl_touch *wl_touch,
                     int32_t id, wl_fixed_t orientation) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    struct touch_point *point = get_touch_point(client_state, id);
    if (point == NULL) {
        return;
    }
    point->event_mask |= TOUCH_EVENT_ORIENTATION;
    point->orientation = orientation;
}


static void
wl_touch_frame(void *data, struct wl_touch *wl_touch) {
    struct client_state *client_state = static_cast<struct client_state *>(data);
    struct touch_event *touch = &client_state->touch_event;
    const size_t nmemb = sizeof(touch->points) / sizeof(struct touch_point);
    fprintf(stdout, "touch event @ %d:\n", touch->time);

    for (size_t i = 0; i < nmemb; ++i) {
        struct touch_point *point = &touch->points[i];
        if (!point->valid) {
            continue;
        }
        fprintf(stdout, "point %d: ", touch->points[i].id);

        if (point->event_mask & TOUCH_EVENT_DOWN) {
            fprintf(stdout, "down %f,%f ",
                    wl_fixed_to_double(point->surface_x),
                    wl_fixed_to_double(point->surface_y));
        }

        if (point->event_mask & TOUCH_EVENT_UP) {
            fprintf(stdout, "up ");
        }

        if (point->event_mask & TOUCH_EVENT_MOTION) {
            fprintf(stdout, "motion %f,%f ",
                    wl_fixed_to_double(point->surface_x),
                    wl_fixed_to_double(point->surface_y));
        }

        if (point->event_mask & TOUCH_EVENT_SHAPE) {
            fprintf(stdout, "shape %fx%f ",
                    wl_fixed_to_double(point->major),
                    wl_fixed_to_double(point->minor));
        }

        if (point->event_mask & TOUCH_EVENT_ORIENTATION) {
            fprintf(stdout, "orientation %f ",
                    wl_fixed_to_double(point->orientation));
        }

        point->valid = false;
        fprintf(stdout, "\n");
    }
}


static const struct wl_touch_listener wl_touch_listener = {
        .down = wl_touch_down,
        .up = wl_touch_up,
        .motion = wl_touch_motion,
        .frame = wl_touch_frame,
        .cancel = wl_touch_cancel,
        .shape = wl_touch_shape,
        .orientation = wl_touch_orientation,
};

/// --------- wl seat listener

static void
wl_seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
    struct client_state *state = static_cast<client_state *>(data);

    bool have_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;

    if (have_pointer && state->wl_pointer == NULL) {
        state->wl_pointer = wl_seat_get_pointer(state->wl_seat);
        wl_pointer_add_listener(state->wl_pointer,
                                &wl_pointer_listener, state);
    } else if (!have_pointer && state->wl_pointer != NULL) {
        wl_pointer_release(state->wl_pointer);
        state->wl_pointer = NULL;
    }

    bool have_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;

    if (have_keyboard && state->wl_keyboard == NULL) {
        state->wl_keyboard = wl_seat_get_keyboard(state->wl_seat);
        wl_keyboard_add_listener(state->wl_keyboard,
                                 &wl_keyboard_listener, state);
    } else if (!have_keyboard && state->wl_keyboard != NULL) {
        wl_keyboard_release(state->wl_keyboard);
        state->wl_keyboard = NULL;
    }

    bool have_touch = capabilities & WL_SEAT_CAPABILITY_TOUCH;

    if (have_touch && state->wl_touch == NULL) {
        state->wl_touch = wl_seat_get_touch(state->wl_seat);
        wl_touch_add_listener(state->wl_touch,
                              &wl_touch_listener, state);
    } else if (!have_touch && state->wl_touch != NULL) {
        wl_touch_release(state->wl_touch);
        state->wl_touch = NULL;
    }

}

static void
wl_seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
    fprintf(stdout, "seat name: %s\n", name);
}

static const struct wl_seat_listener wl_seat_listener = {
        .capabilities = wl_seat_capabilities,
        .name = wl_seat_name,
};

/// --------- xdg wm base listener

static void
xdg_wm_base_ping(void *data,
                 struct xdg_wm_base *xdg_wm_base,
                 uint32_t serial) {
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
        .ping  = xdg_wm_base_ping
};

/// --------- main

int
main(int argc, char *argv[]) {
    struct client_state state = {0};
    state.width = 640;
    state.height = 480;

    /// connect display
    state.wl_display = wl_display_connect(NULL);
    state.wl_registry = wl_display_get_registry(state.wl_display);
    state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    wl_registry_add_listener(state.wl_registry, &registry_listener, &state);
//    wl_shm_add_listener(registry, &registry_listener, &state);
    wl_display_roundtrip(state.wl_display);

    xdg_wm_base_add_listener(state.xdg_wm_base, &xdg_wm_base_listener, &state);
    wl_seat_add_listener(state.wl_seat, &wl_seat_listener, &state);

    /// create surface
    state.wl_surface = wl_compositor_create_surface(state.wl_compositor);

    /// get xdg_surface
    state.xdg_surface = xdg_wm_base_get_xdg_surface(
            state.xdg_wm_base, state.wl_surface);

    xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);

    /// get toplevel
    state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
    xdg_toplevel_add_listener(state.xdg_toplevel,
                              +&xdg_toplevel_listener, &state);
    xdg_toplevel_set_title(state.xdg_toplevel, "Example client");
    wl_surface_commit(state.wl_surface);

//    struct wl_callback *cb = wl_surface_frame(state.wl_surface);
//    wl_callback_add_listener(cb, &wl_surface_frame_listener, &state);




    /// dispatch loop
    while (!state.closed && wl_display_dispatch(state.wl_display)) {
        /* This space deliberately left blank */
    }

    /// TODO destory/free resources

    return 0;
}

