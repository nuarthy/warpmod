/*
 * uiwheel - inject modifier + wheel through /dev/uinput, at kernel level.
 *
 * The point: a modifier pressed through uinput is a real key on a real device,
 * so it changes the compositor's seat state. The focused application learns
 * about it from its own wl_keyboard, no keyboard handover needed, which is what
 * warpd's Wayland virtual-keyboard approach cannot do while warpd holds focus.
 *
 * Usage:
 *   uiwheel --mods=ctrl --steps=60 --units=15 --delay=10
 *   uiwheel --mods=none --steps=10 --units=120 --delay=50   (detent-style)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>

#define KEY_LEFTCTRL  29
#define KEY_LEFTSHIFT 42
#define KEY_LEFTALT   56
#define KEY_LEFTMETA  125

static int uifd = -1;

static void emit(int type, int code, int value)
{
	struct input_event ev = {0};

	ev.type = type;
	ev.code = code;
	ev.value = value;

	if (write(uifd, &ev, sizeof ev) != (ssize_t)sizeof ev) {
		perror("uiwheel: write");
		exit(1);
	}
}

static void sync_now(void)
{
	emit(EV_SYN, SYN_REPORT, 0);
}

static int create_device(void)
{
	struct uinput_setup setup = {0};
	int fd;

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("uiwheel: open /dev/uinput");
		exit(1);
	}

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_REL);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);

	ioctl(fd, UI_SET_KEYBIT, KEY_LEFTCTRL);
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFTSHIFT);
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFTALT);
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFTMETA);
	ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
	ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
	ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);

	ioctl(fd, UI_SET_RELBIT, REL_X);
	ioctl(fd, UI_SET_RELBIT, REL_Y);
	ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
	ioctl(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES);

	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x1234;
	setup.id.product = 0x5678;
	setup.id.version = 1;
	snprintf(setup.name, sizeof setup.name, "uiwheel test device");

	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
		perror("uiwheel: UI_DEV_SETUP");
		exit(1);
	}
	if (ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("uiwheel: UI_DEV_CREATE");
		exit(1);
	}

	return fd;
}

int main(int argc, char **argv)
{
	const char *mods = "ctrl";
	int steps = 10, units = 15, delay = 10;
	int codes[4], ncodes = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--mods=", 7)) mods = argv[i] + 7;
		else if (!strncmp(argv[i], "--steps=", 8)) steps = atoi(argv[i] + 8);
		else if (!strncmp(argv[i], "--units=", 8)) units = atoi(argv[i] + 8);
		else if (!strncmp(argv[i], "--delay=", 8)) delay = atoi(argv[i] + 8);
		else { fprintf(stderr, "uiwheel: unknown argument %s\n", argv[i]); return 2; }
	}

	if (strcmp(mods, "none")) {
		char buf[64];
		char *tok, *save = NULL;

		snprintf(buf, sizeof buf, "%s", mods);
		for (tok = strtok_r(buf, ",", &save); tok && ncodes < 4;
		     tok = strtok_r(NULL, ",", &save)) {
			if (!strcmp(tok, "ctrl")) codes[ncodes++] = KEY_LEFTCTRL;
			else if (!strcmp(tok, "shift")) codes[ncodes++] = KEY_LEFTSHIFT;
			else if (!strcmp(tok, "alt")) codes[ncodes++] = KEY_LEFTALT;
			else if (!strcmp(tok, "super")) codes[ncodes++] = KEY_LEFTMETA;
			else { fprintf(stderr, "uiwheel: unknown modifier '%s'\n", tok); return 2; }
		}
	}

	uifd = create_device();
	usleep(200000);  /* let the compositor pick the device up */

	for (i = 0; i < ncodes; i++) {
		emit(EV_KEY, codes[i], 1);
		sync_now();
	}
	usleep(50000);

	for (i = 0; i < steps; i++) {
		emit(EV_REL, REL_WHEEL_HI_RES, -units);
		sync_now();
		usleep(delay * 1000);
	}
	usleep(50000);

	for (i = ncodes; i > 0; i--) {
		emit(EV_KEY, codes[i - 1], 0);
		sync_now();
	}

	printf("uiwheel: %d step(s) of %d hi-res units with mods=%s\n", steps, units, mods);
	fflush(stdout);

	ioctl(uifd, UI_DEV_DESTROY);
	close(uifd);
	return 0;
}
