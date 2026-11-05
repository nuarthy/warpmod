# warpmod (prototype)

A keyboard-driven pointer for Wayland+niri that can really hold ctrl, shift,
alt and super for the focused application, instead of emulating warpd's
layer-surface keyboard grab.

## Why the old approach failed

warpd's Wayland backend grabs the keyboard with a layer surface
(`KEYBOARD_INTERACTIVITY_EXCLUSIVE`). The compositor then sends key events to
warpd, so the focused application receives none, and a modifier pressed through
`zwp_virtual_keyboard_v1` is delivered to warpd itself. Holding ctrl for an
application is impossible in that design unless keyboard focus is handed to the
application for every pointer action, and that handover leaks keys: anything
typed during the handover (including a scroll key's release) goes to the
application instead of warpd.

## What this does instead

```
  physical keyboard ──(EVIOCGRAB)──> warpmod ──> uinput keyboard: ctrl/shift/alt/super
        │                              │
        └─ compositor sees nothing     ├──> uinput mouse: buttons + hi-res wheel
                                       └──> zwlr_virtual_pointer: absolute motion
                                                    │
                                       compositor ──┴──> focused application
```

* Input is grabbed at the kernel level (`/dev/input/event*`, `EVIOCGRAB`). The
  compositor never sees those keys, and, unlike a layer-surface grab, the
  compositor's focus does not change: the window under the pointer keeps the
  keyboard and keeps its modifier state.
* Modifiers are pressed on a uinput virtual keyboard. That changes the seat's
  modifier state, so the application learns about it from its own `wl_keyboard`
  and sees `ctrl+wheel` as a normal modifier. No focus handoff is needed.
* Motion uses `zwlr_virtual_pointer_v1.motion_absolute`, so the position is
  exact. Buttons and wheel use a uinput virtual mouse, which gives events
  kernel timestamps (fixing context menus ignoring warpd's clicks, which was
  the reason menu items did not respond) and proper hi-res wheel resolution.
* The mapping comes from the outputs the compositor reports (position, size and
  scale, over xdg-output where available) and is rebuilt whenever they change,
  so no monitor arrangement, monitor swap or scale is configured anywhere.

Modifier semantics follow ZMK:

| ZMK behaviour | here |
| --- | --- |
| `&kt` key toggle | press the modifier key again to release it; the modifier stays down until then |
| `&sk` sticky key | not implemented yet (one-shot: release after the next pointer action) |
| `sk` `lazy` | super is lazy: it is only pressed for the duration of an action, because pressing super alone opens the compositor overview |

## Fixed after the first try

* `exit: esc`, `scroll_up: ,` and `scroll_right: .` never matched: warpd's names
  are not keysym names. A name alias table now maps `esc`/`,`, `.`, `-`, `/`,
  `;`, `$` and `backspace` (`warpmod --check-config` reports unresolved keys).
* Esc always exits, and SIGINT/SIGTERM/SIGHUP/SIGQUIT clean up and release the
  grab, so closing the terminal is a safe way out.
* Buttons never worked: the virtual mouse advertised keyboard codes 1-255 but no
  `BTN_LEFT`/`BTN_MIDDLE`/`BTN_RIGHT`, so the kernel dropped every click.
* Scroll ran the wrong way: `REL_WHEEL` is positive upwards, the opposite of
  `wl_pointer.axis`; "scroll up" now sends the positive value.
* Start-up was half a second and the pointer jumped to the middle of the layout.
  The position is now discovered like warpd does it (a transparent layer surface
  per output, read the pointer enter, remove it again), and keyboards are found
  by parsing `/proc/bus/input/devices` instead of probing all 30 event nodes
  (about 20 ms per probe). Start to usable is now ~110 ms.
* Bluetooth keyboards were not grabbed. The "skip virtual devices" test rejected
  anything under `/devices/virtual/`, and Bluetooth HID keyboards live under
  `/devices/virtual/misc/uhid/` even though they are real hardware. It now
  excludes only `/devices/virtual/input/`, where uinput devices appear. A run
  grabs every keyboard it can see (internal, plus each connected Bluetooth one).
* Multi-monitor discovery: probing all outputs at once made the enter arrive on
  the wrong surface, so every position was offset by a whole monitor. Outputs are
  now probed one at a time. Seven known positions across eDP-1 and HDMI-A-1
  all come back exact.
* The discovered position was 40 px too high: the probe surface was pushed down
  by the exclusive zone of the DMS bar (top layer). It now sets
  `exclusive_zone = -1` and anchors to all four edges, so the surface sits at
  the real output origin and the enter coordinates are absolute. Verified
  against five known positions, all exact, and a session reports the pointer
  where it was left.
* Activating on the right monitor moved the pointer to the left one after the
  monitors were swapped. Absolute motion is a fraction of the layout bounding
  box and the compositor adds the box origin back, but the origin was never
  subtracted: as soon as one output sat at a negative coordinate, every position
  was off by a whole monitor (or clamped to the left edge). `pointer_move()` now
  converts layout coordinates into box coordinates. `test-map.sh` covers it, and
  fails on the old mapping with the exact symptom (`960,600` lands at
  `-960,600`).
* The layout is taken from xdg-output (logical position and size) instead of
  wl_output's mode and integer scale, so a fractionally scaled output is mapped
  correctly; at scale 1.25 the mode-and-scale guess was 576x360 px too small.
  Positions round-trip exactly at scale 1 and 1.25.
* The mapping follows the outputs while the session runs, not only at
  activation: moving a monitor with `niri msg output ... position`, turning one
  off, or scaling it re-reads the layout from the wl_output/xdg-output events
  and logs `layout now ...`. The pointer keeps its layout position; only one
  left outside the new layout is pulled back in.

## One session at a time

Two instances each grab a keyboard and each move the pointer towards their own
idea of where it is; the result looks like a pointer that barely moves, goes the
wrong way, and misbehaves on clicks. The tool now takes an exclusive lock, so a
second launch refuses with `another instance is already running`.

```
warpmod --stop        # ask the running session to end (it releases everything)
warpmod --version     # build stamp, to confirm which binary is running
```

Each session writes a small log to `~/.local/state/warpmod/log`: version, number
of outputs and keyboards, every arm/disarm with the pointer position, and the
exit position.

## Launch and exit

* The session is started with `Mod+M`, so Super is still down when a grab would
  begin, and grabbing then swallows its release: the compositor keeps Super held
  and treats everything afterwards as Super+... . The keyboards are therefore
  taken 300 ms after launch, which is enough for the chord to be pressed *and*
  released normally.
* Esc is handled before any binding logic and before anything else in the input
  path, so it ends the session even while ctrl is held, a drag is active, or
  other keys are down.
* On exit, all four modifiers and the mouse buttons are released explicitly, and
  the tool waits 200 ms before the virtual devices disappear, so the releases
  reach the application.

## Modifier safety

A modifier must never survive the session, and the session must never become
impossible to end:

* on exit, modifiers and buttons are released, then the tool waits 200 ms before
  destroying the virtual devices, so the releases actually reach the application
  (destroying a device with keys down relies on the compositor synthesising
  releases, which is not guaranteed);
* releasing the real modifier key also clears a hold armed with a toggle key, so
  a modifier cannot stay down after the key that produced it is released;
* wanted and held modifiers are re-synced every tick, so a lost event cannot
  leave one pressed;
* a keyboard that disappears (Bluetooth drop) is dropped with its modifier
  state, keyboards are rescanned every second, and the session exits if none are
  left instead of sitting there deaf;
* `SIGINT`/`SIGTERM`/`SIGHUP`/`SIGQUIT` all run the same cleanup, and `alarm(2)`
  guards it: if cleanup ever blocks, the process dies and the kernel frees the
  grab and the keys.

Verified: holding ctrl then arming with `x` and releasing ctrl makes the
application see press then release; exiting with ctrl armed does the same;
killing the grabbed keyboard produces `device 0 went away` and a release.

## Verified

With `wev` as the observed application (a normal xdg-shell client):

* the application received `Control_L` pressed, then 72 `wl_pointer.axis` events
  (`axis_value120` ≈ -39 each), then `Control_L` released;
* the application received exactly one `wl_keyboard.enter` for the whole
  session: it kept focus throughout, so there is no key loss at all;
* a click produced `button: 272 (left) pressed/released` with kernel timestamps;
* `,` produced axis ≈ -39 (up) and `m` axis ≈ +39 (down);
* `Esc` ends the session, and `SIGHUP`/`SIGTERM` release the keyboard;
* wheel values and pacing match a real hi-res mouse (~125 events/s, ~1/3 detent
  per event).

## Install and niri bind

```
make install         # ~/.local/bin/warpmod
```

The niri bind in `~/.config/niri/dms/binds.kdl` now starts it:

```
Mod+M       -> warpmod --normal     (was warpd --normal)
Mod+Shift+M -> warpd --hint         (unchanged; warpmod has no hint mode yet)
```

## Usage

```
warpmod              # reads ~/.config/warpd/config, Esc exits
warpmod --probe-once # print where the pointer is, in layout coordinates
warpmod --move X Y   # put the pointer there, in layout coordinates
```

Keys come from the existing warpd config, so the current bindings apply
(`k j h l` movement, `m n , .` scroll, `f r d` buttons, `g` drag, `s` precision).

The real modifier keys work by simply holding them: ctrl, shift, alt and super
are mirrored to the application, so ctrl+scroll is just ctrl held while
scrolling. For one-handed use `x` toggles ctrl, `v` shift, `c` alt and `z`
super (press again to release; combine them for ctrl+shift). Rebind with e.g.
`mod_ctrl: q` in the config.

Requirements: membership of group `input` (for `/dev/input/event*` and
`/dev/uinput`). While it runs, the keyboard is captured, so compositor
shortcuts do not fire; that is the same as warpd's normal mode.

## Not done yet

* hint/grid modes and their drawing (needs the overlay layer surface + cairo
  part of warpd's Wayland backend)
* on-screen indication that a modifier is armed (warpid's cursor box is off in
  this config)
* if discovery fails, the last saved position is reused (then the centre)
* `scroll_acceleration`, sticky (`&sk`) modifiers
* clipboard-related X11 bridging is unchanged and unrelated

## Files

* `warpmod.c` main tool
* `uikey.c`, `uiwheel.c` uinput injectors used by the tests
* `test-wev.sh` end-to-end test with `wev` as the observed application
* `test-map.sh` every output must round-trip the pointer position exactly
* `virtual-pointer.c/.h`, `layer-shell.c/.h`, `xdg-shell.c/.h`, `xdg-output.c/.h`
  generated protocol code
