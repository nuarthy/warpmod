#!/bin/bash
# Mapping check: the pointer must land where warpmod says, on every output.
#
# Absolute motion is a fraction of the compositor's layout bounding box, so the
# tool has to subtract the box origin. When that was missing, every position was
# off by a whole monitor as soon as one output sat at a negative coordinate:
# activating on the right monitor moved the pointer to the left one.
#
# --move and the session both go through pointer_move(), so this checks the
# mapping without grabbing a keyboard. The mouse is moved, so do not touch it
# while the test runs; a moved mouse is retried instead of reported.
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-1}
cd "$(dirname "$0")" || exit 1

fail=0

check() {   # check <label> <x> <y>
	for attempt in 1 2 3; do
		./warpmod --move "$2" "$3"
		sleep 0.6
		got=$(./warpmod --probe-once)
		[ "$got" = "$2 $3" ] && break
	done

	if [ "$got" = "$2 $3" ]; then
		echo "ok   $1: $2,$3"
	else
		echo "FAIL $1: sent $2,$3, pointer is at $got"
		fail=1
	fi
}

# Centre of every output, and a point near its top left corner.
python3 - <<'PY' > /tmp/warpmod-map-points
import json, subprocess
outs = json.loads(subprocess.check_output(["niri", "msg", "-j", "outputs"]))
for name, out in sorted(outs.items(), key=lambda kv: kv[1]["logical"]["x"]):
    l = out["logical"]
    if l["width"] <= 0 or l["height"] <= 0:
        continue
    print(f"{name} centre|{l['x'] + l['width'] // 2}|{l['y'] + l['height'] // 2}")
    print(f"{name} corner|{l['x'] + 100}|{l['y'] + 100}")
PY

while IFS='|' read -r name x y; do
	check "$name" "$x" "$y"
done < /tmp/warpmod-map-points

echo
if [ "$fail" = 0 ]; then
	echo "RESULT: the pointer lands exactly on every output"
else
	echo "RESULT: mapping is wrong, see the FAIL lines"
fi
exit "$fail"
