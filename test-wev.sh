#!/bin/bash
# Prototype test: warpmod holds ctrl for an application, observed by wev.
#
# The app keeps keyboard focus the whole time (no enter/leave churn): the
# modifier arrives because warpmod presses it on a uinput device, not because
# focus was handed over. Expected in wev's log: Control_L pressed, modifiers
# with Control depressed, axis events, Control_L released.
export WAYLAND_DISPLAY=wayland-1

WARPMOD=/home/aru/Projects/warpmod/proto/warpmod
UIKEY=/home/aru/Projects/warpmod/proto/uikey
DIR=/home/aru/.amp/in/clip-diag

: > "$DIR/wev.log"
: > "$DIR/warpmod.log"
rm -f /tmp/vkcmd; mkfifo /tmp/vkcmd

stdbuf -oL -eL wev -f wl_keyboard -f wl_pointer >"$DIR/wev.log" 2>&1 &
WEV=$!

# A long-lived virtual keyboard that warpmod can grab and that we can drive.
"$UIKEY" < /tmp/vkcmd >>"$DIR/warpmod.log" 2>&1 &
UIKEY_PID=$!
exec 3>/tmp/vkcmd
sleep 2

cleanup() {
	[ -n "$WARPMOD_PID" ] && kill "$WARPMOD_PID" 2>/dev/null
	[ -n "$UIKEY_PID" ] && kill "$UIKEY_PID" 2>/dev/null
	[ -n "$WEV_PID" ] && kill "$WEV_PID" 2>/dev/null
	[ -n "$WEV" ] && kill "$WEV" 2>/dev/null
	exec 3>&- 2>/dev/null
	rm -f /tmp/vkcmd
}
trap cleanup EXIT

WID=$(niri msg -j windows | python3 -c '
import json,sys
for w in json.load(sys.stdin):
    if "wev" in (w.get("title") or "").lower(): print(w["id"]); break')
niri msg action fullscreen-window --id "$WID" 2>&1 | sed "s/^/niri: /"
sleep 1

"$WARPMOD" --grab-virtual --debug >>"$DIR/warpmod.log" 2>&1 &
WARPMOD_PID=$!
sleep 1

echo "--- arm ctrl (S-1)";      echo "S-1" >&3; sleep 0.4
echo "--- scroll down (m)";     for i in 1 2 3 4 5 6 7 8; do echo "m" >&3; done; sleep 1.5
echo "--- disarm ctrl (S-1)";   echo "S-1" >&3; sleep 0.6
echo "--- scroll down again (control)"; echo "m" >&3; sleep 1.5
echo "--- exit";                echo "Escape" >&3; sleep 1

echo
echo "=== warpmod log ==="
cat "$DIR/warpmod.log"
echo
echo "=== what the focused application received ==="
grep -E "wl_keyboard] (key|enter|leave)|axis: time" "$DIR/wev.log" | head -24
echo
echo "keyboard enter/leave events in total (churn = old focus-handoff approach):"
grep -cE "wl_keyboard] (enter|leave)" "$DIR/wev.log"
echo "Control_L events: $(grep -c 'sym: Control_L' "$DIR/wev.log")"
echo "axis events: $(grep -c 'wl_pointer] axis:' "$DIR/wev.log")"
