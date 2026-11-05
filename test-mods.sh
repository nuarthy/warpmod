#!/bin/bash
# Modifier check: each modifier key must reach the focused app as a real key,
# and super must stay lazy (not pressed until an action happens).
export WAYLAND_DISPLAY=wayland-1
DIR=/home/aru/.amp/in/clip-diag
WARPMOD_TEST_ONLY_VIRTUAL=1
export WARPMOD_TEST_ONLY_VIRTUAL
: > "$DIR/wev.log"; : > /tmp/wm-mods.log
rm -f /tmp/vkcmd4; mkfifo /tmp/vkcmd4

stdbuf -oL -eL wev -f wl_keyboard -f wl_pointer >"$DIR/wev.log" 2>&1 & WEV=$!
./uikey < /tmp/vkcmd4 >/dev/null 2>&1 & UIKEY_PID=$!
exec 3>/tmp/vkcmd4
sleep 1.5

WID=$(niri msg -j windows | python3 -c '
import json,sys
for w in json.load(sys.stdin):
    if "wev" in (w.get("title") or "").lower(): print(w["id"]); break')
niri msg action fullscreen-window --id "$WID" >/dev/null
sleep 1

./warpmod --grab-virtual --debug >/tmp/wm-mods.log 2>&1 & WM_PID=$!
sleep 1

echo "--- arm ctrl (S-1), scroll (m), disarm"
echo "S-1" >&3; sleep 0.4; echo "m" >&3; sleep 0.7; echo "S-1" >&3; sleep 0.4
echo "--- shift (S-2) on/off"
echo "S-2" >&3; sleep 0.4; echo "S-2" >&3; sleep 0.4
echo "--- alt (S-3) on/off"
echo "S-3" >&3; sleep 0.4; echo "S-3" >&3; sleep 0.4
echo "--- super (S-4) armed only: must stay lazy"
echo "S-4" >&3; sleep 0.8
echo "--- super armed + click (f): presses super around the click"
echo "f" >&3; sleep 0.8
echo "S-4" >&3; sleep 0.4
echo "--- escape"
echo "Escape" >&3; sleep 1

kill $UIKEY_PID 2>/dev/null; exec 3>&-; kill $WEV 2>/dev/null; rm -f /tmp/vkcmd4; sleep 0.2
niri msg action fullscreen-window --id "$WID" >/dev/null 2>&1

echo; echo "=== keys the focused app received, in order ==="
grep -E "wl_keyboard] key:" "$DIR/wev.log" | sed 's/.*key: [0-9]*; //' | head -20
echo "=== modifier symbols seen ==="
grep -oE "sym: [A-Za-z_]+" "$DIR/wev.log" | sort | uniq -c
echo "=== warpmod log ==="; grep -E "armed|key 4[02-9]|key 5[0-9]|key 125|wheel" /tmp/wm-mods.log | head -12
