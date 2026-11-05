#!/bin/bash
place_pointer_in_observer() {
	python3 - "$WID" <<'PY' > /tmp/pt
import json, subprocess, sys
wid = int(sys.argv[1])
wins = json.loads(subprocess.check_output(["niri", "msg", "-j", "windows"]))
w = [x for x in wins if x["id"] == wid][0]
ws = json.loads(subprocess.check_output(["niri", "msg", "-j", "workspaces"]))
out_name = [x for x in ws if x["id"] == w.get("workspace_id")][0].get("output")
outs = json.loads(subprocess.check_output(["niri", "msg", "-j", "outputs"]))
o = outs[out_name]["logical"]
print(o["x"] + o["width"] // 2, o["y"] + o["height"] // 2)
PY
	./warpmod --move $(cat /tmp/pt)
	sleep 0.4
}

# End-to-end check with wev as the observed application.
export WAYLAND_DISPLAY=wayland-1 WARPMOD_TEST_ONLY_VIRTUAL=1
DIR=/home/aru/.amp/in/clip-diag

cycle() {  # wait for a fresh wev window
	for i in $(seq 1 30); do
		W=$(niri msg -j windows | python3 -c '
import json,sys
ids=[w["id"] for w in json.load(sys.stdin) if (w.get("title") or "").lower()=="wev"]
print(max(ids) if ids else "")')
		[ -n "$W" ] && { echo "$W"; return; }
		sleep 0.2
	done
}

PREV_WS=$(niri msg -j workspaces | python3 -c '
import json,sys
for w in json.load(sys.stdin):
    if w.get("is_active") and (w.get("output") or "").startswith("eDP"): print(w["id"]); break')

: > "$DIR/wev.log"; : > /tmp/wmz.log
rm -f /tmp/vkz; mkfifo /tmp/vkz
stdbuf -oL -eL wev -f wl_keyboard -f wl_pointer >"$DIR/wev.log" 2>&1 & WEV=$!
./uikey < /tmp/vkz >/dev/null 2>&1 & UI=$!
exec 3>/tmp/vkz
sleep 1

WID=$(cycle)
echo "observer window: ${WID:-none}"
niri msg action focus-window --id "$WID" >/dev/null
sleep 0.5
niri msg action fullscreen-window --id "$WID" >/dev/null
sleep 0.8
place_pointer_in_observer
sleep 0.8
niri msg windows | grep -A2 "Window ID $WID:" | head -3

./warpmod --grab-virtual --debug >/tmp/wmz.log 2>&1 & WM=$!
sleep 1.2

echo "--- hold ctrl, scroll, release"
echo "down Control_L" >&3; sleep 0.3; echo "m" >&3; sleep 0.7; echo "up Control_L" >&3; sleep 0.4
echo "--- toggle ctrl with x, scroll"
echo "x" >&3; sleep 0.4; echo "m" >&3; sleep 0.6; echo "x" >&3; sleep 0.3
echo "--- middle click via r"
echo "r" >&3; sleep 0.6
echo "--- escape"
echo "Escape" >&3; sleep 0.8

kill $UI $WEV $WM 2>/dev/null; exec 3>&-; rm -f /tmp/vkz
niri msg action fullscreen-window --id "$WID" >/dev/null 2>&1
[ -n "$PREV_WS" ] && niri msg action focus-workspace "$PREV_WS" >/dev/null 2>&1

echo
echo "symbols: $(grep -oE 'sym: [A-Za-z_]+' "$DIR/wev.log" | sort | uniq -c | tr '\n' ' ')"
echo "buttons: $(grep -oE 'button: [0-9]+ \([a-z]+\)' "$DIR/wev.log" | sort | uniq -c | tr '\n' ' ')"
echo "axes:    $(grep -c 'axis_value120' "$DIR/wev.log")"
echo "focus:   $(grep -cE 'wl_keyboard] enter' "$DIR/wev.log") enter, $(grep -cE 'wl_keyboard] leave' "$DIR/wev.log") leave"
echo "warpmod: $(grep -E 'pointer is at|grabbed' /tmp/wmz.log | tr '\n' ' ')"
