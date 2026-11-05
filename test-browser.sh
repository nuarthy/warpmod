#!/bin/bash
# warpmod + a real web page: does the page see ctrlKey on wheel, and does the
# browser zoom (devicePixelRatio changes)?
export WAYLAND_DISPLAY=wayland-1
DIR=/home/aru/.amp/in/clip-diag
WIN=12
MARK=$(wc -l < "$DIR/site-report.log")

niri msg action focus-window --id $WIN >/dev/null; sleep 0.5
niri msg action fullscreen-window --id $WIN >/dev/null; sleep 1

rm -f /tmp/vkcmd2; mkfifo /tmp/vkcmd2
./uikey < /tmp/vkcmd2 >/dev/null 2>&1 & UIKEY_PID=$!
exec 3>/tmp/vkcmd2
sleep 1.5
./warpmod --grab-virtual > /tmp/warpmod-browser.log 2>&1 & WM_PID=$!
sleep 1

echo "S-1" >&3; sleep 0.5
for i in $(seq 1 12); do echo "m" >&3; done
sleep 2
echo "S-1" >&3; sleep 0.5
for i in $(seq 1 6); do echo "m" >&3; done
sleep 2
echo "Escape" >&3; sleep 1

kill $WM_PID $UIKEY_PID 2>/dev/null
exec 3>&-
rm -f /tmp/vkcmd2
niri msg action fullscreen-window --id $WIN >/dev/null

echo "=== page reports ==="
tail -n +"$((MARK + 1))" "$DIR/site-report.log"
