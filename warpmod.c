/*
 * warpmod - keyboard-driven pointer with real modifier holds (prototype).
 *
 * Why: warpd's Wayland backend cannot hold a modifier for an application. It
 * grabs the keyboard through a layer surface, so the focused application
 * receives no key events, and a modifier pressed through the virtual keyboard
 * protocol goes to warpd itself. The workaround - handing keyboard focus to the
 * application around every pointer action - is fragile, because the application
 * holds the keyboard for the duration of each action and can swallow a key
 * release meant for warpd.
 *
 * This prototype takes keyboard input at kernel level instead:
 *
 *   input   grab /dev/input/event* (EVIOCGRAB) for the session. The compositor
 *           never sees those keys, and unlike a layer-surface grab the
 *           compositor's own focus does not change: the window under the
 *           pointer keeps the keyboard, so it keeps its modifier state.
 *   keys    press modifiers on a uinput virtual keyboard. That changes the
 *           seat's modifier state, so the focused application learns about it
 *           from its own wl_keyboard and sees ctrl+wheel as a normal modifier.
 *   pointer motion through zwlr_virtual_pointer (absolute, so the position is
 *           known exactly); buttons and wheel through a uinput virtual mouse,
 *           which gives kernel timestamps and hi-res wheel resolution.
 *
 * Modifier semantics follow ZMK: each modifier key is a key toggle (&kt), held
 * until pressed again, and super is lazy (ZMK's `lazy` sticky-key option) so
 * arming it alone does not open the compositor overview.
 *
 * Config: ~/.config/warpd/config, same syntax as warpd.
 *
 * Usage: warpmod [--config PATH]
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <stdarg.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-pointer.h"
#include "layer-shell.h"
#include "xdg-output.h"

#define MAX_DEVICES 16
#define MAX_SCREENS 8

#define WARPMOD_VERSION "0.4"

#define MOD_CTRL  1u
#define MOD_SHIFT 2u
#define MOD_ALT   4u
#define MOD_SUPER 8u

#define KEY_LEFTCTRL  29
#define KEY_LEFTSHIFT 42
#define KEY_LEFTALT   56
#define KEY_LEFTMETA  125

/* ------------------------------------------------------------------ config */

struct config {
	char *up, *down, *left, *right;
	char *exit_key, *buttons, *drag, *decelerator, *accelerator;
	char *scroll_up, *scroll_down, *scroll_left, *scroll_right;
	char *mod_ctrl, *mod_shift, *mod_alt, *mod_super;

	int speed, max_speed, acceleration, decelerator_speed;
	int scroll_speed, scroll_acceleration;
	int super_lazy, debug, grab_virtual, check_config, probe_once;
};

static struct config cfg = {
	.up = "k Up", .down = "j Down", .left = "h Left", .right = "l Right",
	.exit_key = "esc",
	.buttons = "m , .",
	.drag = "v", .decelerator = "d", .accelerator = "a",
	.scroll_up = "r", .scroll_down = "e",
	.scroll_left = "b", .scroll_right = ",",
	.mod_ctrl = "x", .mod_shift = "v",
	.mod_alt = "c", .mod_super = "z",
	.speed = 220, .max_speed = 1600, .acceleration = 700,
	.decelerator_speed = 50,
	.scroll_speed = 600, .scroll_acceleration = 0,
	.super_lazy = 1,
};

/* -------------------------------------------------------- key name lookup */

static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_km;

/*
 * Names warpd accepts that are not keysym names. Without this, bindings such as
 * "exit: esc", "scroll_up: ," and "end: $" silently never match.
 */
static const struct {
	const char *name;
	const char *keysym;
} key_aliases[] = {
	{ "esc", "Escape" },
	{ ",", "comma" },
	{ ".", "period" },
	{ "-", "minus" },
	{ "/", "slash" },
	{ ";", "semicolon" },
	{ "$", "dollar" },
	{ "backspace", "BackSpace" },
};

static const char *keysym_name(const char *name)
{
	for (size_t i = 0; i < sizeof key_aliases / sizeof key_aliases[0]; i++)
		if (!strcmp(key_aliases[i].name, name))
			return key_aliases[i].keysym;

	return name;
}

static uint32_t code_for_name(const char *name)
{
	xkb_keysym_t sym = xkb_keysym_from_name(keysym_name(name), XKB_KEYSYM_NO_FLAGS);
	xkb_keycode_t kc;

	if (sym == XKB_KEY_NoSymbol)
		return 0;

	for (kc = xkb_keymap_min_keycode(xkb_km); kc <= xkb_keymap_max_keycode(xkb_km); kc++) {
		const xkb_keysym_t *syms;

		if (xkb_keymap_key_get_syms_by_level(xkb_km, kc, 0, 0, &syms) > 0 &&
		    syms[0] == sym)
			return kc - 8;
	}

	return 0;
}

/* Parse "C-x", "S-1", a bare keysym name, or "#58" into code + modifiers. */
static int parse_token(const char *token, uint32_t *code, uint8_t *mods)
{
	*mods = 0;

	while (token[0] && token[1] == '-' && token[2]) {
		switch (token[0]) {
		case 'C': *mods |= MOD_CTRL; break;
		case 'S': *mods |= MOD_SHIFT; break;
		case 'A': *mods |= MOD_ALT; break;
		case 'M': *mods |= MOD_SUPER; break;
		default: return 0;
		}
		token += 2;
	}

	if (token[0] == '#') {
		*code = (uint32_t)atoi(token + 1);
		return *code != 0;
	}

	*code = code_for_name(token);
	return *code != 0;
}

/* Does a space-separated binding list contain this key with these modifiers? */
static int matches(const char *binding, uint32_t code, uint8_t mods)
{
	char buf[256];
	char *tok, *save = NULL;

	if (!binding)
		return 0;

	snprintf(buf, sizeof buf, "%s", binding);

	for (tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
		uint32_t want_code;
		uint8_t want_mods;

		if (!parse_token(tok, &want_code, &want_mods))
			continue;
		if (want_code != code)
			continue;
		/*
		 * A binding without a modifier prefix matches whatever modifiers
		 * are held. The real modifier keys are mirrored to the
		 * application, so holding ctrl to scroll must not stop "m" from
		 * being recognised; a binding with a prefix still requires it.
		 */
		if (want_mods == 0 || want_mods == mods)
			return 1;
	}

	return 0;
}

/* 1-based index of a key in a button list, 0 if absent. */
static int button_index(const char *binding, uint32_t code, uint8_t mods)
{
	char buf[256];
	char *tok, *save = NULL;
	int idx = 1;

	if (!binding)
		return 0;

	snprintf(buf, sizeof buf, "%s", binding);

	for (tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save), idx++) {
		uint32_t want_code;
		uint8_t want_mods;

		if (!parse_token(tok, &want_code, &want_mods))
			continue;
		if (want_code == code && (want_mods == 0 || want_mods == mods))
			return idx;
	}

	return 0;
}

static void config_load(const char *path)
{
	FILE *fh = fopen(path, "r");
	char line[512];

	if (!fh) {
		fprintf(stderr, "warpmod: no config at %s, using defaults\n", path);
		return;
	}

	while (fgets(line, sizeof line, fh)) {
		char *colon, *key, *val;
		size_t len;

		if (line[0] == '#')
			continue;
		colon = strchr(line, ':');
		if (!colon)
			continue;

		*colon = 0;
		key = line;
		val = colon + 1;
		while (*val == ' ')
			val++;

		len = strlen(val);
		while (len && (val[len - 1] == '\n' || val[len - 1] == '\r'))
			val[--len] = 0;

#define STR_OPT(name) else if (!strcmp(key, #name)) { cfg.name = strdup(val); }
#define INT_OPT(name) else if (!strcmp(key, #name)) { cfg.name = atoi(val); }

		if (0) {}
		STR_OPT(up) STR_OPT(down) STR_OPT(left) STR_OPT(right)
		else if (!strcmp(key, "exit")) { cfg.exit_key = strdup(val); }
		STR_OPT(buttons) STR_OPT(drag)
		STR_OPT(decelerator) STR_OPT(accelerator)
		STR_OPT(scroll_up) STR_OPT(scroll_down) STR_OPT(scroll_left) STR_OPT(scroll_right)
		STR_OPT(mod_ctrl) STR_OPT(mod_shift) STR_OPT(mod_alt) STR_OPT(mod_super)
		INT_OPT(speed) INT_OPT(max_speed) INT_OPT(acceleration)
		INT_OPT(decelerator_speed) INT_OPT(scroll_speed) INT_OPT(scroll_acceleration)
		INT_OPT(super_lazy) INT_OPT(debug)
#undef STR_OPT
#undef INT_OPT
	}

	fclose(fh);
}

/* -------------------------------------------------------------- wayland io */

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_virtual_pointer_manager_v1 *ptr_mgr;
static struct zwlr_virtual_pointer_v1 *ptr;

/*
 * Position discovery, the way warpd does it: cover every output with a
 * transparent layer surface that takes no keyboard focus, then read the pointer
 * enter event. Without this the tool would have to guess where the pointer is
 * and would visibly jump on start.
 */
struct probe {
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	struct wl_buffer *buffer;
	void *data;
	size_t size;
	struct output *out;
	int done;
	double x, y;
};

static struct probe probes[1];   /* one probe surface at a time */
static int probe_hit = -1;

/*
 * An output as the compositor lays it out: the position and the size in logical
 * pixels, which is the space the absolute pointer mapping works in.
 */
struct output {
	struct wl_output *wl;
	struct zxdg_output_v1 *xdg;
	uint32_t global;         /* registry name, so it can be dropped again */
	char name[64];
	int x, y;                /* logical position in the layout */
	int w, h;                /* logical size, in layout orientation */
	int mode_w, mode_h;      /* current mode, before transform and scale */
	int scale, transform;
	int have_logical_pos;    /* xdg-output reported a position */
	int have_logical_size;   /* xdg-output reported a size */
};

static struct output outputs[MAX_SCREENS];
static size_t nr_outputs;
static struct zxdg_output_manager_v1 *xdg_mgr;

/*
 * Set whenever the compositor reports a change to an output. The mapping is
 * derived from the outputs each time it changes, not once at activation, so a
 * monitor that is moved, plugged, unplugged or rescaled while the session runs
 * cannot leave the pointer moving towards a stale layout.
 */
static int layout_changed;

static void xdg_attach(struct output *out);

static uint32_t start_ms;
static uint32_t now_ms(void);

static void phase(const char *what)
{
	if (cfg.debug)
		fprintf(stderr, "warpmod: %-14s %u ms\n", what, now_ms() - start_ms);
}

static uint32_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int output_rotated(const struct output *out)
{
	return out->transform == WL_OUTPUT_TRANSFORM_90 ||
	       out->transform == WL_OUTPUT_TRANSFORM_270 ||
	       out->transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
	       out->transform == WL_OUTPUT_TRANSFORM_FLIPPED_270;
}

/*
 * The logical size is what the layout is made of, so a scaled output takes the
 * space it appears to take. xdg-output gives it directly, with rotation and
 * fractional scale already applied; without xdg-output the mode is rotated and
 * divided by the integer scale instead.
 */
static void output_update_size(struct output *out)
{
	int scale = out->scale ? out->scale : 1;

	if (out->have_logical_size)
		return;

	if (output_rotated(out)) {
		out->w = out->mode_h / scale;
		out->h = out->mode_w / scale;
	} else {
		out->w = out->mode_w / scale;
		out->h = out->mode_h / scale;
	}
}

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
			    int32_t pw, int32_t ph, int32_t subpixel, const char *make,
			    const char *model, int32_t transform)
{
	struct output *out = data;
	(void)o; (void)subpixel; (void)make; (void)model;

	(void)pw; (void)ph;   /* millimetres, not pixels */

	if (!out->have_logical_pos) {
		out->x = x;   /* xdg-output is the better source once it arrives */
		out->y = y;
	}
	if (out->transform != transform) {
		out->transform = transform;
		output_update_size(out);
	}
	layout_changed = 1;
}

static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w,
			int32_t h, int32_t refresh)
{
	struct output *out = data;
	(void)o; (void)refresh;

	if (!(flags & WL_OUTPUT_MODE_CURRENT))
		return;

	out->mode_w = w;
	out->mode_h = h;
	output_update_size(out);
	layout_changed = 1;
}

static void output_done(void *data, struct wl_output *o)
{
	(void)data; (void)o;
}

static void output_scale(void *data, struct wl_output *o, int32_t factor)
{
	struct output *out = data;
	(void)o;

	out->scale = factor ? factor : 1;
	output_update_size(out);
	layout_changed = 1;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

/*
 * xdg-output is the exact source for the position and the size in logical
 * pixels. wl_output alone gives the integer scale, which is wrong on a
 * fractionally scaled output and would make the mapping drift.
 */
static void xdg_logical_position(void *data, struct zxdg_output_v1 *xdg, int32_t x, int32_t y)
{
	struct output *out = data;
	(void)xdg;

	out->x = x;
	out->y = y;
	out->have_logical_pos = 1;
	layout_changed = 1;
}

static void xdg_logical_size(void *data, struct zxdg_output_v1 *xdg, int32_t w, int32_t h)
{
	struct output *out = data;
	(void)xdg;

	if (w <= 0 || h <= 0)
		return;

	out->w = w;
	out->h = h;
	out->have_logical_size = 1;
	layout_changed = 1;
}

static void xdg_done(void *data, struct zxdg_output_v1 *xdg)
{
	(void)data; (void)xdg;
}

static void xdg_name(void *data, struct zxdg_output_v1 *xdg, const char *name)
{
	struct output *out = data;
	(void)xdg;

	snprintf(out->name, sizeof out->name, "%s", name);
}

static void xdg_description(void *data, struct zxdg_output_v1 *xdg, const char *description)
{
	(void)data; (void)xdg; (void)description;
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_logical_position,
	.logical_size = xdg_logical_size,
	.done = xdg_done,
	.name = xdg_name,
	.description = xdg_description,
};

static void xdg_attach(struct output *out)
{
	if (!xdg_mgr || out->xdg || !out->wl)
		return;

	out->xdg = zxdg_output_manager_v1_get_xdg_output(xdg_mgr, out->wl);
	zxdg_output_v1_add_listener(out->xdg, &xdg_output_listener, out);
}

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
		       const char *iface, uint32_t version)
{
	(void)data;

	if (!strcmp(iface, "zwlr_virtual_pointer_manager_v1")) {
		ptr_mgr = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface,
					   version < 2 ? version : 2);
	} else if (!strcmp(iface, "wl_compositor")) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (!strcmp(iface, "wl_shm")) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, "wl_seat")) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
	} else if (!strcmp(iface, "zwlr_layer_shell_v1")) {
		layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 2);
	} else if (!strcmp(iface, "zxdg_output_manager_v1")) {
		xdg_mgr = wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface,
					   version < 3 ? version : 3);
		/* outputs are usually announced before the manager */
		for (size_t i = 0; i < nr_outputs; i++)
			xdg_attach(&outputs[i]);
	} else if (!strcmp(iface, "wl_output") && nr_outputs < MAX_SCREENS) {
		struct output *out = &outputs[nr_outputs++];

		memset(out, 0, sizeof *out);
		out->global = name;
		out->scale = 1;
		out->wl = wl_registry_bind(reg, name, &wl_output_interface, 3);
		wl_output_add_listener(out->wl, &output_listener, out);
		xdg_attach(out);
		layout_changed = 1;
	}
}

static void reg_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data; (void)reg;

	for (size_t i = 0; i < nr_outputs; i++) {
		if (outputs[i].global != name)
			continue;

		if (outputs[i].xdg)
			zxdg_output_v1_destroy(outputs[i].xdg);
		wl_output_destroy(outputs[i].wl);
		outputs[i] = outputs[--nr_outputs];

		/*
		 * The moved output's proxies still point at the slot it came
		 * from, which is now unused: point them at the slot it lives in.
		 */
		wl_output_add_listener(outputs[i].wl, &output_listener, &outputs[i]);
		if (outputs[i].xdg)
			zxdg_output_v1_add_listener(outputs[i].xdg, &xdg_output_listener,
						    &outputs[i]);
		layout_changed = 1;
		return;
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = reg_global,
	.global_remove = reg_remove,
};

static int output_size(const struct output *out, int horizontal)
{
	return horizontal ? out->w : out->h;
}

static int layout_edge(int horizontal, int want_max)
{
	int best = want_max ? INT_MIN : INT_MAX;

	for (size_t i = 0; i < nr_outputs; i++) {
		int pos = horizontal ? outputs[i].x : outputs[i].y;
		int size = output_size(&outputs[i], horizontal);

		if (size <= 0)
			continue;   /* no mode yet, so it is not in the layout */

		if (want_max) {
			if (pos + size > best)
				best = pos + size;
		} else if (pos < best) {
			best = pos;
		}
	}

	if (best == INT_MAX || best == INT_MIN)
		return 0;

	return best;
}

static int layout_w(void)
{
	return layout_edge(1, 1) - layout_edge(1, 0);
}

static int layout_h(void)
{
	return layout_edge(0, 1) - layout_edge(0, 0);
}

/* --------------------------------------------------------------- uinput out */

static int ui_kbd = -1;
static int ui_mouse = -1;

static void ui_emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
	struct input_event ev = {0};

	ev.type = type;
	ev.code = code;
	ev.value = value;

	if (write(fd, &ev, sizeof ev) != (ssize_t)sizeof ev)
		perror("warpmod: uinput write");
}

static void ui_sync(int fd)
{
	ui_emit(fd, EV_SYN, SYN_REPORT, 0);
}

static int ui_create(const char *name, int keys[], int nkeys, int rels[], int nrels)
{
	struct uinput_setup setup = {0};
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

	if (fd < 0) {
		perror("warpmod: open /dev/uinput");
		return -1;
	}

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	for (int i = 0; i < nkeys; i++)
		ioctl(fd, UI_SET_KEYBIT, keys[i]);

	if (nrels) {
		ioctl(fd, UI_SET_EVBIT, EV_REL);
		for (int i = 0; i < nrels; i++)
			ioctl(fd, UI_SET_RELBIT, rels[i]);
	}

	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x1234;
	setup.id.product = nrels ? 0x0002 : 0x0001;
	snprintf(setup.name, sizeof setup.name, "%s", name);

	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("warpmod: uinput setup");
		close(fd);
		return -1;
	}

	return fd;
}

static uint32_t held_mods;

static uint32_t mod_code(uint32_t bit)
{
	switch (bit) {
	case MOD_CTRL: return KEY_LEFTCTRL;
	case MOD_SHIFT: return KEY_LEFTSHIFT;
	case MOD_ALT: return KEY_LEFTALT;
	case MOD_SUPER: return KEY_LEFTMETA;
	default: return 0;
	}
}

static void mods_press(uint32_t mask)
{
	uint32_t add = mask & ~held_mods;

	if (!add)
		return;

	for (int i = 0; i < 4; i++) {
		uint32_t bit = 1u << i;

		if (add & bit) {
			ui_emit(ui_kbd, EV_KEY, mod_code(bit), 1);
			held_mods |= bit;
		}
	}
	ui_sync(ui_kbd);
}

static void mods_release(uint32_t mask)
{
	uint32_t drop = mask & held_mods;

	if (!drop)
		return;

	for (int i = 3; i >= 0; i--) {
		uint32_t bit = 1u << i;

		if (drop & bit) {
			ui_emit(ui_kbd, EV_KEY, mod_code(bit), 0);
			held_mods &= ~bit;
		}
	}
	ui_sync(ui_kbd);
}

/* ------------------------------------------------------------------ pointer */

static int px, py;
static unsigned wheel_count, wheel_units, tick_count;
static double move_speed;
static double move_frac_x, move_frac_y;
static double scroll_v;
static int scroll_dir;
static int scroll_hdir;
static uint32_t armed;
static uint32_t phys_mods;   /* modifiers held on the real keyboard */

static void save_position(int x, int y);
static void logmsg(const char *fmt, ...);

/*
 * Absolute motion is a fraction of the compositor's layout bounding box, and
 * the compositor adds the box origin back. 0,0 here is therefore the top left
 * of the topmost/leftmost output, not the top left of the layout: without
 * subtracting the origin, every position is off by a whole monitor as soon as
 * one output sits at a negative coordinate.
 */
static void pointer_move(int x, int y)
{
	int ox = layout_edge(1, 0), oy = layout_edge(0, 0);
	int w = layout_w(), h = layout_h();

	if (w <= 0 || h <= 0)
		return;

	if (x < ox) x = ox;
	if (y < oy) y = oy;
	if (x >= ox + w) x = ox + w - 1;
	if (y >= oy + h) y = oy + h - 1;

	px = x;
	py = y;
	save_position(px, py);

	zwlr_virtual_pointer_v1_motion_absolute(ptr, now_ms(),
						wl_fixed_from_int(x - ox),
						wl_fixed_from_int(y - oy),
						wl_fixed_from_int(w), wl_fixed_from_int(h));
	zwlr_virtual_pointer_v1_frame(ptr);
	wl_display_flush(dpy);
}

/*
 * The compositor can rearrange the outputs while the session runs: a monitor is
 * plugged, unplugged, or niri applies a config with other positions. The
 * mapping follows the new layout, and the pointer stays where it is, because
 * its position is kept in layout coordinates. Only a pointer left outside the
 * new layout is pulled back into it.
 */
static void relayout(void)
{
	int ox = layout_edge(1, 0), oy = layout_edge(0, 0);
	int w = layout_w(), h = layout_h();
	int x = px, y = py;

	if (w <= 0 || h <= 0)
		return;

	if (x < ox) x = ox;
	if (y < oy) y = oy;
	if (x >= ox + w) x = ox + w - 1;
	if (y >= oy + h) y = oy + h - 1;

	logmsg("layout now %zu output(s) %dx%d at %d,%d, pointer %d,%d", nr_outputs,
	       w, h, ox, oy, x, y);

	if (cfg.debug)
		fprintf(stderr, "warpmod: layout now %zu output(s) %dx%d at %d,%d, pointer %d,%d\n",
			nr_outputs, w, h, ox, oy, x, y);

	if (x != px || y != py)
		pointer_move(x, y);
}

static int btn_code(int idx)
{
	switch (idx) {
	case 1: return BTN_LEFT;
	case 2: return BTN_MIDDLE;
	case 3: return BTN_RIGHT;
	default: return 0;
	}
}

/* Super is lazy: pressing it alone opens the compositor overview. */
static uint32_t lazy_mask(void)
{
	return cfg.super_lazy ? (armed & MOD_SUPER) : 0;
}

/*
 * Modifiers the application should see right now: whatever is held on the real
 * keyboard, plus whatever was armed with a toggle key. Super stays out of it
 * while it is only armed, and is pressed for the duration of an action.
 */
static uint32_t wanted_mods(void)
{
	return phys_mods | (armed & ~lazy_mask());
}

/* Bring the uinput keyboard in line with wanted_mods(). */
static void mods_apply(void)
{
	uint32_t want = wanted_mods();

	mods_press(want);
	mods_release(held_mods & ~want);
}

static void action_begin(void)
{
	mods_press(lazy_mask());
}

static void action_after(void)
{
	mods_release(lazy_mask() & ~phys_mods);
}

static void click(int idx)
{
	int code = btn_code(idx);

	if (!code)
		return;

	if (cfg.debug)
		fprintf(stderr, "warpmod: click button %d\n", code);

	action_begin();
	ui_emit(ui_mouse, EV_KEY, code, 1);
	ui_sync(ui_mouse);
	ui_emit(ui_mouse, EV_KEY, code, 0);
	ui_sync(ui_mouse);
	action_after();
}

/*
 * Hi-res wheel events are paced like a real device: about one event per 8 ms
 * carrying the units accumulated in that interval. Flooding tiny events makes
 * compositors coalesce or drop them, which shows up as scroll that barely moves.
 */
static void wheel_emit(double units)
{
	static double acc;
	static uint32_t last;
	uint32_t now = now_ms();
	int whole;

	acc += units;

	if (now - last < 8)
		return;
	last = now;

	whole = (int)acc;
	if (!whole)
		return;
	acc -= whole;

	action_begin();
	ui_emit(ui_mouse, EV_REL, scroll_hdir ? REL_HWHEEL_HI_RES : REL_WHEEL_HI_RES, whole);
	ui_sync(ui_mouse);
	action_after();

	wheel_count++;
	wheel_units += whole;

	if (cfg.debug)
		fprintf(stderr, "warpmod: wheel %d\n", whole);
}


/* ------------------------------------------------------------- probe surface */

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
			  struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data; (void)p; (void)serial;

	if (cfg.debug)
		fprintf(stderr, "warpmod: probe enter %f,%f on surface %p\n",
			wl_fixed_to_double(sx), wl_fixed_to_double(sy), (void *)surface);

	if (probes[0].surface == surface) {
		probes[0].x = wl_fixed_to_double(sx);
		probes[0].y = wl_fixed_to_double(sy);
		probe_hit = 0;
	}
}

static void pointer_leave(void *d, struct wl_pointer *p, uint32_t serial,
			  struct wl_surface *s)
{ (void)d; (void)p; (void)serial; (void)s; }
static void pointer_motion(void *d, struct wl_pointer *p, uint32_t t,
			   wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)p; (void)t; (void)x; (void)y; }
static void pointer_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
			   uint32_t b, uint32_t st)
{ (void)d; (void)p; (void)serial; (void)t; (void)b; (void)st; }
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{ (void)d; (void)p; (void)t; (void)a; (void)v; }
static void pointer_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void pointer_axis_source(void *d, struct wl_pointer *p, uint32_t src)
{ (void)d; (void)p; (void)src; }
static void pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{ (void)d; (void)p; (void)t; (void)a; }
static void pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t dis)
{ (void)d; (void)p; (void)a; (void)dis; }

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
			    uint32_t serial, uint32_t w, uint32_t h)
{
	struct probe *pr = data;

	(void)w; (void)h;
	if (cfg.debug)
		fprintf(stderr, "warpmod: probe configure\n");
	zwlr_layer_surface_v1_ack_configure(layer, serial);

	if (pr->buffer) {
		wl_surface_attach(pr->surface, pr->buffer, 0, 0);
		wl_surface_commit(pr->surface);
	}
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
	(void)data; (void)layer;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static struct wl_buffer *probe_buffer(struct probe *pr, int w, int h)
{
	size_t stride = (size_t)w * 4;
	struct wl_shm_pool *pool;
	int fd;

	pr->size = stride * h;
	fd = memfd_create("warpmod-probe", MFD_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (ftruncate(fd, pr->size) < 0) {
		close(fd);
		return NULL;
	}

	pr->data = mmap(NULL, pr->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pr->data == MAP_FAILED) {
		pr->data = NULL;
		close(fd);
		return NULL;
	}
	memset(pr->data, 0, pr->size);   /* fully transparent */

	pool = wl_shm_create_pool(shm, fd, pr->size);
	pr->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
					       WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	return pr->buffer;
}

/* ------------------------------------------------------------- saved position */

static const char *state_file(const char *name, char *buf, size_t sz)
{
	const char *base = getenv("XDG_STATE_HOME");
	char dir[PATH_MAX];

	if (base && *base)
		snprintf(dir, sizeof dir, "%s/warpmod", base);
	else
		snprintf(dir, sizeof dir, "%s/.local/state/warpmod",
			 getenv("HOME") ? getenv("HOME") : ".");

	mkdir(dir, 0700);

	if (snprintf(buf, sz, "%s/%s", dir, name) >= (int)sz)
		buf[0] = 0;

	return buf;
}

static const char *state_path(char *buf, size_t sz)
{
	return state_file("position", buf, sz);
}

/*
 * One session at a time. Two instances each grab their own keyboard and each
 * move the pointer towards their own idea of where it is, which looks like a
 * cursor that barely moves and goes the wrong way.
 */
static int lock_fd = -1;
static char lock_path[PATH_MAX];

static int acquire_lock(void)
{
	state_file("lock", lock_path, sizeof lock_path);
	lock_fd = open(lock_path, O_RDWR | O_CREAT, 0600);
	if (lock_fd < 0)
		return 1;   /* cannot lock: do not refuse to run over it */

	if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0)
		return 0;

	return 1;
}

static void write_pidfile(void)
{
	char path[PATH_MAX];
	FILE *f = fopen(state_file("pid", path, sizeof path), "w");

	if (f) {
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
	}
}

static void remove_pidfile(void)
{
	char path[PATH_MAX];

	unlink(state_file("pid", path, sizeof path));
}

/* A small always-on log, so a session can be inspected after the fact. */
static FILE *log_file;

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (log_file) {
		vfprintf(log_file, fmt, ap);
		fputc('\n', log_file);
		fflush(log_file);
	}
	va_end(ap);
}

static int load_position(int *x, int *y)
{
	char buf[PATH_MAX];
	FILE *f = fopen(state_path(buf, sizeof buf), "r");
	int ok = 0;

	if (f) {
		ok = fscanf(f, "%d %d", x, y) == 2;
		fclose(f);
	}

	return ok;
}

static void save_position_force(int x, int y)
{
	char buf[PATH_MAX];
	FILE *f = fopen(state_path(buf, sizeof buf), "w");

	if (f) {
		fprintf(f, "%d %d\n", x, y);
		fclose(f);
	}
}

static void save_position(int x, int y)
{
	static uint32_t last;

	if (now_ms() - last < 500)   /* not on every movement tick */
		return;
	last = now_ms();

	save_position_force(x, y);
}

/*
 * Learn where the pointer is: cover one output with a transparent layer surface
 * that takes no keyboard focus, read the pointer enter event, remove it again.
 * One output at a time, because with several such surfaces up at once the enter
 * can arrive on the wrong one and every position is then offset by a monitor.
 */
static int probe_one(struct output *out, double *lx, double *ly)
{
	struct probe *pr = &probes[0];
	int w = out->w, h = out->h;
	int ok;

	if (w <= 0 || h <= 0)
		return 0;

	memset(pr, 0, sizeof *pr);
	pr->out = out;
	pr->surface = wl_compositor_create_surface(compositor);
	pr->layer = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, pr->surface, out->wl,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "warpmod-probe");
	zwlr_layer_surface_v1_add_listener(pr->layer, &layer_listener, pr);
	zwlr_layer_surface_v1_set_size(pr->layer, w, h);
	/*
	 * Anchor to every edge and refuse to be moved by other surfaces'
	 * exclusive zones, so the surface sits at the real output origin. Without
	 * this the enter coordinates are offset by the height of a panel.
	 */
	zwlr_layer_surface_v1_set_anchor(pr->layer,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(pr->layer, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
		pr->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	probe_buffer(pr, w, h);
	wl_surface_commit(pr->surface);

	probe_hit = -1;
	wl_display_roundtrip(dpy);   /* configure */
	wl_display_flush(dpy);
	wl_display_roundtrip(dpy);   /* mapped */

	for (int i = 0; i < 8 && probe_hit < 0; i++) {
		struct pollfd pfd = {
			.fd = wl_display_get_fd(dpy),
			.events = POLLIN,
		};

		wl_display_flush(dpy);
		if (!poll(&pfd, 1, 10))
			break;
		wl_display_dispatch(dpy);
	}

	ok = probe_hit >= 0;

	if (cfg.debug)
		fprintf(stderr, "warpmod: probe attempt on output %d,%d %dx%d -> enter=%d local=%g,%g\n",
			out->x, out->y, w, h, ok, pr->x, pr->y);

	if (ok) {
		*lx = pr->x;
		*ly = pr->y;
	}

	zwlr_layer_surface_v1_destroy(pr->layer);
	wl_surface_destroy(pr->surface);
	if (pr->buffer)
		wl_buffer_destroy(pr->buffer);
	if (pr->data)
		munmap(pr->data, pr->size);
	pr->buffer = NULL;
	pr->data = NULL;
	wl_display_roundtrip(dpy);

	return ok;
}

static int probe_position(int *x, int *y)
{
	if (!compositor || !shm || !layer_shell || !seat)
		return 0;

	pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(pointer, &pointer_listener, NULL);

	for (size_t i = 0; i < nr_outputs; i++) {
		double lx = 0, ly = 0;

		if (!probe_one(&outputs[i], &lx, &ly))
			continue;

		*x = outputs[i].x + (int)llround(lx);
		*y = outputs[i].y + (int)llround(ly);

		if (cfg.debug)
			fprintf(stderr, "warpmod: probe: pointer on output %zu (%d,%d) + %g,%g\n",
				i, outputs[i].x, outputs[i].y, lx, ly);

		wl_pointer_release(pointer);
		pointer = NULL;
		return 1;
	}

	wl_pointer_release(pointer);
	pointer = NULL;
	return 0;
}

/* -------------------------------------------------------------------- input */

struct device {
	int fd;
	uint8_t mods;
	char path[64];
};

static struct device devices[MAX_DEVICES];
static size_t nr_devices;

static int is_keyboard(const char *path)
{
	uint8_t bits[(KEY_MAX + 7) / 8] = {0};
	int fd = open(path, O_RDONLY | O_NONBLOCK);
	int ret;

	if (fd < 0)
		return 0;

	ret = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits);
	close(fd);

	if (ret < 0)
		return 0;

	return bits[KEY_A / 8] & (1u << (KEY_A % 8));
}

static void device_name(const char *path, char *out, size_t outsz)
{
	int fd = open(path, O_RDONLY | O_NONBLOCK);

	out[0] = 0;
	if (fd < 0)
		return;
	ioctl(fd, EVIOCGNAME(outsz), out);
	close(fd);
}

static int is_virtual(const char *path)
{
	char link[PATH_MAX], resolved[PATH_MAX];

	snprintf(link, sizeof link, "/sys/class/input/%s/device", strrchr(path, '/') + 1);
	if (!realpath(link, resolved))
		return 0;

	return strstr(resolved, "/devices/virtual/input/") != NULL;
}

static int open_path(const char *path)
{
	uint8_t bits[(KEY_MAX + 7) / 8] = {0};
	int fd = open(path, O_RDONLY | O_NONBLOCK);

	if (fd < 0)
		return 0;

	/*
	 * Require a letter key on this same descriptor: power and sleep buttons
	 * and the video bus are keyboard-class devices too. Opening an input
	 * device costs about 20 ms here, so this must not open it twice.
	 */
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0 ||
	    !(bits[KEY_A / 8] & (1u << (KEY_A % 8)))) {
		close(fd);
		return 0;
	}

	for (size_t d = 0; d < nr_devices; d++) {
		if (!strcmp(devices[d].path, path)) {   /* already taken over */
			close(fd);
			return 0;
		}
	}

	devices[nr_devices].fd = fd;
	devices[nr_devices].mods = 0;
	snprintf(devices[nr_devices].path, sizeof devices[nr_devices].path, "%s", path);
	nr_devices++;

	if (cfg.debug) {
		char dev_name[128] = "?";

		ioctl(fd, EVIOCGNAME(sizeof dev_name), dev_name);
		fprintf(stderr, "warpmod: opened %s (%s) at %u ms\n", path, dev_name,
			now_ms() - start_ms);
	}

	return 1;
}



/*
 * Fast path: /proc/bus/input/devices names every keyboard and its event node in
 * one read. Walking /dev/input and probing every node instead costs about 20 ms
 * per device, which is most of a slow start.
 */
static int grab_from_proc(void)
{
	FILE *f = fopen("/proc/bus/input/devices", "r");
	char line[1024];
	char name[128] = "", handlers[1024] = "", sysfs[1024] = "";
	int found = 0;

	if (!f)
		return 0;

	while (fgets(line, sizeof line, f)) {
		if (line[0] == 'N' && line[1] == ':') {
			char *q = strchr(line, '"');
			char *e = q ? strrchr(q + 1, '"') : NULL;

			name[0] = 0;
			if (q && e && e > q + 1 && (size_t)(e - q - 1) < sizeof name) {
				memcpy(name, q + 1, (size_t)(e - q - 1));
				name[e - q - 1] = 0;
			}
		} else if (line[0] == 'H' && line[1] == ':') {
			snprintf(handlers, sizeof handlers, "%s", line + 2);
		} else if (line[0] == 'S' && line[1] == ':') {
			snprintf(sysfs, sizeof sysfs, "%s", line + 2);
		} else if (line[0] == '\n' || line[0] == 0) {
			/* end of a block: decide on it */
			char *ev = strstr(handlers, "event");

			if (ev && strstr(handlers, "kbd") &&
			    !strstr(sysfs, "/devices/virtual/input/") &&
			    strncmp(name, "warpmod ", 8) &&
			    nr_devices < MAX_DEVICES) {
				char path[PATH_MAX];
				int n = 0;

				if (sscanf(ev, "event%d", &n) == 1) {
					snprintf(path, sizeof path, "/dev/input/event%d", n);
					found += open_path(path);
				}
			}
			handlers[0] = sysfs[0] = name[0] = 0;
		}
	}

	fclose(f);
	return found;
}

static void open_keyboards(void)
{
	DIR *dir;
	struct dirent *ent;
	/*
	 * Test hook: grab only virtual keyboards, so a test run cannot capture
	 * the keyboard someone is typing on.
	 */
	int only_virtual = getenv("WARPMOD_TEST_ONLY_VIRTUAL") != NULL;

	/* fast path: /proc names every keyboard and its event node in one read */
	if (!only_virtual && !cfg.grab_virtual && grab_from_proc() > 0)
		return;

	dir = opendir("/dev/input");
	if (!dir) {
		perror("warpmod: /dev/input");
		return;
	}

	while ((ent = readdir(dir)) && nr_devices < MAX_DEVICES) {
		char path[PATH_MAX], name[128];

		if (strncmp(ent->d_name, "event", 5))
			continue;

		snprintf(path, sizeof path, "/dev/input/%s", ent->d_name);
		if (!is_keyboard(path))
			continue;

		device_name(path, name, sizeof name);
		if (!strncmp(name, "warpmod ", 8))
			continue;   /* never take over our own injection devices */
		if (is_virtual(path) && !cfg.grab_virtual)
			continue;
		if (only_virtual && !is_virtual(path))
			continue;

		open_path(path);
	}

	closedir(dir);

	if (!nr_devices)
		fprintf(stderr, "warpmod: no keyboard found (is the user in group input?)\n");
}

static void grab_open(void);

/* rescan path: look for keyboards again (each is grabbed as it is opened) */
static void grab_keyboards(void)
{
	open_keyboards();
	grab_open();
}



/* Take over every opened device; drop the ones that cannot be grabbed. */
static void grab_open(void)
{
	for (size_t d = 0; d < nr_devices; d++) {
		if (cfg.debug) {
			char lnk[128] = "?";

			char proc[64];

			snprintf(proc, sizeof proc, "/proc/self/fd/%d", (int)devices[d].fd);
			ssize_t n = readlink(proc, lnk, sizeof lnk - 1);

			if (n > 0)
				lnk[n] = 0;
			fprintf(stderr, "warpmod: grabbing %s fd %d -> %s\n",
				devices[d].path, (int)devices[d].fd, lnk);
		}
		if (ioctl(devices[d].fd, EVIOCGRAB, (void *)1) < 0) {
			if (cfg.debug)
				fprintf(stderr, "warpmod: cannot grab %s: %s\n",
					devices[d].path, strerror(errno));
			close(devices[d].fd);
			devices[d].fd = -1;
		}
	}

	for (size_t d = nr_devices; d > 0; d--) {
		size_t i = d - 1;

		if (devices[i].fd >= 0)
			continue;
		devices[i] = devices[--nr_devices];
	}
}

static int any_key_held(void)
{
	uint8_t keys[(KEY_MAX + 7) / 8];

	for (size_t d = 0; d < nr_devices; d++) {
		memset(keys, 0, sizeof keys);
		if (ioctl(devices[d].fd, EVIOCGKEY(sizeof keys), keys) < 0) {
			if (cfg.debug)
				fprintf(stderr, "warpmod: EVIOCGKEY %s: %s\n",
					devices[d].path, strerror(errno));
			continue;
		}

		for (size_t i = 0; i < sizeof keys; i++)
			if (keys[i])
				return 1;
	}

	return 0;
}

/*
 * The session starts with Mod+M, so Super (and M) are still down when a grab
 * would normally begin. The compositor tracks key state per device, so grabbing
 * now swallows their release and leaves Super held there for good. Wait, without
 * grabbing, until the keyboards report nothing held: that is exactly as long as
 * the user holds the chord, usually a hundred milliseconds, not a fixed delay.
 * Events seen during the wait are already reaching the compositor, so they are
 * simply drained.
 */
static void wait_until_released(int timeout_ms)
{
	struct pollfd pfds[MAX_DEVICES];
	uint32_t start = now_ms();
	uint32_t deadline = start + timeout_ms;

	while (now_ms() < deadline && any_key_held()) {
		int nfds = 0;

		for (size_t d = 0; d < nr_devices; d++) {
			pfds[nfds].fd = devices[d].fd;
			pfds[nfds].events = POLLIN;
			nfds++;
		}

		if (poll(pfds, nfds, 10) <= 0)
			continue;

		for (int i = 0; i < nfds; i++) {
			struct input_event ev;

			if (!(pfds[i].revents & POLLIN))
				continue;
			while (read(pfds[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev)
				;
		}
	}

	if (cfg.debug)
		fprintf(stderr, "warpmod: waited %u ms for the launch chord, %s\n",
			now_ms() - start, any_key_held() ? "still held" : "clear");
}

static void ungrab_keyboards(void)
{
	for (size_t i = 0; i < nr_devices; i++) {
		ioctl(devices[i].fd, EVIOCGRAB, (void *)0);
		close(devices[i].fd);
	}
	nr_devices = 0;
}

/* ------------------------------------------------------------- mode state */

static int running = 1;
static uint32_t last_rescan;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static int held_up, held_down, held_left, held_right;
static int decel, accel;
static int dragging;

static void toggle_mod(uint32_t bit)
{
	armed ^= bit;
	mods_apply();

	logmsg("armed 0x%x held 0x%x physical 0x%x at %d,%d", armed, held_mods,
	       phys_mods, px, py);

	if (cfg.debug)
		fprintf(stderr, "warpmod: armed 0x%x (held 0x%x, physical 0x%x)\n",
			armed, held_mods, phys_mods);
}

static void handle_key(struct device *dev, uint32_t code, int value)
{
	uint8_t mods = dev->mods;

	if (value == 2)   /* autorepeat: state unchanged */
		return;

	if (value == 0) {
		if (matches(cfg.up, code, mods) || matches(cfg.down, code, mods) ||
		    matches(cfg.left, code, mods) || matches(cfg.right, code, mods)) {
			if (matches(cfg.up, code, mods)) held_up = 0;
			if (matches(cfg.down, code, mods)) held_down = 0;
			if (matches(cfg.left, code, mods)) held_left = 0;
			if (matches(cfg.right, code, mods)) held_right = 0;
		}
		if (matches(cfg.scroll_up, code, mods) || matches(cfg.scroll_down, code, mods) ||
		    matches(cfg.scroll_left, code, mods) || matches(cfg.scroll_right, code, mods)) {
			scroll_dir = 0;
			scroll_hdir = 0;
			scroll_v = 0;
		}
		if (matches(cfg.decelerator, code, mods))
			decel = 0;
		if (matches(cfg.accelerator, code, mods))
			accel = 0;
		return;
	}

	if (code == KEY_ESC || matches(cfg.exit_key, code, mods)) {
		running = 0;
		return;
	}

	if (matches(cfg.up, code, mods)) held_up = 1;
	if (matches(cfg.down, code, mods)) held_down = 1;
	if (matches(cfg.left, code, mods)) held_left = 1;
	if (matches(cfg.right, code, mods)) held_right = 1;


	if (matches(cfg.decelerator, code, mods))
		decel = 1;
	if (matches(cfg.accelerator, code, mods))
		accel = 1;

	/*
	 * REL_WHEEL and REL_HWHEEL are positive upwards and rightwards, the
	 * opposite of wl_pointer.axis, so "scroll up" is the positive value.
	 */
	if (matches(cfg.scroll_up, code, mods)) {
		scroll_dir = 1;
		scroll_hdir = 0;
		scroll_v = cfg.scroll_speed;
	} else if (matches(cfg.scroll_down, code, mods)) {
		scroll_dir = -1;
		scroll_hdir = 0;
		scroll_v = cfg.scroll_speed;
	} else if (matches(cfg.scroll_left, code, mods)) {
		scroll_dir = -1;
		scroll_hdir = 1;
		scroll_v = cfg.scroll_speed * 2;
	} else if (matches(cfg.scroll_right, code, mods)) {
		scroll_dir = 1;
		scroll_hdir = 1;
		scroll_v = cfg.scroll_speed * 2;
	}

	if (matches(cfg.drag, code, mods)) {
		dragging = !dragging;
		action_begin();
		ui_emit(ui_mouse, EV_KEY, BTN_LEFT, dragging ? 1 : 0);
		ui_sync(ui_mouse);
		action_after();
	}

	if (matches(cfg.mod_ctrl, code, mods))
		toggle_mod(MOD_CTRL);
	else if (matches(cfg.mod_shift, code, mods))
		toggle_mod(MOD_SHIFT);
	else if (matches(cfg.mod_alt, code, mods))
		toggle_mod(MOD_ALT);
	else if (matches(cfg.mod_super, code, mods))
		toggle_mod(MOD_SUPER);

	if (!dragging) {
		int idx = button_index(cfg.buttons, code, mods);

		if (idx)
			click(idx);
	}
}

static uint32_t mod_bit(uint32_t code)
{
	switch (code) {
	case KEY_LEFTCTRL: case KEY_RIGHTCTRL: return MOD_CTRL;
	case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: return MOD_SHIFT;
	case KEY_LEFTALT: case KEY_RIGHTALT: return MOD_ALT;
	case KEY_LEFTMETA: case KEY_RIGHTMETA: return MOD_SUPER;
	default: return 0;
	}
}

static void drop_device(size_t i)
{
	close(devices[i].fd);
	devices[i] = devices[--nr_devices];

	phys_mods = 0;
	for (size_t d = 0; d < nr_devices; d++)
		phys_mods |= devices[d].mods;

	mods_apply();
}

static void read_device(struct device *dev)
{
	struct input_event ev;

	while (read(dev->fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
		uint32_t bit;

		if (ev.type != EV_KEY)
			continue;

		/*
		 * Esc ends the session no matter what else is held or in progress,
		 * and before any binding logic can consume it.
		 */
		if (ev.code == KEY_ESC && ev.value) {
			logmsg("exit on Esc (other keys may be held)");
			running = 0;
			return;
		}

		bit = mod_bit(ev.code);
		if (bit) {
			if (ev.value) {
				dev->mods |= bit;
			} else {
				dev->mods &= ~bit;
				/*
				 * Letting go of the real key also lets go of a hold
				 * armed with a toggle key, so a modifier cannot stay
				 * down after the key that produced it is released.
				 */
				armed &= ~bit;
			}

			/* several keyboards can hold modifiers; take the union */
			phys_mods = 0;
			for (size_t d = 0; d < nr_devices; d++)
				phys_mods |= devices[d].mods;

			if (cfg.debug)
				fprintf(stderr, "warpmod: modifier 0x%x %s (physical 0x%x)\n",
					bit, ev.value ? "down" : "up", phys_mods);

			mods_apply();
			continue;
		}

		if (cfg.debug)
			fprintf(stderr, "warpmod: key %u %s (mods 0x%x)\n", ev.code,
				ev.value == 1 ? "down" : ev.value == 0 ? "up" : "repeat",
				dev->mods);

		handle_key(dev, ev.code, ev.value);
	}
}

/* ------------------------------------------------------------------- ticks */

static void tick(double dt)
{
	double ix = 0, iy = 0;
	double target, len, step_x, step_y;
	int dx, dy;

	if (held_left || held_right || held_up || held_down) {
		ix = held_right - held_left;
		iy = held_down - held_up;
		len = (ix && iy) ? 1.41421356 : 1.0;

		target = cfg.max_speed;
		if (decel)
			target = cfg.decelerator_speed;
		else if (accel)
			target = cfg.max_speed * 2.0;

		move_speed += cfg.acceleration * dt;
		if (move_speed > target)
			move_speed = target;
		if (move_speed < cfg.speed)
			move_speed = cfg.speed;

		step_x = move_speed * ix / len * dt + move_frac_x;
		step_y = move_speed * iy / len * dt + move_frac_y;
		dx = (int)step_x;
		dy = (int)step_y;
		move_frac_x = step_x - dx;
		move_frac_y = step_y - dy;

		if (cfg.debug && (dx || dy))
			fprintf(stderr, "warpmod: move %+d%+d -> %d,%d (held u%d d%d l%d r%d speed %.0f dt %.4f)\n",
				dx, dy, px + dx, py + dy, held_up, held_down, held_left,
				held_right, move_speed, dt);

		pointer_move(px + dx, py + dy);
	} else {
		move_speed = cfg.speed;   /* a fresh press starts at full base speed */
		move_frac_x = move_frac_y = 0;
	}

	if (scroll_dir) {
		if (cfg.scroll_acceleration > 0)
			scroll_v = scroll_v + cfg.scroll_acceleration * dt;

		/* one wheel detent is 15 axis units, which is 120 hi-res units */
		wheel_emit(scroll_dir * scroll_v * dt * 8.0);
	}
}

int main(int argc, char **argv)
{
	const char *config_path = NULL;
	char default_config[PATH_MAX];
	int move_x = INT_MIN, move_y = INT_MIN;
	struct pollfd pfds[MAX_DEVICES + 1];
	struct timespec last, now;
	double dt;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--config=", 9))
			config_path = argv[i] + 9;
		else if (!strcmp(argv[i], "--normal"))
			;
		else if (!strcmp(argv[i], "--grab-virtual"))
			cfg.grab_virtual = 1;
		else if (!strcmp(argv[i], "--debug"))
			cfg.debug = 1;
		else if (!strcmp(argv[i], "--check-config"))
			cfg.check_config = 1;
		else if (!strcmp(argv[i], "--probe-once"))
			cfg.probe_once = 1;
		else if (!strcmp(argv[i], "--version")) {
			printf("warpmod %s (%s %s)\n", WARPMOD_VERSION, __DATE__, __TIME__);
			return 0;
		}
		else if (!strcmp(argv[i], "--stop")) {
			char path[PATH_MAX];
			FILE *f = fopen(state_file("pid", path, sizeof path), "r");
			int pid = 0;

			if (f && fscanf(f, "%d", &pid) == 1 && pid > 1) {
				if (kill(pid, SIGTERM) == 0)
					printf("warpmod: asked %d to stop\n", pid);
				else
					printf("warpmod: no such process %d\n", pid);
			} else {
				printf("warpmod: not running\n");
			}
			if (f)
				fclose(f);
			return 0;
		}
		else if (!strcmp(argv[i], "--move") && i + 2 < argc) {
			move_x = atoi(argv[i + 1]);
			move_y = atoi(argv[i + 2]);
			i += 2;
		}
		else {
			fprintf(stderr, "usage: warpmod [--config=PATH]\n");
			return 2;
		}
	}

	if (!config_path) {
		const char *home = getenv("HOME");

		snprintf(default_config, sizeof default_config, "%s/.config/warpd/config",
			 home ? home : ".");
		config_path = default_config;
	}

	start_ms = now_ms();
	if (!acquire_lock()) {
		fprintf(stderr, "warpmod: another instance is already running "
				"(use `warpmod --stop`)\n");
		return 1;
	}
	write_pidfile();

	{
		char path[PATH_MAX];

		log_file = fopen(state_file("log", path, sizeof path), "w");
	}
	logmsg("warpmod %s (%s %s) pid %d", WARPMOD_VERSION, __DATE__, __TIME__,
	       (int)getpid());

	config_load(config_path);
	phase("config");

	xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xkb_ctx) {
		fprintf(stderr, "warpmod: no xkb context\n");
		return 1;
	}
	xkb_km = xkb_keymap_new_from_names(xkb_ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!xkb_km) {
		fprintf(stderr, "warpmod: cannot build keymap\n");
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);    /* closing the terminal must release the keys */
	signal(SIGQUIT, on_signal);

	phase("xkb");
	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "warpmod: cannot connect to Wayland\n");
		return 1;
	}

	{
		struct wl_registry *registry = wl_display_get_registry(dpy);

		wl_registry_add_listener(registry, &registry_listener, NULL);
	}
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);

	phase("wayland");

	if (cfg.debug) {
		fprintf(stderr, "warpmod: %zu output(s), layout %dx%d at %d,%d\n", nr_outputs,
			layout_w(), layout_h(), layout_edge(1, 0), layout_edge(0, 0));
		for (size_t i = 0; i < nr_outputs; i++)
			fprintf(stderr, "warpmod:   output %zu %-8s at %d,%d size %dx%d scale %d\n", i,
				outputs[i].name[0] ? outputs[i].name : "?",
				outputs[i].x, outputs[i].y, outputs[i].w, outputs[i].h,
				outputs[i].scale);
	}

	if (!ptr_mgr) {
		fprintf(stderr, "warpmod: compositor has no virtual pointer manager\n");
		return 1;
	}

	ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(ptr_mgr, NULL);
	wl_display_roundtrip(dpy);

	if (move_x != INT_MIN) {
		pointer_move(move_x, move_y);
		usleep(50000);
		zwlr_virtual_pointer_v1_destroy(ptr);
		wl_display_disconnect(dpy);
		return 0;
	}

	if (cfg.probe_once) {
		int x = 0, y = 0;

		if (probe_position(&x, &y))
			printf("%d %d\n", x, y);
		else
			printf("none\n");
		return 0;
	}

	phase("uinput done");
	{
		int keys[256];
		int nkeys = 0;

		for (int c = 1; c < 256; c++)
			keys[nkeys++] = c;

		int rels[] = { REL_WHEEL, REL_WHEEL_HI_RES, REL_HWHEEL, REL_HWHEEL_HI_RES };
		int buttons[] = { BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA };

		ui_kbd = ui_create("warpmod virtual keyboard", keys, nkeys, NULL, 0);
		phase("ui_kbd");
		ui_mouse = ui_create("warpmod virtual mouse", buttons,
				     sizeof buttons / sizeof buttons[0], rels, 4);
		phase("ui_mouse");
	}

	if (ui_kbd < 0 || ui_mouse < 0) {
		fprintf(stderr, "warpmod: cannot create uinput devices\n");
		return 1;
	}

	if (cfg.check_config) {
		struct { const char *name; const char *value; } binds[] = {
			{ "up", cfg.up }, { "down", cfg.down }, { "left", cfg.left },
			{ "right", cfg.right }, { "exit", cfg.exit_key },
			{ "buttons", cfg.buttons }, { "drag", cfg.drag },
			{ "scroll_up", cfg.scroll_up }, { "scroll_down", cfg.scroll_down },
			{ "scroll_left", cfg.scroll_left }, { "scroll_right", cfg.scroll_right },
			{ "mod_ctrl", cfg.mod_ctrl }, { "mod_shift", cfg.mod_shift },
			{ "mod_alt", cfg.mod_alt }, { "mod_super", cfg.mod_super },
		};

		for (size_t b = 0; b < sizeof binds / sizeof binds[0]; b++) {
			char buf[256];
			char *tok, *save = NULL;
			int ok = 0, bad = 0;

			snprintf(buf, sizeof buf, "%s", binds[b].value ? binds[b].value : "");
			for (tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
				uint32_t code;
				uint8_t mods;

				if (parse_token(tok, &code, &mods))
					ok++;
				else
					bad++;
			}
			fprintf(stderr, "warpmod: %-11s = %-12s %d key(s) resolved, %d unresolved\n",
				binds[b].name, binds[b].value ? binds[b].value : "-", ok, bad);
		}
		exit(0);
	}

	/*
	 * Discover the pointer first, then take the keyboards: grabbing before the
	 * probe would swallow keys for the duration of the probe.
	 */
	phase("probe start");

	if (probe_position(&px, &py)) {
		if (cfg.debug)
			fprintf(stderr, "warpmod: pointer is at %d,%d\n", px, py);
		save_position(px, py);
	} else if (load_position(&px, &py)) {
		pointer_move(px, py);
	} else {
		px = layout_edge(1, 0) + layout_w() / 2;
		py = layout_edge(0, 0) + layout_h() / 2;
		pointer_move(px, py);
	}

	/* the layout was just read, so it has nothing to follow yet */
	layout_changed = 0;

	/*
	 * Discover the keyboards, wait for the launch chord to be released (that is
	 * as long as the chord is actually held), then take them over.
	 */
	open_keyboards();
	wait_until_released(600);
	grab_open();

	phase("ready");

	if (!nr_devices) {
		fprintf(stderr, "warpmod: no keyboard could be grabbed, exiting\n");
		logmsg("no keyboard could be grabbed");
		ungrab_keyboards();
		ioctl(ui_kbd, UI_DEV_DESTROY);
		ioctl(ui_mouse, UI_DEV_DESTROY);
		close(ui_kbd);
		close(ui_mouse);
		remove_pidfile();
		zwlr_virtual_pointer_v1_destroy(ptr);
		wl_display_disconnect(dpy);
		return 1;
	}

	fprintf(stderr, "warpmod: %zu keyboard(s) grabbed; Esc exits\n", nr_devices);
	logmsg("started: %zu output(s) in %dx%d at %d,%d, %zu keyboard(s)", nr_outputs,
	       layout_w(), layout_h(), layout_edge(1, 0), layout_edge(0, 0), nr_devices);

	clock_gettime(CLOCK_MONOTONIC, &last);

	while (running) {
		int nfds = 0;
		int ret;

		for (size_t d = 0; d < nr_devices; d++) {
			pfds[nfds].fd = devices[d].fd;
			pfds[nfds].events = POLLIN;
			nfds++;
		}

		pfds[nfds].fd = wl_display_get_fd(dpy);
		pfds[nfds].events = POLLIN;

		wl_display_flush(dpy);
		ret = poll(pfds, nfds + 1, 8);
		if (ret < 0 && errno != EINTR) {
			perror("warpmod: poll");
			break;
		}

		for (size_t d = nr_devices; d > 0; d--) {
			size_t i = d - 1;

			if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
				if (cfg.debug)
					fprintf(stderr, "warpmod: device %zu went away\n", i);
				drop_device(i);
			} else if (pfds[i].revents & POLLIN) {
				read_device(&devices[i]);
			}
		}

		if (pfds[nfds].revents & POLLIN)
			wl_display_dispatch(dpy);

		/*
		 * An output changed position, mode or scale: rebuild the mapping
		 * before the next movement uses it. Outputs that are not
		 * configured yet are not part of the layout, so wait for a size.
		 */
		if (layout_changed && layout_w() > 0 && layout_h() > 0) {
			layout_changed = 0;
			relayout();
		}

		clock_gettime(CLOCK_MONOTONIC, &now);
		dt = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
		last = now;

		if (dt > 0.05)
			dt = 0.05;

		if (!nr_devices && now_ms() - last_rescan > 1000) {
			last_rescan = now_ms();
			grab_keyboards();   /* pick a keyboard up again if one reconnected */

			if (!nr_devices) {
				fprintf(stderr, "warpmod: no keyboard left, exiting\n");
				running = 0;
			}
		}

		/* Self-healing: whatever is wanted is pressed, everything else is not. */
		mods_apply();

		tick(dt);

		tick_count++;
		if (cfg.debug && tick_count % 125 == 0)
			fprintf(stderr, "warpmod: ticks %u dt %.4f held %d%d%d%d speed %.0f at %d,%d devices %zu wheel %u\n",
				tick_count, dt, held_up, held_down, held_left, held_right,
				move_speed, px, py, nr_devices, wheel_count);
	}

	/*
	 * Release everything the application may be holding, then give the
	 * compositor time to deliver those releases before the virtual devices
	 * go away. Destroying a device with keys still down relies on the
	 * compositor synthesising releases, which is not guaranteed.
	 */
	signal(SIGALRM, SIG_DFL);
	alarm(2);   /* if cleanup ever blocks, the kernel still frees the grabs */

	armed = 0;
	phys_mods = 0;
	mods_release(MOD_CTRL | MOD_SHIFT | MOD_ALT | MOD_SUPER);

	/* Also send plain releases for every modifier, whatever the bookkeeping
	 * thinks is held, so the application cannot be left with one down. */
	ui_emit(ui_kbd, EV_KEY, KEY_LEFTCTRL, 0);
	ui_emit(ui_kbd, EV_KEY, KEY_LEFTSHIFT, 0);
	ui_emit(ui_kbd, EV_KEY, KEY_LEFTALT, 0);
	ui_emit(ui_kbd, EV_KEY, KEY_LEFTMETA, 0);
	if (dragging)
		ui_emit(ui_mouse, EV_KEY, BTN_LEFT, 0);
	ui_sync(ui_kbd);
	ui_sync(ui_mouse);
	wl_display_flush(dpy);
	usleep(200000);

	ungrab_keyboards();

	ioctl(ui_kbd, UI_DEV_DESTROY);
	ioctl(ui_mouse, UI_DEV_DESTROY);
	close(ui_kbd);
	close(ui_mouse);

	save_position_force(px, py);
	remove_pidfile();
	logmsg("exit at %d,%d", px, py);

	zwlr_virtual_pointer_v1_destroy(ptr);
	wl_display_disconnect(dpy);

	return 0;
}
