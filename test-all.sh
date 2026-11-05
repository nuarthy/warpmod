#!/bin/bash
# End-to-end check of the three reported bugs: exit, click, scroll direction.
export WAYLAND_DISPLAY=wayland-1
DIR=/home/aru/.amp/in/clip-diag
: > "$DIR/wev.log"; : > /tmp/wm.log
rm -f /tmp/vkcmd3; mkfifo /tmp/vkcmd3

stdbuf -oL -eL wev -f wl_keyboard -f wl_pointer >"$DIR/wev.log" 2>&1 & WEV=$!
./uikey < /tmp/vkcmd3 >/dev/null 2>&1 & UIKEY_PID=$!
exec 3>/tmp/vkcmd3
sleep 1.5

WID=$(niri msg -j windows | python3 -c '
import json,sys
for w in json.load(sys.stdin):
    if "wev" in (w.get("title") or "").lower(): print(w["id"]); break')
niri msg action fullscreen-window --id "$WID" >/dev/null
sleep 1

./warpmod --grab-virtual --debug >/tmp/wm.log 2>&1 & WM_PID=$!
sleep 1

echo "--- click (f)";            echo "f" >&3; sleep 0.8
echo "--- scroll up (,)";        echo "comma" >&3; sleep 0.8
echo "--- scroll down (m)";      echo "m" >&3; sleep 0.8
echo "--- escape";               echo "Escape" >&3; sleep 1.2

if kill -0 $WM_PID 2>/dev/null; then echo "RESULT: warpmod STILL RUNNING after Esc"; kill $WM_PID;
else echo "RESULT: warpmod exited on Esc"; fi

kill $UIKEY_PID 2>/dev/null; exec 3>&-; kill $WEV 2>/dev/null; rm -f /tmp/vkcmd3; sleep 0.3
niri msg action fullscreen-window --id "$WID" >/dev/null 2>&1

echo; echo "=== pointer button events the app received ==="
grep -E "wl_pointer] button" "$DIR/wev.log" | head -4
echo "=== axis events (positive = scroll down in Wayland terms) ==="
grep -E "wl_pointer] axis_value120" "$DIR/wev.log" | head -8
echo "=== warpmod log ==="; grep -E "grabbed|wheel [0-9]|mods|armed" /tmp/wm.log | head -6
