/*
 * uikey - inject keys through /dev/uinput, one key name per line on stdin.
 *
 * Sibling of uiwheel, used to drive warpmod in tests: the events appear on a
 * real evdev device, so a tool that grabbed that device receives them and the
 * compositor receives nothing.
 *
 * Keys can be given as keysym names ("m", "Escape"), with modifier prefixes
 * ("S-1", "C-a"), or as raw evdev codes ("#29").
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <xkbcommon/xkbcommon.h>

#define KEY_LEFTCTRL  29
#define KEY_LEFTSHIFT 42
#define KEY_LEFTALT   56
#define KEY_LEFTMETA  125

static int uifd = -1;
static struct xkb_keymap *km;

static void emit(int type, int code, int value)
{
	struct input_event ev = {0};

	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(uifd, &ev, sizeof ev) != (ssize_t)sizeof ev) {
		perror("uikey: write");
		exit(1);
	}
}

static void sync_now(void)
{
	emit(EV_SYN, SYN_REPORT, 0);
}

static int mod_code(const char *arg)
{
	switch (arg[0]) {
	case 'C': return KEY_LEFTCTRL;
	case 'S': return KEY_LEFTSHIFT;
	case 'A': return KEY_LEFTALT;
	case 'M': return KEY_LEFTMETA;
	default:  return 0;
	}
}

static int keycode_for(xkb_keysym_t target)
{
	xkb_keycode_t kc;

	for (kc = xkb_keymap_min_keycode(km); kc <= xkb_keymap_max_keycode(km); kc++) {
		const xkb_keysym_t *syms;

		if (xkb_keymap_key_get_syms_by_level(km, kc, 0, 0, &syms) > 0 &&
		    syms[0] == target)
			return kc - 8;
	}
	return 0;
}

int main(void)
{
	struct uinput_setup setup = {0};
	struct xkb_context *ctx;
	char line[256];

	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

	uifd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (uifd < 0) {
		perror("uikey: open /dev/uinput");
		return 1;
	}

	ioctl(uifd, UI_SET_EVBIT, EV_KEY);
	ioctl(uifd, UI_SET_EVBIT, EV_SYN);
	for (int c = 1; c < 256; c++)
		ioctl(uifd, UI_SET_KEYBIT, c);

	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x1234;
	setup.id.product = 0x0003;
	snprintf(setup.name, sizeof setup.name, "uikey test keyboard");
	ioctl(uifd, UI_DEV_SETUP, &setup);
	if (ioctl(uifd, UI_DEV_CREATE) < 0) {
		perror("uikey: UI_DEV_CREATE");
		return 1;
	}

	fprintf(stderr, "uikey: ready\n");

	while (fgets(line, sizeof line, stdin)) {
		const char *arg = line;
		int mods[4], nmods = 0, m, code;
		size_t len = strlen(line);

		while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = 0;
		if (!len)
			continue;

		if (!strncmp(arg, "down ", 5) || !strncmp(arg, "up ", 3)) {
			int down = arg[0] == 'd';
			const char *key = arg + (down ? 5 : 3);
			xkb_keysym_t sym = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
			int code2 = sym == XKB_KEY_NoSymbol ? 0 : keycode_for(sym);

			if (!code2) {
				fprintf(stderr, "uikey: unknown key '%s'\n", key);
				continue;
			}
			emit(EV_KEY, code2, down ? 1 : 0);
			sync_now();
			fprintf(stderr, "uikey: %s %s (code %d)\n", down ? "hold" : "release", key, code2);
			continue;
		}

		while (arg[0] && arg[1] == '-' && arg[2] && mod_code(arg)) {
			mods[nmods++] = mod_code(arg);
			arg += 2;
		}

		if (arg[0] == '#') {
			code = atoi(arg + 1);
		} else {
			xkb_keysym_t sym = xkb_keysym_from_name(arg, XKB_KEYSYM_NO_FLAGS);

			code = sym == XKB_KEY_NoSymbol ? 0 : keycode_for(sym);
		}

		if (!code) {
			fprintf(stderr, "uikey: unknown key '%s'\n", arg);
			continue;
		}

		for (m = 0; m < nmods; m++)
			emit(EV_KEY, mods[m], 1);
		emit(EV_KEY, code, 1);
		sync_now();
		usleep(60000);
		emit(EV_KEY, code, 0);
		for (m = nmods; m > 0; m--)
			emit(EV_KEY, mods[m - 1], 0);
		sync_now();
		usleep(60000);

		fprintf(stderr, "uikey: %s (code %d)\n", line, code);
	}

	ioctl(uifd, UI_DEV_DESTROY);
	close(uifd);
	return 0;
}
