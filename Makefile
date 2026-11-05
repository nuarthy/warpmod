CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags wayland-client xkbcommon)
LDLIBS += $(shell pkg-config --libs wayland-client xkbcommon)

all: warpmod uikey uiwheel

warpmod: warpmod.c virtual-pointer.c layer-shell.c xdg-shell.c xdg-output.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

uikey: uikey.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

uiwheel: uiwheel.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

install: warpmod
	install -Dm755 warpmod $(HOME)/.local/bin/warpmod

clean:
	rm -f warpmod uikey uiwheel

.PHONY: all install clean
