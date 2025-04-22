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

#include <wayland-client.h>
#include "wlr-layer-shell.h"

#include <libinput.h>

#include "kloak.h"

/********************/
/* global variables */
/********************/

int crosshair_x = 0;
int crosshair_y = 0;

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

static void panic(const char *errmsg, int err_no) {
  printf("FATAL ERROR: %s: %s\n", errmsg, strerror(-err_no));
  exit(1);
}

static void randname(char *buf, size_t len) {
  int randfd = open("/dev/urandom", O_RDONLY);
  if (randfd < 0)
    panic("Could not open /dev/urandom", errno);

  char randchar = 0;
  for (size_t i = 0; i < len; ++i) {
    do {
      if (read(randfd, &randchar, 1) < 1)
        panic("Could not read byte from /dev/urandom", errno);
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

  if (close(randfd) == -1)
    panic("Could not close /dev/urandom", errno);
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

  if (fd == -1)
    panic("Could not create shared memory fd", -EAGAIN);

  int ret;
  do {
    ret = ftruncate(fd, size);
  } while (ret < 0 && errno == EINTR);
  if (ret < 0) {
    close(fd);
    panic("Could not allocate shared memory block", errno);
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
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 2);
  }else if (strcmp(interface, wl_output_interface.name) == 0) {
    state->output = wl_registry_bind(registry, name, &wl_output_interface, 4);
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    state->layer_shell = wl_registry_bind(registry, name,
      &zwlr_layer_shell_v1_interface, 4);
  }
}

static void registry_handle_global_remove(void *data,
  struct wl_registry *registry, uint32_t name) {
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
  state->layer.size = width * state->layer.stride * height;;
  int shm_fd = create_shm_file(state->layer.size);
  if (shm_fd == -1)
    panic("Cannot allocate shared memory block for frame", errno);
  state->layer.pixbuf = mmap(NULL, state->layer.size, PROT_READ | PROT_WRITE,
    MAP_SHARED, shm_fd, 0);
  if (state->layer.pixbuf == MAP_FAILED) {
    close(shm_fd);
    panic("Failed to map shared memory block for frame", errno);
  }
  state->layer.shm_pool = wl_shm_create_pool(state->shm, shm_fd,
    state->layer.size);

  zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
  state->layer.layer_surface_configured = true;
  draw_frame(state);
}

/*********************/
/* libinput handling */
/*********************/

static int li_open_restricted(const char *path, int flags, void *user_data) {
  int fd = open(path, flags);
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
      if (x == crosshair_x)
        state->layer.pixbuf[y * state->layer.width + x] = 0xffff0000;
      else if (y == crosshair_y)
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
  layer->surface = wl_compositor_create_surface(state->compositor);
  if (!layer->surface)
    panic("Could not create Wayland surface", errno);
  layer->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
    state->layer_shell, layer->surface, state->output,
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
}

static void update_virtual_cursor(enum libinput_event_type ev_type) {
  struct libinput_event *li_event = libinput_get_event(li);
  if (ev_type == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE) {
    struct libinput_event_pointer *pointer_event
      = libinput_event_get_pointer_event(li_event);
    double abs_x = libinput_event_pointer_get_absolute_x_transformed(
      pointer_event, state.layer.width);
    double abs_y = libinput_event_pointer_get_absolute_y_transformed(
      pointer_event, state.layer.height);
    crosshair_x = (int) abs_x;
    crosshair_y = (int) abs_y;
    frame_pending = true;
  }
  libinput_event_destroy(li_event);
}

/****************************/
/* initialization functions */
/****************************/

static void applayer_wayland_init() {
  state.display = wl_display_connect(NULL);
  if (!state.display)
    panic("Could not get Wayland display", errno);
  state.display_fd = wl_display_get_fd(state.display);

  state.registry = wl_display_get_registry(state.display);
  if (!state.registry)
    panic("Could not get Wayland registry", errno);
  wl_registry_add_listener(state.registry, &registry_listener, &state);
  wl_display_roundtrip(state.display);

  /* At this point, the shm, compositor, and wm_base objects will be
   * allocated by registry global handler. */

  allocate_drawable_layer(&state, &state.layer);
}

static void applayer_libinput_init() {
  li = libinput_path_create_context(&li_interface, NULL);
  DIR *dev_input_dir = opendir("/dev/input");
  if (dev_input_dir == NULL)
    panic("Could not open directory /dev/input", errno);
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
      update_virtual_cursor(next_ev_type);
    }

    if (frame_pending)
      draw_frame(&state);

    poll(ev_fds, li_fd_count + 1, -1);

    if (ev_fds[0].revents & POLLIN) {
      wl_display_read_events(state.display);
      wl_display_dispatch_pending(state.display);
    } else {
      wl_display_cancel_read(state.display);
    }

    for (int i = 0; i < li_fd_count; ++i) {
      if (ev_fds[i+1].revents & POLLIN) {
        libinput_dispatch(li);
        break;
      }
    }
  }

  wl_display_disconnect(state.display);
  return 0;
}
