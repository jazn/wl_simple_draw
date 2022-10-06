#include <stdint.h>
#include <stdio.h>
#include <wayland-client.h>
#include <cstring>
#include "shm_tools.h"

struct client_state {
    /* Globals */
    struct wl_display *wl_display;
    struct wl_registry *wl_registry;
    struct wl_shm *wl_shm;
    struct wl_compositor *wl_compositor;
    struct xdg_wm_base *xdg_wm_base;
    /* Objects */
    struct wl_surface *wl_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
};


static void
registry_handle_global(void *data, struct wl_registry *wl_registry,
                       uint32_t name, const char *interface, uint32_t version)
{
    printf("interface: '%s', version: %d, name: %d\n",
           interface, version, name);

    struct client_state *state = static_cast<client_state *>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->wl_compositor = static_cast<wl_compositor *>(wl_registry_bind(
                wl_registry, name, &wl_compositor_interface, 5));
    }
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->wl_shm = static_cast<wl_shm *>(wl_registry_bind(
                wl_registry, name, &wl_shm_interface, 1));
    }
}


static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
                              uint32_t name)
{
    // This space deliberately left blank
}

static const struct wl_registry_listener
        registry_listener = {
        .global = registry_handle_global,
        .global_remove = registry_handle_global_remove,
};

int
main(int argc, char *argv[])
{
    struct client_state state = {0 };

    /// connect display
    struct wl_display *display = wl_display_connect(NULL);
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &state);
//    wl_shm_add_listener(registry, &registry_listener, &state);
    wl_display_roundtrip(display);

    /// create surface
    struct wl_surface *surface = wl_compositor_create_surface(state.wl_compositor);

    /// create mem pool
    const int width = 1920, height = 1080;
    const int stride = width * 4;
    const int shm_pool_size = height * stride * 2;

    int fd = allocate_shm_file(shm_pool_size);
    uint8_t *pool_data = static_cast<uint8_t *>(mmap(NULL, shm_pool_size,
                                                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));

    struct wl_shm *shm = state.wl_shm; // Bound from registry
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, shm_pool_size);

    /// create buffer from pool
    int index = 0;
    int offset = height * stride * index;
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, offset,
                                                         width, height, stride, WL_SHM_FORMAT_XRGB8888);

    /// set memdata to something
    uint32_t *pixels = (uint32_t *)&pool_data[offset];
    memset(pixels, 0, width * height * 4);

    /// checker perhaps
//    uint32_t *pixels = (uint32_t *)&pool_data[offset];
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if ((x + y / 8 * 8) % 16 < 8) {
                pixels[y * width + x] = 0xFF666666;
            } else {
                pixels[y * width + x] = 0xFFEEEEEE;
            }
        }
    }

    /// attach buffer to surface
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, UINT32_MAX, UINT32_MAX);
    wl_surface_commit(surface);

    return 0;
}

