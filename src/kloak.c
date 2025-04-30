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

#include <wayland-client.h>
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

double crosshair_x = 0;
double crosshair_y = 0;

bool frame_released = true;
bool frame_pending = true;

struct disp_state state = { 0 };
struct libinput *li;

int li_fds[16];
int li_fd_count = 0;
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

static int create_shm_file(size_t size)
{
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
    state->output = wl_registry_bind(registry, name, &wl_output_interface, 4);
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
  ;
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
    printf("%d\n", strcmp(state->old_kb_map_shm, kb_map_shm));
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
    free(state->xkb_keymap);
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
    free(state->xkb_state);
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
  wl_buffer_destroy(buffer);
  frame_released = true;
}

static void layer_surface_configure(void *data,
  struct zwlr_layer_surface_v1 *layer_surface, uint32_t serial, uint32_t width,
  uint32_t height) {
  struct disp_state *state = data;
  state->layer.width = width;
  state->layer.height = height;
  state->layer.stride = width * 4;
  state->layer.size = state->layer.stride * (size_t) height;
  int shm_fd = create_shm_file(state->layer.size);
  if (shm_fd == -1) {
    fprintf(stderr,
      "FATAL ERROR: Cannot allocate shared memory block for frame: %s\n",
      strerror(errno));
    exit(1);
  }
  state->layer.pixbuf = mmap(NULL, state->layer.size, PROT_READ | PROT_WRITE,
    MAP_SHARED, shm_fd, 0);
  if (state->layer.pixbuf == MAP_FAILED) {
    close(shm_fd);
    fprintf(stderr,
      "FATAL ERROR: Failed to map shared memory block for frame: %s\n",
      strerror(errno));
    exit(1);
  }
  state->layer.shm_pool = wl_shm_create_pool(state->shm, shm_fd,
    state->layer.size);

  struct wl_region *zeroed_region = wl_compositor_create_region(state->compositor);
  wl_region_add(zeroed_region, 0, 0, 0, 0);
  wl_surface_set_input_region(state->layer.surface, zeroed_region);

  zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
  state->layer.layer_surface_configured = true;
  draw_frame(state);
}

/*********************/
/* libinput handling */
/*********************/

static int li_open_restricted(const char *path, int flags, void *user_data) {
  int fd = open(path, flags);
  struct libevdev *evdev_dev;
  int err = libevdev_new_from_fd(fd, &evdev_dev);
  if (err != 0) {
    fprintf(stderr, "FATAL ERROR: Could not create evdev for input device '%s'!\n", path);
    exit(1);
  }
  const char *dev_name = libevdev_get_name(evdev_dev);
  int one = 1;
  if (ioctl(fd, EVIOCGRAB, &one) < 0) {
    fprintf(stderr, "FATAL ERROR: Could not grab evdev device '%s'!\n", path);
    exit(1);
  }
  li_fds[li_fd_count] = fd;
  ++li_fd_count;
  return fd < 0 ? -errno : fd;
}

static void li_close_restricted(int fd, void *user_data) {
  close(fd);
}

/************************/
/* high-level functions */
/************************/

static void draw_frame(struct disp_state *state) {
  if (!frame_released)
    return;
  if (!state->layer.layer_surface_configured)
    return;
  frame_pending = false;

  struct wl_buffer *buffer = wl_shm_pool_create_buffer(state->layer.shm_pool,
    0, state->layer.width,
    state->layer.height, state->layer.stride, WL_SHM_FORMAT_ARGB8888);

  /* Draw a red line in the middle of the buffer */
  for (int y = 0; y < state->layer.height; ++y) {
    for (int x = 0; x < state->layer.width; ++x) {
      if (x == (int) crosshair_x)
        state->layer.pixbuf[y * state->layer.width + x] = 0xffff0000;
      else if (y == (int) crosshair_y)
        state->layer.pixbuf[y * state->layer.width + x] = 0xffff0000;
      else
        state->layer.pixbuf[y * state->layer.width + x] = 0x00000000;
    }
  }

  wl_buffer_add_listener(buffer, &buffer_listener, NULL);
  wl_surface_attach(state->layer.surface, buffer, 0, 0);
  wl_surface_damage_buffer(state->layer.surface, 0, 0, INT32_MAX, INT32_MAX);
  wl_surface_commit(state->layer.surface);
  frame_released = false;
}

/*static void allocate_drawable_layer(struct disp_state *state,
  struct drawable_layer *layer, int layer_width, int layer_height) {*/
static void allocate_drawable_layer(struct disp_state *state,
  struct drawable_layer *layer) {
  layer->output = state->output;
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

static void handle_libinput_event(enum libinput_event_type ev_type) {
  bool mouse_event_handled = false;

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  unsigned int ts_milliseconds = ts.tv_sec * 1000;
  ts_milliseconds += (ts.tv_nsec / 1000000);

  struct libinput_event *li_event = libinput_get_event(li);
  if (ev_type == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE) {
    mouse_event_handled = true;
    struct libinput_event_pointer *pointer_event
      = libinput_event_get_pointer_event(li_event);
    double abs_x = libinput_event_pointer_get_absolute_x_transformed(
      pointer_event, state.layer.width);
    double abs_y = libinput_event_pointer_get_absolute_y_transformed(
      pointer_event, state.layer.height);
    crosshair_x = abs_x;
    crosshair_y = abs_y;
    frame_pending = true;
    zwlr_virtual_pointer_v1_motion_absolute(state.layer.virt_pointer,
      ts_milliseconds, (unsigned int) crosshair_x, (unsigned int) crosshair_y,
      state.layer.width, state.layer.height);

  } else if (ev_type == LIBINPUT_EVENT_POINTER_MOTION) {
    mouse_event_handled = true;
    struct libinput_event_pointer *pointer_event
      = libinput_event_get_pointer_event(li_event);
    double rel_x = libinput_event_pointer_get_dx(pointer_event);
    double rel_y = libinput_event_pointer_get_dy(pointer_event);
    crosshair_x += rel_x;
    crosshair_y += rel_y;
    if (crosshair_x < 0) crosshair_x = 0;
    if (crosshair_y < 0) crosshair_y = 0;
    if (crosshair_x > state.layer.width) crosshair_x = state.layer.width;
    if (crosshair_y > state.layer.height) crosshair_y = state.layer.height;
    frame_pending = true;
    zwlr_virtual_pointer_v1_motion_absolute(state.layer.virt_pointer,
      ts_milliseconds, (unsigned int) crosshair_x, (unsigned int) crosshair_y,
      state.layer.width, state.layer.height);

  } else if (ev_type == LIBINPUT_EVENT_POINTER_BUTTON) {
    mouse_event_handled = true;
    struct libinput_event_pointer *pointer_event
      = libinput_event_get_pointer_event(li_event);
    uint32_t button_code = libinput_event_pointer_get_button(pointer_event);
    enum libinput_button_state button_state
      = libinput_event_pointer_get_button_state(pointer_event);
    if (button_state == LIBINPUT_BUTTON_STATE_PRESSED) {
      /* Both libinput and zwlr_virtual_pointer_v1 use evdev devent codes to
       * identify the button pressed, so we can just pass the data from
       * libinput straight through */
      zwlr_virtual_pointer_v1_button(state.layer.virt_pointer,
        ts_milliseconds, button_code, WL_POINTER_BUTTON_STATE_PRESSED);
    } else {
      zwlr_virtual_pointer_v1_button(state.layer.virt_pointer,
        ts_milliseconds, button_code, WL_POINTER_BUTTON_STATE_RELEASED);
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
    if (vert_scroll_present) {
      double vert_scroll_value = libinput_event_pointer_get_scroll_value(
        pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
      if (vert_scroll_value == 0) {
        zwlr_virtual_pointer_v1_axis_stop(state.layer.virt_pointer,
          ts_milliseconds, WL_POINTER_AXIS_VERTICAL_SCROLL);
      } else {
        zwlr_virtual_pointer_v1_axis(state.layer.virt_pointer,
          ts_milliseconds, WL_POINTER_AXIS_VERTICAL_SCROLL,
          wl_fixed_from_double(vert_scroll_value));
      }
      switch (ev_type) {
        case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
          zwlr_virtual_pointer_v1_axis_source(state.layer.virt_pointer,
            WL_POINTER_AXIS_SOURCE_WHEEL);
          break;
        case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
          zwlr_virtual_pointer_v1_axis_source(state.layer.virt_pointer,
            WL_POINTER_AXIS_SOURCE_FINGER);
          break;
        case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
          zwlr_virtual_pointer_v1_axis_source(state.layer.virt_pointer,
            WL_POINTER_AXIS_SOURCE_CONTINUOUS);
          break;
      }
    }

    if (horiz_scroll_present) {
      double horiz_scroll_value = libinput_event_pointer_get_scroll_value(
        pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
      if (horiz_scroll_value == 0) {
        zwlr_virtual_pointer_v1_axis_stop(state.layer.virt_pointer,
          ts_milliseconds, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
      } else {
        zwlr_virtual_pointer_v1_axis(state.layer.virt_pointer,
          ts_milliseconds, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
          wl_fixed_from_double(horiz_scroll_value));
      }
      switch (ev_type) {
        case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
          zwlr_virtual_pointer_v1_axis_source(state.layer.virt_pointer,
            WL_POINTER_AXIS_SOURCE_WHEEL);
          break;
        case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
          zwlr_virtual_pointer_v1_axis_source(state.layer.virt_pointer,
            WL_POINTER_AXIS_SOURCE_FINGER);
          break;
        case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
          zwlr_virtual_pointer_v1_axis_source(state.layer.virt_pointer,
            WL_POINTER_AXIS_SOURCE_CONTINUOUS);
          break;
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
    zwlr_virtual_pointer_v1_frame(state.layer.virt_pointer);
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
  allocate_drawable_layer(&state, &state.layer);
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
    libinput_path_add_device(li, dev_path);
    free(dev_path);
  }
}

static void applayer_poll_init() {
  ev_fds = calloc(1 + li_fd_count, sizeof(struct pollfd));
  ev_fds[0].fd = state.display_fd;
  ev_fds[0].events = POLLIN;
  for (int i = 0; i < li_fd_count; ++i) {
    ev_fds[i+1].fd = li_fds[i];
    ev_fds[i+1].events = POLLIN;
  }
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

    if (frame_pending)
      draw_frame(&state);
    wl_display_flush(state.display);

    poll(ev_fds, li_fd_count + 1, -1);

    if (ev_fds[0].revents & POLLIN) {
      wl_display_read_events(state.display);
      wl_display_dispatch_pending(state.display);
    } else {
      wl_display_cancel_read(state.display);
    }
    ev_fds[0].revents = 0;

    for (int i = 0; i < li_fd_count; ++i) {
      if (ev_fds[i+1].revents & POLLIN) {
        libinput_dispatch(li);
        break;
      }
      ev_fds[i+1].revents = 0;
    }
  }

  wl_display_disconnect(state.display);
  return 0;
}
