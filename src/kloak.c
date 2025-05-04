/*
 * Copyright (c) 2025 - 2025 ENCRYPTED SUPPORT LLC <adrelanos@whonix.org>
 * See the file COPYING for copying conditions.
 */

#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <stdbool.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <math.h>

#include <wayland-client.h>
#include "xdg-output-protocol.h"
#include "wlr-layer-shell.h"
#include "wlr-virtual-pointer.h"
#include "virtual-keyboard.h"

#include <libinput.h>

#include <libevdev/libevdev.h>

#include <xkbcommon/xkbcommon.h>

#include "kloak.h"

/********************/
/* global variables */
/********************/

double cursor_x = 0;
double cursor_y = 0;
double prev_cursor_x = 0;
double prev_cursor_y = 0;

struct disp_state state = { 0 };
struct libinput *li;

struct pollfd *ev_fds;

/*********************/
/* utility functions */
/*********************/

static void randname(char *buf, size_t len) {
  int randfd = open("/dev/urandom", O_RDONLY);
  if (randfd < 0) {
    fprintf(stderr, "FATAL ERROR: Could not open /dev/urandom: %s\n",
      strerror(errno));
    exit(1);
  }

  char randchar = 0;
  for (size_t i = 0; i < len; ++i) {
    do {
      if (read(randfd, &randchar, 1) < 1) {
        fprintf(stderr,
          "FATAL ERROR: Could not read byte from /dev/urandom: %s\n",
          strerror(errno));
        exit(1);
      }
      if (randchar & 0x80)
        randchar ^= 0x80;
    } while (randchar >= (127 - 127 % 52));

    randchar %= 52;
    if (randchar < 26) {
      randchar += 65;
    } else {
      randchar += 71;
    }
    buf[i] = randchar;
  }

  if (close(randfd) == -1) {
    fprintf(stderr, "FATAL ERROR: Could not close /dev/urandom: %s\n",
      strerror(errno));
    exit(1);
  }
}

static int create_shm_file(size_t size) {
  int retries = 100;
  int fd = -1;
  do {
    char name[] = "/kloak-XXXXXXXXXX";
    randname(name + sizeof(name) - 11, 10);
    --retries;
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      shm_unlink(name);
      break;
    }
  } while (retries > 0 && errno == EEXIST);

  if (fd == -1) {
    fprintf(stderr,
      "FATAL ERROR: Could not create shared memory fd: Resource temporarily unavailable\n");
    exit(1);
  }

  int ret;
  do {
    ret = ftruncate(fd, size);
  } while (ret < 0 && errno == EINTR);
  if (ret < 0) {
    close(fd);
    fprintf(stderr,
      "FATAL ERROR: Could not allocate shared memory block: %s\n",
      strerror(errno));
    exit(1);
  }

  return fd;
}

static void recalc_global_space(struct disp_state * state) {
  uint32_t ul_corner_x = UINT32_MAX;
  uint32_t ul_corner_y = UINT32_MAX;
  uint32_t br_corner_x = 0;
  uint32_t br_corner_y = 0;

  struct output_geometry *screen_list[MAX_DRAWABLE_LAYERS];
  size_t screen_list_len = 0;

  for (size_t i = 0; i < MAX_DRAWABLE_LAYERS; ++i) {
    if (!state->output_geometry[i])
      continue;
    if (!state->output_geometry[i]->init_done)
      continue;
    screen_list[screen_list_len] = state->output_geometry[i];
    ++screen_list_len;
    if (state->output_geometry[i]->x < ul_corner_x) {
      ul_corner_x = state->output_geometry[i]->x;
    }
    if (state->output_geometry[i]->y < ul_corner_y) {
      ul_corner_y = state->output_geometry[i]->y;
    }
    uint32_t temp_br_x
      = state->output_geometry[i]->x + state->output_geometry[i]->width;
    uint32_t temp_br_y
      = state->output_geometry[i]->y + state->output_geometry[i]->height;
    if (temp_br_x > br_corner_x) {
      br_corner_x = temp_br_x;
    }
    if (temp_br_y > br_corner_y) {
      br_corner_y = temp_br_y;
    }
  }

  if (ul_corner_x > br_corner_x) {
    /* Maybe we just haven't gotten a valid screen state yet, silently fail */
    return;
  }
  if (ul_corner_y > br_corner_y) {
    /* same as above */
    return;
  }

  struct output_geometry *conn_screen_list[MAX_DRAWABLE_LAYERS];
  conn_screen_list[0] = screen_list[0];
  size_t conn_screen_list_len = 1;

  /*
   * Check for gaps between the screens. We don't support running if gaps are
   * present.
   *
   * If a screen's left edge touches another screen, current_screen->x ==
   * left_screen->x + left_screen->width.
   * If a screen's right edge touches another screen, current_screen->x +
   * current_screen->width == right_screen->x
   * If a screen's top edge touches another screen, current_screen->y ==
   * up_screen->y + up_screen->height
   * If a screen's bottom edge touches another screen, current_screen->y +
   * current_screen->height == bottom_screen->y
   */
  for (size_t i = 0; i < conn_screen_list_len; ++i) {
    for (size_t j = 0; j < screen_list_len; ++j) {
      bool screen_in_conn_list = false;
      for (size_t k = 0; k < conn_screen_list_len; ++k) {
        if (screen_list[j] == conn_screen_list[k]) {
          screen_in_conn_list = true;
          break;
        }
      }
      if (screen_in_conn_list)
        continue;
      struct output_geometry *conn_screen = conn_screen_list[i];
      struct output_geometry *cur_screen = screen_list[j];
      if ((conn_screen->x == cur_screen->x + cur_screen->width)
        || (conn_screen->x + conn_screen->width == cur_screen->x)
        || (conn_screen->y == cur_screen->y + cur_screen->height)
        || (conn_screen->y + conn_screen->height == cur_screen->y) ) {
        /* Found a touching screen! */
        conn_screen_list[conn_screen_list_len] = cur_screen;
        ++conn_screen_list_len;
      }
    }
  }

  if (conn_screen_list_len != screen_list_len) {
    fprintf(stderr,
      "FATAL ERROR: Multiple screens are attached and gaps are present between them. kloak cannot operate in this configuration.\n");
    exit(1);
  }

  state->global_space_width = br_corner_x;
  state->global_space_height = br_corner_y;
}

static struct screen_local_coord abs_coord_to_screen_local_coord(int32_t x,
  int32_t y) {
  struct screen_local_coord out_data = { 0 };

  for (size_t i = 0; i < MAX_DRAWABLE_LAYERS; ++i) {
    if (!state.output_geometry[i]) {
      continue;
    }
    if (!state.output_geometry[i]->init_done) {
      continue;
    }
    if (!(x >= state.output_geometry[i]->x)) {
      continue;
    }
    if (!(y >= state.output_geometry[i]->y)) {
      continue;
    }
    if (!(x < state.output_geometry[i]->x
      + state.output_geometry[i]->width)) {
      continue;
    }
    if (!(y < state.output_geometry[i]->y
      + state.output_geometry[i]->height)) {
      continue;
    }
    out_data.output_idx = i;
    out_data.x = x - state.output_geometry[i]->x;
    out_data.y = y - state.output_geometry[i]->y;
    out_data.valid = true;
    break;
  }

  /* There is a possibility that out_data will contain all zeros; if this
   * happens, it means that no output covered the requested coordinates in the
   * compositor's global space. That's a valid thing to tell a caller, so just
   * return it anyway. */
  return out_data;
}

static struct coord screen_local_coord_to_abs_coord(uint32_t x, uint32_t y,
  int32_t output_idx) {
  struct coord output = {
    .x = -1,
    .y = -1,
  };

  if (!state.layer[output_idx]
    || x >= state.output_geometry[output_idx]->width
    || y >= state.output_geometry[output_idx]->height) {
    return output;
  }
  output.x = state.output_geometry[output_idx]->x + x;
  output.y = state.output_geometry[output_idx]->y + y;
  return output;
}

struct coord traverse_line(struct coord start, struct coord end,
  int32_t pos) {
  if (pos == 0) return start;
  struct coord output = { 0 };

  double num = ((double) end.y) - ((double) start.y);
  double denom = ((double) start.x) - ((double) end.x);
  if (denom == 0) {
    /* vertical line */
    output.x = start.x;
    if (start.y < end.y) {
      output.y = start.y + pos;
    } else {
      output.y = start.y - pos;
    }
    return output;
  }

  double slope = num / denom;
  double steep;
  if (slope < 0)
    steep = -slope;
  else
    steep = slope;

  if (steep < 1) {
    if (start.x < end.x) {
      output.x = start.x + pos;
    } else {
      output.x = start.x - pos;
    }
    if (start.y < end.y) {
      output.y = start.y + (int32_t)((double) pos * steep);
    } else {
      output.y = start.y - (int32_t)((double) pos * steep);
    }
  } else {
    if (start.y < end.y) {
      output.y = start.y + pos;
    } else {
      output.y = start.y - pos;
    }
    if (start.x < end.x) {
      output.x = start.x + (int32_t)((double) pos * (1 / steep));
    } else {
      output.x = start.x - (int32_t)((double) pos * (1 / steep));
    }
  }

  return output;
}

static void draw_block(uint32_t *pixbuf, int32_t x, int32_t y,
  int32_t layer_width, int32_t layer_height, int32_t rad, bool crosshair) {
  int32_t start_x = x - rad;
  if (start_x < 0) start_x = 0;
  int32_t start_y = y - rad;
  if (start_y < 0) start_y = 0;
  int32_t end_x = x + rad;
  if (end_x >= layer_width) end_x = layer_width - 1;
  int32_t end_y = y + rad;
  if (end_y >= layer_height) end_y = layer_height - 1;

  for (int32_t work_y = start_y; work_y < end_y; ++work_y) {
    for (int32_t work_x = start_x; work_x < end_x; ++work_x) {
      if (crosshair && work_x == x) {
        pixbuf[work_y * layer_width + work_x] = 0xffff0000;
      } else if (crosshair && work_y == y) {
        pixbuf[work_y * layer_width + work_x] = 0xffff0000;
      } else {
        pixbuf[work_y * layer_width + work_x] = 0x00000000;
      }
    }
  }
}

/********************/
/* wayland handling */
/********************/

static void registry_handle_global(void *data, struct wl_registry *registry,
  uint32_t name, const char *interface, uint32_t version) {
  struct disp_state *state = data;
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    state->compositor = wl_registry_bind(registry, name,
      &wl_compositor_interface, 5);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    if (!state->seat_set) {
      state->seat = wl_registry_bind(registry, name, &wl_seat_interface, 9);
      state->seat_set = true;
    } else {
      fprintf(stderr,
        "WARNING: Multiple seats detected, all but first will be ignored.\n");
    }
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 2);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    bool new_layer_allocated = false;
    for (size_t i = 0; i < MAX_DRAWABLE_LAYERS; ++i) {
      if (!state->layer[i]) {
        state->output[i] = wl_registry_bind(registry, name,
          &wl_output_interface, 4);
        state->layer[i] = malloc(sizeof(struct drawable_layer));
        memset(state->layer[i], 0, sizeof(struct drawable_layer));
        state->layer[i]->frame_released = true;
        state->layer[i]->frame_pending = true;
        state->layer[i]->last_drawn_cursor_x = -1;
        state->layer[i]->last_drawn_cursor_y = -1;
        allocate_drawable_layer(state, state->layer[i], state->output[i]);
        if (state->xdg_output_manager) {
          /*
           * We can only create xdg_outputs for wl_outputs if we've received
           * the zxdg_output_manager_v1 object from the server, thus the 'if'
           * condition here. When we *do* get the zxdg_output_manager_v1
           * object, we go through and make xdg_outputs for any wl_outputs
           * that were sent too early.
           *
           * NOTE: We do not add wl_output listeners until we have the
           * zxdg_output_manager_v1 object to avoid the situation where we get
           * wl_output_done signals before an xdg_output is created for a
           * wl_output.
           */
          state->xdg_output[i] = zxdg_output_manager_v1_get_xdg_output(
            state->xdg_output_manager, state->output[i]);
          zxdg_output_v1_add_listener(state->xdg_output[i],
            &xdg_output_listener, state);
          wl_output_add_listener(state->output[i], &output_listener, state);
          state->output_geometry[i] = malloc(sizeof(struct output_geometry));
          memset(state->output_geometry[i], 0, sizeof(struct output_geometry));
        }
        new_layer_allocated = true;
        break;
      }
    }
    if (!new_layer_allocated) {
      fprintf(stderr,
        "FATAL ERROR: Cannot handle more than %d displays attached at once!",
        MAX_DRAWABLE_LAYERS);
      exit(1);
    }
  } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
    state->xdg_output_manager = wl_registry_bind(registry, name,
      &zxdg_output_manager_v1_interface, 3);
    for (size_t i = 0; i < MAX_DRAWABLE_LAYERS; ++i) {
      if (state->output[i]) {
        if (!state->xdg_output[i]) {
          /* This is where we make xdg_outputs for any wl_outputs that were
           * sent too early. */
          state->xdg_output[i] = zxdg_output_manager_v1_get_xdg_output(
            state->xdg_output_manager, state->output[i]);
          zxdg_output_v1_add_listener(state->xdg_output[i],
            &xdg_output_listener, state);
          wl_output_add_listener(state->output[i], &output_listener, state);
          state->output_geometry[i] = malloc(sizeof(struct output_geometry));
          memset(state->output_geometry[i], 0, sizeof(struct output_geometry));
        }
      }
    }
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    state->layer_shell = wl_registry_bind(registry, name,
      &zwlr_layer_shell_v1_interface, 4);
  } else if (
    strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
    state->virt_pointer_manager = wl_registry_bind(registry, name,
      &zwlr_virtual_pointer_manager_v1_interface, 2);
  } else if (
    strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
    state->virt_kb_manager = wl_registry_bind(registry, name,
      &zwp_virtual_keyboard_manager_v1_interface, 1);
  }
}

static void registry_handle_global_remove(void *data,
  struct wl_registry *registry, uint32_t name) {
  struct disp_state *state = data;
  for (size_t i = 0; i < MAX_DRAWABLE_LAYERS; ++i) {
    if (state->layer[i]) {
      if (wl_proxy_get_id((struct wl_proxy *) state->output[i]) == name) {
        struct drawable_layer *layer = state->layer[i];
        wl_output_release(state->output[i]);
        zxdg_output_v1_destroy(state->xdg_output[i]);
        free(state->output_geometry[i]);
        state->output[i] = NULL;
        wl_surface_destroy(layer->surface);
        zwlr_layer_surface_v1_destroy(layer->layer_surface);
        zwlr_virtual_pointer_v1_destroy(layer->virt_pointer);
        if (layer->pixbuf != NULL) {
          munmap(layer->pixbuf, layer->size);
        }
        if (layer->shm_pool != NULL) {
          wl_shm_pool_destroy(layer->shm_pool);
        }
        free(layer);
        state->layer[i] = NULL;
        recalc_global_space(state);
        break;
      }
    }
  }
}

static void seat_handle_name(void *data, struct wl_seat *seat,
  const char *name) {
  struct disp_state *state = data;
  state->seat_name = name;
}

static void seat_handle_capabilities(void *data, struct wl_seat *seat,
  uint32_t capabilities) {
  struct disp_state *state = data;
  state->seat_caps = capabilities;
  if (capabilities | WL_SEAT_CAPABILITY_KEYBOARD) {
    state->kb = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(state->kb, &kb_listener, state);
  } else {
    fprintf(stderr,
      "FATAL ERROR: No keyboard capability for seat, cannot continue.\n");
    exit(1);
  }
}

static void kb_handle_keymap(void *data, struct wl_keyboard *kb,
  uint32_t format, int32_t fd, uint32_t size) {
  struct disp_state *state = data;
  char *kb_map_shm = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (kb_map_shm == MAP_FAILED) {
    fprintf(stderr, "FATAL ERROR: Could not mmap xkb layout!");
    exit(1);
  }
  if (state->old_kb_map_shm) {
    if (strcmp(state->old_kb_map_shm, kb_map_shm) == 0) {
      /* New and old maps are the same, cleanup and return. */
      munmap(kb_map_shm, size);
      close(fd);
      return;
    } else {
      munmap(state->old_kb_map_shm, state->old_kb_map_shm_size);
    }
  }
  zwp_virtual_keyboard_v1_keymap(state->virt_kb, format, fd, size);
  state->old_kb_map_shm = kb_map_shm;
  state->old_kb_map_shm_size = size;
  if (state->xkb_keymap) {
    xkb_keymap_unref(state->xkb_keymap);
  }
  state->xkb_keymap = xkb_keymap_new_from_string(
    state->xkb_ctx, kb_map_shm, XKB_KEYMAP_FORMAT_TEXT_V1,
    XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!state->xkb_keymap) {
    fprintf(stderr, "FATAL ERROR: Could not compile xkb layout!");
    exit(1);
  }
  close(fd);
  if (state->xkb_state) {
    xkb_state_unref(state->xkb_state);
  }
  state->xkb_state = xkb_state_new(state->xkb_keymap);
  if (!state->xkb_state) {
    fprintf(stderr, "FATAL ERROR: Could not create xkb state!");
    exit(1);
  }
  state->virt_kb_keymap_set = true;
}

static void kb_handle_enter(void *data, struct wl_keyboard *kb,
  uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
  wl_array_release(keys);
}

static void kb_handle_leave(void *data, struct wl_keyboard *kb,
  uint32_t serial, struct wl_surface *surface) {
  ;
}

static void kb_handle_key(void *data, struct wl_keyboard *kb,
  uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
  ;
}

static void kb_handle_modifiers(void *data, struct wl_keyboard *kb,
  uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
  uint32_t mods_locked, uint32_t group) {
  ;
}

static void kb_handle_repeat_info(void *data, struct wl_keyboard *kb,
  int32_t rate, int32_t delay) {
  ;
}

static void wl_buffer_release(void *data, struct wl_buffer *buffer) {
  for (size_t i = 0; i < MAX_DRAWABLE_LAYERS; ++i) {
    if (state.layer[i]) {
      if (state.layer[i]->buffer == buffer) {
        state.layer[i]->frame_released = true;
        state.layer[i]->buffer = NULL;
        break;
      }
    }
  }
  wl_buffer_destroy(buffer);
}

static void xdg_output_handle_logical_position(void *data,
  struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y) {
  struct disp_state *state = data;
  for (size_t i = 0; i < MAX_DRAWABLE_LAYERS; ++i) {
    if (state->xdg_output[i] == xdg_output) {
      state->output_geometry[i]->x = x;
      state->output_geometry[i]->y = y;
      break;
    }
  }
}

static void xdg_output_handle_logical_size(void *data,
  struct zxdg_output_v1 *xdg_output, int32_t width, int32_t height) {
  struct disp_state *state = data;
  for (size_t i = 0; i < MAX_DRAWABLE_LAYERS; ++i) {
    if (state->xdg_output[i] == xdg_output) {
      state->output_geometry[i]->width = width;
      state->output_geometry[i]->height = height;
      break;
    }
  }
}

static void wl_output_handle_geometry(void *data, struct wl_output *output,
  int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
  int32_t subpixel, const char *make, const char *model, int32_t transform) {
  ;
}

static void wl_output_handle_mode(void *data, struct wl_output *output,
  uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
  ;
}

static void wl_output_info_done(void *data, struct wl_output *output) {
  struct disp_state *state = data;
  for (size_t i = 0; i < MAX_DRAWABLE_LAYERS; ++i) {
    if (state->output[i] == output) {
      struct output_geometry *geometry = state->output_geometry[i];
      if (geometry->x == 0 && geometry->y == 0 && geometry->width == 0
        && geometry->height == 0)
        return;
      state->output_geometry[i]->init_done = true;
      recalc_global_space(state);
      break;
    }
  }
}

static void wl_output_handle_scale(void *data, struct wl_output *output,
  int32_t factor) {
  ;
}

static void wl_output_handle_name(void *data, struct wl_output *output,
  const char *name) {
  ;
}

static void wl_output_handle_description(void *data, struct wl_output *output,
  const char *description) {
  ;
}

static void xdg_output_info_done(void *data,
  struct zxdg_output_v1 *xdg_output) {
  ;
}

static void xdg_output_handle_name(void *data,
  struct zxdg_output_v1 *xdg_output, const char *name) {
  ;
}

static void xdg_output_handle_description(void *data,
  struct zxdg_output_v1 *xdg_output, const char *description) {
  ;
}

static void layer_surface_configure(void *data,
  struct zwlr_layer_surface_v1 *layer_surface, uint32_t serial, uint32_t width,
  uint32_t height) {
  struct disp_state *state = data;
  struct drawable_layer *layer;
  for (size_t i = 0; i < MAX_DRAWABLE_LAYERS; ++i) {
    if (state->layer[i]) {
      if (state->layer[i]->layer_surface == layer_surface) {
        layer = state->layer[i];
        break;
      }
    }
  }
  layer->width = width;
  layer->height = height;
  layer->stride = width * 4;
  layer->size = layer->stride * (size_t) height;
  int shm_fd = create_shm_file(layer->size);
  if (shm_fd == -1) {
    fprintf(stderr,
      "FATAL ERROR: Cannot allocate shared memory block for frame: %s\n",
      strerror(errno));
    exit(1);
  }
  layer->pixbuf = mmap(NULL, layer->size, PROT_READ | PROT_WRITE,
    MAP_SHARED, shm_fd, 0);
  if (layer->pixbuf == MAP_FAILED) {
    close(shm_fd);
    fprintf(stderr,
      "FATAL ERROR: Failed to map shared memory block for frame: %s\n",
      strerror(errno));
    exit(1);
  }
  layer->shm_pool = wl_shm_create_pool(state->shm, shm_fd,
    layer->size);

  struct wl_region *zeroed_region = wl_compositor_create_region(state->compositor);
  wl_region_add(zeroed_region, 0, 0, 0, 0);
  wl_surface_set_input_region(layer->surface, zeroed_region);

  zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
  layer->layer_surface_configured = true;
  wl_region_destroy(zeroed_region);
  draw_frame(layer);
}

/*********************/
/* libinput handling */
/*********************/

static int li_open_restricted(const char *path, int flags, void *user_data) {
  int fd = open(path, flags);
  int one = 1;
  if (ioctl(fd, EVIOCGRAB, &one) < 0) {
    fprintf(stderr, "FATAL ERROR: Could not grab evdev device '%s'!\n", path);
    exit(1);
  }
  return fd < 0 ? -errno : fd;
}

static void li_close_restricted(int fd, void *user_data) {
  close(fd);
}

/************************/
/* high-level functions */
/************************/

static void draw_frame(struct drawable_layer *layer) {
  if (!layer->frame_released)
    return;
  if (!layer->layer_surface_configured)
    return;
  layer->frame_pending = false;

  struct wl_buffer *buffer = wl_shm_pool_create_buffer(layer->shm_pool,
    0, layer->width,
    layer->height, layer->stride, WL_SHM_FORMAT_ARGB8888);

  struct screen_local_coord coord = abs_coord_to_screen_local_coord(
    (int32_t) cursor_x, (int32_t) cursor_y);
  struct screen_local_coord prev_coord = abs_coord_to_screen_local_coord(
    (int32_t) prev_cursor_x, (int32_t) prev_cursor_y);

  bool cursor_is_on_layer = false;
  for (size_t i = 0; i < MAX_DRAWABLE_LAYERS; ++i) {
    if (state.layer[i] == layer) {
      if (i == coord.output_idx) {
        cursor_is_on_layer = true;
      }
    }
  }

  if (layer->last_drawn_cursor_x >= 0 && layer->last_drawn_cursor_y >= 0) {
    /* Blank out the previous cursor location */
    draw_block(layer->pixbuf, layer->last_drawn_cursor_x,
      layer->last_drawn_cursor_y, layer->width, layer->height,
      CURSOR_RADIUS, false);
    damage_surface_enh(layer->surface,
      layer->last_drawn_cursor_x - CURSOR_RADIUS,
      layer->last_drawn_cursor_y - CURSOR_RADIUS,
      layer->last_drawn_cursor_x + CURSOR_RADIUS,
      layer->last_drawn_cursor_y + CURSOR_RADIUS);
  }
  if (cursor_is_on_layer) {
    /* Draw red crosshairs at the pointer location */
    draw_block(layer->pixbuf, coord.x, coord.y, layer->width, layer->height,
      CURSOR_RADIUS, true);
    damage_surface_enh(layer->surface, coord.x - CURSOR_RADIUS,
      coord.y - CURSOR_RADIUS, coord.x + CURSOR_RADIUS,
      coord.y + CURSOR_RADIUS);
  }

  wl_buffer_add_listener(buffer, &buffer_listener, NULL);
  layer->buffer = buffer;
  wl_surface_attach(layer->surface, buffer, 0, 0);
  /*wl_surface_damage_buffer(layer->surface, coord.x - 15, coord.y - 15,
    coord.x + 15, coord.y + 15);
  wl_surface_damage_buffer(layer->surface, prev_coord.x - 15,
    prev_coord.y - 15, prev_coord.x + 15, prev_coord.y + 15);*/
  wl_surface_commit(layer->surface);
  if (cursor_is_on_layer) {
    layer->last_drawn_cursor_x = coord.x;
    layer->last_drawn_cursor_y = coord.y;
  } else {
    layer->last_drawn_cursor_x = -1;
    layer->last_drawn_cursor_y = -1;
  }
  layer->frame_released = false;
}

static void allocate_drawable_layer(struct disp_state *state,
  struct drawable_layer *layer, struct wl_output *output) {
  layer->output = output;
  layer->surface = wl_compositor_create_surface(state->compositor);
  if (!layer->surface) {
    fprintf(stderr, "FATAL ERROR: Could not create Wayland surface!");
    exit(1);
  }
  layer->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
    state->layer_shell, layer->surface, layer->output,
    ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "com.kicksecure.kloak");
  zwlr_layer_surface_v1_add_listener(layer->layer_surface,
    &layer_surface_listener, state);

  zwlr_layer_surface_v1_set_anchor(layer->layer_surface,
    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP);
  zwlr_layer_surface_v1_set_anchor(layer->layer_surface,
    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
  zwlr_layer_surface_v1_set_anchor(layer->layer_surface,
    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
  zwlr_layer_surface_v1_set_anchor(layer->layer_surface,
    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  wl_surface_commit(layer->surface);

  layer->virt_pointer
    = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
    state->virt_pointer_manager, NULL, layer->output);
}

static void damage_surface_enh(struct wl_surface *surface, int32_t x,
  int32_t y, int32_t width, int32_t height) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  wl_surface_damage_buffer(surface, x, y, width, height);
}

static struct screen_local_coord update_virtual_cursor(
  uint32_t ts_milliseconds) {
  struct screen_local_coord prev_coord_data = abs_coord_to_screen_local_coord(
    (int32_t) prev_cursor_x, (int32_t) prev_cursor_y);
  struct screen_local_coord coord_data = { 0 };

  if (!prev_coord_data.valid || !state.layer[prev_coord_data.output_idx]) {
    /* We've somehow gotten into a spot where the previous coordinate data
     * either is invalid or points at an area where there is no screen. Reset
     * everything in the hopes of recovering sanity. */
    for (int32_t i = 0; i < MAX_DRAWABLE_LAYERS; i++) {
      if (state.layer[i]) {
        struct coord sane_location = screen_local_coord_to_abs_coord(0, 0, i);
        prev_cursor_x = sane_location.x;
        prev_cursor_y = sane_location.y;
        cursor_x = sane_location.x;
        cursor_y = sane_location.y;
        prev_coord_data = abs_coord_to_screen_local_coord(
          (int32_t) prev_cursor_x, (int32_t) prev_cursor_y);
      }
    }
  }

  /*
   * Ensure the cursor doesn't move off-screen, and recalculate its end
   * position if it would end up off-screen.
   *
   * This is a bit tricky to do since we can't just look at the intended final
   * location of the mouse and move it there if that location is valid, since
   * that would allow jumping over "voids" in the compositor global space
   * (places whether global space has a pixel but no screen covers that
   * pixel). Instead, we use the following algorithm:
   *
   * - Take the previous cursor position and treat it as a "start location".
   *   Treat the current cursor position as an "end location".
   * - Start walking in a straight line from the start location to the end
   *   location, one pixel at a time.
   * - Once we hit the end location, move the real mouse cursor there.
   * - Each time we walk forward a pixel, calculate the screen-local
   *   coordinates of that pixel, and ensure it actually is on a screen.
   * - If a pixel is NOT on any screen, determine which direction we can move
   *   to get back onto a screen. Move one pixel in that direction, then
   *   change the start location to this new position and adjust the end
   *   location so that we can walk to it moving in a straight line without
   *   moving any further in the dimension we just moved to get back to a
   *   screen. I.e., if we moved horizontally to the left one pixel to get
   *   back on a screen, move the end location so that we can walk to it in a
   *   vertical line. This allows us to "glide" along the wall.
   *
   * This sounds like an awful lot of work, but I couldn't find another way to
   * get the mouse to glide smoothly along edges while still respecting them.
   */
  struct coord start = {
    .x = (int32_t) prev_cursor_x,
    .y = (int32_t) prev_cursor_y,
  };
  struct coord end = {
    .x = (int32_t) cursor_x,
    .y = (int32_t) cursor_y,
  };
  struct coord prev_trav = start;
  bool end_x_hit = false;
  bool end_y_hit = false;
  for (int32_t i = 0; ; ++i) {
    struct coord trav = traverse_line(start, end, i);
    if (trav.x == end.x) end_x_hit = true;
    if (trav.y == end.y) end_y_hit = true;
    struct screen_local_coord trav_coord = abs_coord_to_screen_local_coord(
      trav.x, trav.y);
    if (!trav_coord.valid) {
      /* Figure out what direction we moved when we went off screen, and move
       * move backwards in that direction, but in only one dimension. */
      if (prev_trav.x < trav.x) {
        trav_coord = abs_coord_to_screen_local_coord(trav.x - 1, trav.y);
        if (trav_coord.valid) {
          start.x = trav.x - 1;
          start.y = trav.y;
          end.x = trav.x - 1;
          i = -1;
          continue;
        }
      }
      if (prev_trav.x > trav.x) {
        trav_coord = abs_coord_to_screen_local_coord(trav.x + 1, trav.y);
        if (trav_coord.valid) {
          start.x = trav.x + 1;
          start.y = trav.y;
          end.x = trav.x + 1;
          i = -1;
          continue;
        }
      }
      if (prev_trav.y < trav.y) {
        trav_coord = abs_coord_to_screen_local_coord(trav.x, trav.y - 1);
        if (trav_coord.valid) {
          start.y = trav.y - 1;
          start.x = trav.x;
          end.y = trav.y -1;
          i = -1;
          continue;
        }
      }
      if (prev_trav.y > trav.y) {
        trav_coord = abs_coord_to_screen_local_coord(trav.x, trav.y + 1);
        if (trav_coord.valid) {
          start.y = trav.y + 1;
          start.x = trav.x;
          end.y = trav.y + 1;
          i = -1;
          continue;
        }
      }
    }
    if (end_x_hit && end_y_hit) {
      if ((int32_t) cursor_x != end.x) {
        cursor_x = end.x;
      }
      if ((int32_t) cursor_y != end.y) {
        cursor_y = end.y;
      }
      break;
    }
    prev_trav = trav;
  }
  coord_data = abs_coord_to_screen_local_coord((int32_t) cursor_x,
    (int32_t) cursor_y);

  state.layer[prev_coord_data.output_idx]->frame_pending = true;
  state.layer[coord_data.output_idx]->frame_pending = true;
  zwlr_virtual_pointer_v1_motion_absolute(
    state.layer[coord_data.output_idx]->virt_pointer,
    ts_milliseconds, (uint32_t) coord_data.x, (uint32_t) coord_data.y,
    state.layer[coord_data.output_idx]->width,
    state.layer[coord_data.output_idx]->height);
  return coord_data;
}

static void handle_libinput_event(enum libinput_event_type ev_type) {
  bool mouse_event_handled = false;

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  unsigned int ts_milliseconds = ts.tv_sec * 1000;
  ts_milliseconds += (ts.tv_nsec / 1000000);

  struct screen_local_coord coord_data = { 0 };
  struct libinput_event *li_event = libinput_get_event(li);

  if (ev_type != LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE
    && ev_type != LIBINPUT_EVENT_POINTER_MOTION) {
    coord_data = abs_coord_to_screen_local_coord((int32_t) cursor_x,
      (int32_t) cursor_y);
  }

  if (ev_type == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE) {
    mouse_event_handled = true;
    struct libinput_event_pointer *pointer_event
      = libinput_event_get_pointer_event(li_event);
    double abs_x = libinput_event_pointer_get_absolute_x_transformed(
      pointer_event, state.global_space_width);
    double abs_y = libinput_event_pointer_get_absolute_y_transformed(
      pointer_event, state.global_space_height);
    prev_cursor_x = cursor_x;
    prev_cursor_y = cursor_y;
    cursor_x = abs_x;
    cursor_y = abs_y;
    coord_data = update_virtual_cursor(ts_milliseconds);

  } else if (ev_type == LIBINPUT_EVENT_POINTER_MOTION) {
    mouse_event_handled = true;
    struct libinput_event_pointer *pointer_event
      = libinput_event_get_pointer_event(li_event);
    double rel_x = libinput_event_pointer_get_dx(pointer_event);
    double rel_y = libinput_event_pointer_get_dy(pointer_event);
    prev_cursor_x = cursor_x;
    prev_cursor_y = cursor_y;
    cursor_x += rel_x;
    cursor_y += rel_y;
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_x > state.global_space_width - 1)
      cursor_x = state.global_space_width - 1;
    if (cursor_y > state.global_space_height - 1)
      cursor_y = state.global_space_height - 1;
    coord_data = update_virtual_cursor(ts_milliseconds);

  } else if (ev_type == LIBINPUT_EVENT_POINTER_BUTTON) {
    mouse_event_handled = true;
    struct libinput_event_pointer *pointer_event
      = libinput_event_get_pointer_event(li_event);
    uint32_t button_code = libinput_event_pointer_get_button(pointer_event);
    enum libinput_button_state button_state
      = libinput_event_pointer_get_button_state(pointer_event);
    if (coord_data.valid) {
      if (!state.layer[coord_data.output_idx]) {
        fprintf(stderr,
          "FATAL ERROR: Current coordinate data points to an unallocated drawable layer!\n");
        exit(1);
      }
      if (button_state == LIBINPUT_BUTTON_STATE_PRESSED) {
        /* Both libinput and zwlr_virtual_pointer_v1 use evdev devent codes to
         * identify the button pressed, so we can just pass the data from
         * libinput straight through */
        zwlr_virtual_pointer_v1_button(
          state.layer[coord_data.output_idx]->virt_pointer,
          ts_milliseconds, button_code, WL_POINTER_BUTTON_STATE_PRESSED);
      } else {
        zwlr_virtual_pointer_v1_button(
          state.layer[coord_data.output_idx]->virt_pointer,
          ts_milliseconds, button_code, WL_POINTER_BUTTON_STATE_RELEASED);
      }
    }

  } else if (ev_type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL
    || ev_type == LIBINPUT_EVENT_POINTER_SCROLL_FINGER
    || ev_type == LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS) {
    mouse_event_handled = true;
    struct libinput_event_pointer *pointer_event
      = libinput_event_get_pointer_event(li_event);
    int vert_scroll_present = libinput_event_pointer_has_axis(pointer_event,
      LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
    int horiz_scroll_present = libinput_event_pointer_has_axis(pointer_event,
      LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
    if (coord_data.valid) {
      if (!state.layer[coord_data.output_idx]) {
        fprintf(stderr,
          "FATAL ERROR: Current coordinate data points to an unallocated drawable layer!\n");
        exit(1);
      }

      if (vert_scroll_present) {
        double vert_scroll_value = libinput_event_pointer_get_scroll_value(
          pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
        if (vert_scroll_value == 0) {
          zwlr_virtual_pointer_v1_axis_stop(
            state.layer[coord_data.output_idx]->virt_pointer,
            ts_milliseconds, WL_POINTER_AXIS_VERTICAL_SCROLL);
        } else {
          zwlr_virtual_pointer_v1_axis(
            state.layer[coord_data.output_idx]->virt_pointer,
            ts_milliseconds, WL_POINTER_AXIS_VERTICAL_SCROLL,
            wl_fixed_from_double(vert_scroll_value));
        }
        switch (ev_type) {
          case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
            zwlr_virtual_pointer_v1_axis_source(
              state.layer[coord_data.output_idx]->virt_pointer,
              WL_POINTER_AXIS_SOURCE_WHEEL);
            break;
          case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
            zwlr_virtual_pointer_v1_axis_source(
              state.layer[coord_data.output_idx]->virt_pointer,
              WL_POINTER_AXIS_SOURCE_FINGER);
            break;
          case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
            zwlr_virtual_pointer_v1_axis_source(
              state.layer[coord_data.output_idx]->virt_pointer,
              WL_POINTER_AXIS_SOURCE_CONTINUOUS);
            break;
        }
      }

      if (horiz_scroll_present) {
        double horiz_scroll_value = libinput_event_pointer_get_scroll_value(
          pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
        if (horiz_scroll_value == 0) {
          zwlr_virtual_pointer_v1_axis_stop(
            state.layer[coord_data.output_idx]->virt_pointer,
            ts_milliseconds, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
        } else {
          zwlr_virtual_pointer_v1_axis(
            state.layer[coord_data.output_idx]->virt_pointer,
            ts_milliseconds, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
            wl_fixed_from_double(horiz_scroll_value));
        }
        switch (ev_type) {
          case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
            zwlr_virtual_pointer_v1_axis_source(
              state.layer[coord_data.output_idx]->virt_pointer,
              WL_POINTER_AXIS_SOURCE_WHEEL);
            break;
          case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
            zwlr_virtual_pointer_v1_axis_source(
              state.layer[coord_data.output_idx]->virt_pointer,
              WL_POINTER_AXIS_SOURCE_FINGER);
            break;
          case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
            zwlr_virtual_pointer_v1_axis_source(
              state.layer[coord_data.output_idx]->virt_pointer,
              WL_POINTER_AXIS_SOURCE_CONTINUOUS);
            break;
        }
      }
    }

  } else if (ev_type == LIBINPUT_EVENT_KEYBOARD_KEY) {
    if (state.virt_kb_keymap_set) {
      struct libinput_event_keyboard *kb_event
        = libinput_event_get_keyboard_event(li_event);
      uint32_t key = libinput_event_keyboard_get_key(kb_event);
      enum libinput_key_state key_state
        = libinput_event_keyboard_get_key_state(kb_event);
      xkb_mod_mask_t depressed_mods = xkb_state_serialize_mods(
        state.xkb_state, XKB_STATE_MODS_DEPRESSED);
      xkb_mod_mask_t latched_mods = xkb_state_serialize_mods(
        state.xkb_state, XKB_STATE_MODS_LATCHED);
      xkb_mod_mask_t locked_mods = xkb_state_serialize_mods(
        state.xkb_state, XKB_STATE_MODS_LOCKED);
      xkb_layout_index_t effective_group = xkb_state_serialize_layout(
        state.xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
      zwp_virtual_keyboard_v1_modifiers(state.virt_kb, depressed_mods,
        latched_mods, locked_mods, effective_group);
      zwp_virtual_keyboard_v1_key(state.virt_kb, ts_milliseconds, key,
        key_state);
      if (key_state == LIBINPUT_KEY_STATE_PRESSED) {
        /* XKB keycodes == evdev keycodes + 8. Why this design decision was
         * made, I have no idea. */
        xkb_state_update_key(state.xkb_state, key + 8, XKB_KEY_DOWN);
      } else {
        xkb_state_update_key(state.xkb_state, key + 8, XKB_KEY_UP);
      }
    }
  }

  if (mouse_event_handled) {
    zwlr_virtual_pointer_v1_frame(
      state.layer[coord_data.output_idx]->virt_pointer);
  }
  libinput_event_destroy(li_event);
}

/****************************/
/* initialization functions */
/****************************/

static void applayer_wayland_init() {
  /* Technically we also initialize xkbcommon in here but it's only involved
   * because it turned out to be important for sending key events to
   * Wayland. */
  state.display = wl_display_connect(NULL);
  if (!state.display) {
    fprintf(stderr, "FATAL ERROR: Could not get Wayland display!");
    exit(1);
  }
  state.display_fd = wl_display_get_fd(state.display);

  state.registry = wl_display_get_registry(state.display);
  if (!state.registry) {
    fprintf(stderr, "FATAL ERROR: Could not get Wayland registry!");
    exit(1);
  }
  wl_registry_add_listener(state.registry, &registry_listener, &state);
  wl_display_roundtrip(state.display);

  /* At this point, the shm, compositor, and wm_base objects will be
   * allocated by registry global handler. */

  state.virt_kb = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
    state.virt_kb_manager, state.seat);
  /* The virtual-keyboard-v1 protocol returns 0 when making a new virtual
   * keyboard if kloak is unauthorized to create a virtual keyboard. However,
   * the protocol treats this as an enum value, meaning... we have to compare
   * a pointer to an enum. This is horrible and the protocol really shouldn't
   * require this, but it does, so... */
  if ((uint64_t)state.virt_kb
    == ZWP_VIRTUAL_KEYBOARD_MANAGER_V1_ERROR_UNAUTHORIZED) {
    fprintf(stderr,
      "Not authorized to create a virtual keyboard! Bailing out.");
    exit(1);
  }
  wl_seat_add_listener(state.seat, &seat_listener, &state);

  state.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!state.xkb_ctx) {
    fprintf(stderr, "FATAL ERROR: Could not create XKB context!");
    exit(1);
  }
}

static void applayer_libinput_init() {
  li = libinput_path_create_context(&li_interface, NULL);
  DIR *dev_input_dir = opendir("/dev/input");
  if (dev_input_dir == NULL) {
    fprintf(stderr, "FATAL ERROR: Could not open directory /dev/input: %s\n",
      strerror(errno));
    exit(1);
  }
  struct dirent *dev_input_entry;

  while ((dev_input_entry = readdir(dev_input_dir)) != NULL) {
    if (dev_input_entry->d_type != DT_CHR)
      continue;
    /* eventX = 6 characters */
    if (strlen(dev_input_entry->d_name) < 6)
      continue;
    if (strncmp(dev_input_entry->d_name, "event", 5) != 0)
      continue;
    char *dev_path = malloc(11 + strlen(dev_input_entry->d_name) + 1
      * sizeof(char));
    char *dev_path_post = stpcpy(dev_path, "/dev/input/");
    strcpy(dev_path_post, dev_input_entry->d_name);
    struct libinput_device *li_dev = libinput_path_add_device(li, dev_path);
    if (li_dev) {
      int32_t can_tap = libinput_device_config_tap_get_finger_count(li_dev);
      if (can_tap) {
        libinput_device_config_tap_set_enabled(li_dev,
          LIBINPUT_CONFIG_TAP_ENABLED);
      }
    }
    free(dev_path);
  }
  closedir(dev_input_dir);
}

static void applayer_poll_init() {
  ev_fds = calloc(2, sizeof(struct pollfd));
  ev_fds[0].fd = state.display_fd;
  ev_fds[0].events = POLLIN;
  ev_fds[1].fd = libinput_get_fd(li);
  ev_fds[1].events = POLLIN;
}

/**********/
/**********/
/** MAIN **/
/**********/
/**********/

int main(int argc, char **argv) {
  applayer_wayland_init();
  applayer_libinput_init();
  applayer_poll_init();

  for (;;) {
    while (wl_display_prepare_read(state.display) != 0)
      wl_display_dispatch_pending(state.display);
    wl_display_flush(state.display);

    for (;;) {
      enum libinput_event_type next_ev_type = libinput_next_event_type(li);
      if (next_ev_type == LIBINPUT_EVENT_NONE)
        break;
      handle_libinput_event(next_ev_type);
    }

    for (size_t i = 0; i < MAX_DRAWABLE_LAYERS; ++i) {
      if (!state.layer[i])
        continue;
      if (state.layer[i]->frame_pending)
        draw_frame(state.layer[i]);
    }
    wl_display_flush(state.display);

    poll(ev_fds, 2, -1);

    if (ev_fds[0].revents & POLLIN) {
      wl_display_read_events(state.display);
      wl_display_dispatch_pending(state.display);
    } else {
      wl_display_cancel_read(state.display);
    }
    ev_fds[0].revents = 0;

    if (ev_fds[1].revents & POLLIN) {
      libinput_dispatch(li);
    }
    ev_fds[1].revents = 0;
  }

  wl_display_disconnect(state.display);
  return 0;
}
