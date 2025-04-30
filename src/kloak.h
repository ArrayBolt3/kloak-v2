/*
 * Copyright (c) 2025 - 2025 ENCRYPTED SUPPORT LLC <adrelanos@whonix.org>
 * See the file COPYING for copying conditions.
 */

#include <stdint.h>
#include <wayland-client.h>
#include <libinput.h>

/*******************/
/* core structures */
/*******************/

struct drawable_layer {
  struct wl_output *output;
  size_t width;
  size_t height;
  size_t stride;
  size_t size;
  uint32_t *pixbuf;
  struct wl_surface *surface;
  struct wl_shm_pool *shm_pool;
  /* Layer shell stuff */
  struct zwlr_layer_surface_v1 *layer_surface;
  bool layer_surface_configured;
  /* Virtual pointer */
  struct zwlr_virtual_pointer_v1 *virt_pointer;
};

struct disp_state {
  /* miscellaneous guts */
  struct wl_display *display;
  int display_fd;
  struct wl_registry *registry;
  struct wl_shm *shm;
  struct wl_compositor *compositor;
  struct wl_seat *seat;
  const char *seat_name;
  uint32_t seat_caps;
  bool seat_set;
  struct wl_keyboard *kb;
  /* TODO: Handle multiple of these, also handle hotplug */
  struct wl_output *output;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct zwlr_virtual_pointer_manager_v1 *virt_pointer_manager;
  struct zwp_virtual_keyboard_manager_v1 *virt_kb_manager;
  struct zwp_virtual_keyboard_v1 *virt_kb;
  bool virt_kb_keymap_set;
  struct xkb_context *xkb_ctx;
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;
  char *old_kb_map_shm;
  uint32_t old_kb_map_shm_size;

  /* window and buffer properties */
  struct drawable_layer layer;
};

/*********************/
/* utility functions */
/*********************/

static void panic(const char *, int);
static void randname(char *, size_t);
static int create_shm_file(size_t);

/********************/
/* wayland handling */
/********************/

static void registry_handle_global(void *, struct wl_registry *, uint32_t,
  const char *, uint32_t);
static void registry_handle_global_remove(void *, struct wl_registry *,
  uint32_t);
static void seat_handle_name(void *, struct wl_seat *, const char *);
static void seat_handle_capabilities(void *, struct wl_seat *, uint32_t);
static void kb_handle_keymap(void *, struct wl_keyboard *, uint32_t, int32_t,
  uint32_t);
static void kb_handle_enter(void *, struct wl_keyboard *, uint32_t,
  struct wl_surface *, struct wl_array *);
static void kb_handle_leave(void *, struct wl_keyboard *, uint32_t,
  struct wl_surface *);
static void kb_handle_key(void *, struct wl_keyboard *, uint32_t, uint32_t,
  uint32_t, uint32_t);
static void kb_handle_modifiers(void *, struct wl_keyboard *, uint32_t,
  uint32_t, uint32_t, uint32_t, uint32_t);
static void kb_handle_repeat_info(void *, struct wl_keyboard *, int32_t,
  int32_t);
static void wl_buffer_release(void *, struct wl_buffer *);
static void layer_surface_configure(void *, struct zwlr_layer_surface_v1 *,
  uint32_t, uint32_t, uint32_t);

/*********************/
/* libinput handling */
/*********************/

static int li_open_restricted(const char *path, int, void *);
static void li_close_restricted(int, void *);

/************************/
/* high-level functions */
/************************/

static void draw_frame(struct disp_state *);
/*static void allocate_drawable_layer(struct disp_state *,
  struct drawable_layer *, int, int);*/
static void allocate_drawable_layer(struct disp_state *,
  struct drawable_layer *);
static void handle_libinput_event(enum libinput_event_type);

/****************************/
/* initialization functions */
/****************************/

static void applayer_wayland_init();
static void applayer_libinput_init();
static void applayer_poll_init();

/*********************/
/* wayland callbacks */
/*********************/

static const struct wl_registry_listener registry_listener = {
  .global = registry_handle_global,
  .global_remove = registry_handle_global_remove,
};
static const struct wl_seat_listener seat_listener = {
  .name = seat_handle_name,
  .capabilities = seat_handle_capabilities,
};
static const struct wl_keyboard_listener kb_listener = {
  .keymap = kb_handle_keymap,
  .enter = kb_handle_enter,
  .leave = kb_handle_leave,
  .key = kb_handle_key,
  .modifiers = kb_handle_modifiers,
  .repeat_info = kb_handle_repeat_info,
};
static const struct wl_buffer_listener buffer_listener = {
  .release = wl_buffer_release,
};
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
  .configure = layer_surface_configure,
};

/**********************/
/* libinput callbacks */
/**********************/

static const struct libinput_interface li_interface = {
  .open_restricted = li_open_restricted,
  .close_restricted = li_close_restricted,
};
