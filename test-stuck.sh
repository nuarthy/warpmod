#!/bin/bash
# Reproduce the two failures reported: a modifier left stuck in the application,
# and a session that cannot be ended because its grabbed keyboard disappeared.
export WAYLAND_DISPLAY=wayland-1 WARPMOD_TEST_ONLY_VIRTUAL=1
DIR=/home/aru/.amp/in/clip-diag

: > "$DIR/wev.log"; : > /tmp/wmst.log
rm -f /tmp/vkst; mkfifo /tmp/vkst
stdbuf -oL -eL wev -f wl_keyboard -f wl_pointer >"$DIR/wev.log" 2>&1 & WEV=$!
./uikey < /tmp/vkst >/dev/null 2>&1 & UI=$!
exec 3>/tmp/vkst
sleep 1

WID=
for i in $(seq 1 30); do
	WID=$(niri msg -j windows | python3 -c '
import json,sys
ids=[w["id"] for w in json.load(sys.stdin) if (w.get("title") or "").lower()=="wev"]
print(max(ids) if ids else "")')
	[ -n "$WID" ] && break
	sleep 0.2
done
niri msg action focus-window --id "$WID" >/dev/null; sleep 0.4
niri msg action fullscreen-window --id "$WID" >/dev/null; sleep 0.5
/home/aru/.local/bin/warpd-mods --move '960 600' >/dev/null 2>&1
sleep 0.6

./warpmod --grab-virtual --debug >/tmp/wmst.log 2>&1 & WM=$!
sleep 1

echo "--- hold ctrl, then make the keyboard disappear (kill the injector)"
echo "down Control_L" >&3
sleep 0.6
kill $UI 2>/dev/null
exec 3>&-
sleep 1.2

echo "warpmod alive after its device vanished: $(kill -0 $WM 2>/dev/null && echo yes || echo no)"
echo "app still sees Control held: $(grep -c 'sym: Control_L' "$DIR/wev.log") control events"
grep -E "modifier|key 29" /tmp/wmst.log | tail -3

echo "--- press escape (via a fresh injector: warpmod should still be reachable)"
rm -f /tmp/vkst2; mkfifo /tmp/vkst2
./uikey < /tmp/vkst2 >/dev/null 2>&1 & UI2=$!
exec 4>/tmp/vkst2
sleep 0.8
echo "Escape" >&4
sleep 1.2
echo "warpmod alive after Esc: $(kill -0 $WM 2>/dev/null && echo yes || echo no)"

echo "--- terminate it and check the modifier is released to the app"
kill $WM 2>/dev/null; sleep 1
echo "total Control_L events in app: $(grep -c 'sym: Control_L' "$DIR/wev.log") (expect an even number: press and release)"
kill $UI2 $WEV 2>/dev/null; exec 4>&-; rm -f /tmp/vkst /tmp/vkst2
niri msg action fullscreen-window --id "$WID" >/dev/null 2>&1
