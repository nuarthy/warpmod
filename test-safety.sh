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

# Modifier safety checks.
#   A: holding the real ctrl, then arming with x, then releasing ctrl must release
#      the modifier (a release of the real key ends the hold).
#   B: exiting with ctrl armed must release ctrl in the application.
#   C: if the grabbed keyboard disappears, the session ends instead of hanging
#      with a modifier stuck down.
export WAYLAND_DISPLAY=wayland-1 WARPMOD_TEST_ONLY_VIRTUAL=1
DIR=/home/aru/.amp/in/clip-diag

start_wev() {
	: > "$DIR/wev.log"
	stdbuf -oL -eL wev -f wl_keyboard -f wl_pointer >"$DIR/wev.log" 2>&1 & WEV=$!
	sleep 1
	WID=$(niri msg -j windows | python3 -c '
import json,sys
ids=[w["id"] for w in json.load(sys.stdin) if (w.get("title") or "").lower()=="wev"]
print(max(ids) if ids else "")')
	niri msg action focus-window --id "$WID" >/dev/null; sleep 0.6
	sleep 0.5
}

start_injector() {
	rm -f /tmp/vks; mkfifo /tmp/vks
	./uikey < /tmp/vks >/dev/null 2>&1 & UI=$!
	exec 3>/tmp/vks
	sleep 0.8
}

ctrl_press_release_counts() {
	echo "presses $(grep -c 'sym: Control_L' "$DIR/wev.log") raw ctrl events; last state: $(grep -B1 'sym: Control_L' "$DIR/wev.log" | grep -o 'state: [01]' | tail -1)"
}

echo "=== A: physical release must end the hold ==="
start_wev; start_injector
./warpmod --grab-virtual --debug >/tmp/wmA2.log 2>&1 & WM=$!
sleep 1
echo "down Control_L" >&3; sleep 0.5     # hold ctrl -> app sees ctrl
echo "x" >&3; sleep 0.4                   # arm it too
echo "up Control_L" >&3; sleep 0.6        # release the real key
echo "   warpmod: $(grep -E 'modifier|armed' /tmp/wmA2.log | tail -3 | tr '\n' ' ')"
echo "   app: $(grep -B1 'sym: Control_L' "$DIR/wev.log" | grep -o 'state: [01]' | tr '\n' ' ')"
echo "Escape" >&3; sleep 0.8
kill $WM $UI $WEV 2>/dev/null; exec 3>&-; rm -f /tmp/vks; sleep 0.4

echo
echo "=== B: exiting with ctrl armed must release ctrl ==="
start_wev; start_injector
./warpmod --grab-virtual --debug >/tmp/wmB2.log 2>&1 & WM=$!
sleep 1
echo "x" >&3; sleep 0.4                    # armed, ctrl pressed
echo "Escape" >&3; sleep 1.2              # exit
echo "   warpmod alive: $(kill -0 $WM 2>/dev/null && echo yes || echo no)"
echo "   ctrl events in app (expect press then release): $(grep -B1 'sym: Control_L' "$DIR/wev.log" | grep -o 'state: [01]' | tr '\n' ' ')"
kill $UI $WEV 2>/dev/null; exec 3>&-; rm -f /tmp/vks; sleep 0.4

echo
echo "=== C: grabbed keyboard disappears ==="
start_wev; start_injector
./warpmod --grab-virtual --debug >/tmp/wmC2.log 2>&1 & WM=$!
sleep 1
echo "down Control_L" >&3; sleep 0.5       # ctrl held
kill $UI 2>/dev/null; exec 3>&-           # the only keyboard vanishes
sleep 2
echo "   warpmod alive: $(kill -0 $WM 2>/dev/null && echo yes || echo no)"
echo "   log: $(grep -E 'went away|no keyboard left' /tmp/wmC2.log | tr '\n' ' ')"
echo "   ctrl events in app (expect press then release): $(grep -B1 'sym: Control_L' "$DIR/wev.log" | grep -o 'state: [01]' | tr '\n' ' ')"
kill $WM $WEV 2>/dev/null; rm -f /tmp/vks
niri msg action fullscreen-window --id "$WID" >/dev/null 2>&1
