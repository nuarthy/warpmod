#!/bin/bash
# Real modifier keys (hold) + z/x/c/v toggles + middle click on r.
export WAYLAND_DISPLAY=wayland-1 WARPMOD_TEST_ONLY_VIRTUAL=1
DIR=/home/aru/.amp/in/clip-diag
: > "$DIR/wev.log"; : > /tmp/wm-hold.log
rm -f /tmp/vkc6; mkfifo /tmp/vkc6

stdbuf -oL -eL wev -f wl_keyboard -f wl_pointer >"$DIR/wev.log" 2>&1 & WEV=$!
./uikey < /tmp/vkc6 >/tmp/uikey6.log 2>&1 & UI=$!
exec 3>/tmp/vkc6; sleep 1.5
WID=
for i in $(seq 1 20); do
	WID=$(niri msg -j windows | python3 -c '
import json,sys
for w in json.load(sys.stdin):
    if "wev" in (w.get("title") or "").lower(): print(w["id"]); break')
	[ -n "$WID" ] && break
	sleep 0.4
done
echo "observer window: ${WID:-none}"
niri msg action fullscreen-window --id "$WID" >/dev/null; sleep 0.6
# put the pointer inside the observer: focus follows the mouse
/home/aru/.local/bin/warpd-mods --move '960 600' >/dev/null 2>&1
sleep 0.8
niri msg -j windows | python3 -c "
import json,sys
for w in json.load(sys.stdin):
    if w.get('id')==$WID:
        print('observer focused:', w.get('is_focused'), 'fullscreen:', w.get('is_fullscreen'))"
./warpmod --grab-virtual --debug >/tmp/wm-hold.log 2>&1 & WM=$!
sleep 1

PREV_WS=$(niri msg -j workspaces | python3 -c '
import json,sys
for w in json.load(sys.stdin):
    if w.get("is_active") and w.get("output","").startswith("eDP"): print(w["id"]); break')
echo "--- hold real ctrl, scroll, release ctrl"
echo "down Control_L" >&3; sleep 0.3; echo "m" >&3; sleep 0.6; echo "up Control_L" >&3; sleep 0.4
echo "--- toggle ctrl with x, scroll, toggle off"
echo "x" >&3; sleep 0.4; echo "m" >&3; sleep 0.5; echo "x" >&3; sleep 0.4
echo "--- toggle shift (v), alt (c), super (z) then off"
echo "v" >&3; sleep 0.3; echo "v" >&3; sleep 0.3
echo "c" >&3; sleep 0.3; echo "c" >&3; sleep 0.3
echo "--- middle click on r"
echo "r" >&3; sleep 0.6
echo "--- escape"
echo "Escape" >&3; sleep 0.8

kill $UI $WEV 2>/dev/null; exec 3>&-; rm -f /tmp/vkc6
niri msg action fullscreen-window --id "$WID" >/dev/null 2>&1
[ -n "$PREV_WS" ] && niri msg action focus-workspace "$PREV_WS" >/dev/null 2>&1

echo; echo "=== modifier symbols the app saw ==="
grep -oE "sym: [A-Za-z_]+" "$DIR/wev.log" | sort | uniq -c
echo "=== buttons ==="; grep -oE "button: [0-9]+ \([a-z]+\)" "$DIR/wev.log" | sort | uniq -c
echo "=== axis count ==="; grep -c "axis_value120" "$DIR/wev.log"
echo "=== warpmod decisions ==="; grep -E "modifier|armed|key 114|key 53|key 54|key 52|key 19" /tmp/wm-hold.log | head -12
