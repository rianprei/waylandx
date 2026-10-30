# WaylandX

[![CI](https://github.com/rianprei/waylandx/actions/workflows/ci.yml/badge.svg)](https://github.com/rianprei/waylandx/actions/workflows/ci.yml)

*[Leia em português](README.pt-BR.md)*

A single display server that speaks **Wayland and X11 at the same time** and fuses the best of both:
Wayland's modern, tear-free pipeline with the freedoms X11 always gave you — windows that place
themselves, screenshots without prompts, input automation, xdotool/wmctrl-style tools, network
transparency.

Written in C11 on top of wlroots 0.20. X11 apps run through a rootless Xwayland managed by WaylandX itself.

## What comes from each side

| From Wayland (now also for X11 apps) | How |
|---|---|
| Tear-free atomic KMS, damage tracking | wlroots scene graph |
| Per-monitor VRR | `[output:NAME] vrr = on` |
| Tearing allowed for fullscreen games | tearing-control protocol or rule `allow_tearing = true` |
| Fractional scaling per monitor | `scale = 1.25` + fractional-scale-v1 |
| dmabuf, explicit sync, color management, presentation-time | enabled when the GPU supports them |
| Relative/locked pointer (games), touchpad gestures | relative-pointer, pointer-constraints, pointer-gestures |
| Drawing tablets, touchscreens, input methods (IME) | tablet-v2, wl_touch, text-input-v3 + input-method-v2 |
| Secure screen lock | ext-session-lock (swaylock etc.); a crashed locker keeps the screen locked |

| From X11 (now also for Wayland apps) | How |
|---|---|
| Apps choose their own position | `waylandx-unstable-v1` protocol (Wayland) and configure requests (X11) |
| Always on top / below / sticky | `_NET_WM_STATE` (X11), our protocol (Wayland), `waylandxctl above` |
| Screenshots without a dialog | `waylandxctl screenshot`, screencopy, ext-image-copy-capture, portal without chooser |
| Input automation (xdotool) | `waylandxctl type/key/click/mousemove`, open virtual keyboard/pointer, `xdotool` shim |
| Introspection and control (wmctrl/xprop) | `waylandxctl list/info/move/close…`, `wmctrl` shim, foreign-toplevel |
| Global hotkeys for any program | `waylandxctl bind COMBO CMD` |
| Clipboard reachable from anywhere | data-control (wl-clipboard) + `xclip`/`xsel` shims |
| Remote app, local window (`ssh -X`) | `waylandx-remote host app` (waypipe), `ssh -X` through Xwayland |
| Classic WM: floating, title bars, Mod+drag | server-side decorations, Mod+drag, snapping; optional tiling |
| Open policy | any local client may capture and inject; only the IPC socket is restricted to your user (0600) |

## Build and test

Dependencies (Arch): `wlroots0.20 wayland wayland-protocols libxkbcommon libinput pixman libxcb xcb-util-wm cairo pango xorg-xwayland meson ninja`.
For the end-to-end tests and extras: `xclip wl-clipboard waypipe file`, optionally `swaylock xdotool wmctrl xdg-desktop-portal-wlr`.

```sh
meson setup build --prefix="$HOME/.local"
ninja -C build
meson test -C build
```

`meson test` runs unit tests plus two headless end-to-end suites (`tests/run-headless.sh`,
`tests/run-multihead.sh`) that start WaylandX without a screen and drive real Wayland and X11
clients: input injection, IME, screenshots, rules, shims, clipboard, workspaces on two monitors,
tiling, panel, waypipe, session lock and live config reload. CI runs them normally and under ASan/UBSan.

## Run

**Inside your current desktop** (opens a window; the modifier switches to Alt automatically):

```sh
./build/waylandx
```

**As its own session** (TTY or login screen):

```sh
meson install -C build                                   # installs to ~/.local
sudo cp session/waylandx.desktop /usr/share/wayland-sessions/
sudo sed -i "s|^Exec=.*|Exec=$HOME/.local/bin/waylandx-session|" /usr/share/wayland-sessions/waylandx.desktop
mkdir -p ~/.config/waylandx ~/.config/xdg-desktop-portal ~/.config/xdg-desktop-portal-wlr
cp config/default.ini ~/.config/waylandx/config.ini
cp session/WaylandX-portals.conf ~/.config/xdg-desktop-portal/
cp session/xdg-desktop-portal-wlr.conf ~/.config/xdg-desktop-portal-wlr/config
```

Then pick "WaylandX" in your display manager, or run `waylandx-session` from a TTY.

## Default key bindings

`Mod` = Super (Alt when nested). Everything is configurable under `[keybind]`.

| Binding | Action |
|---|---|
| Mod+Enter | terminal |
| Mod+D | panel launcher |
| Mod+Q / Mod+Shift+Q | close / kill |
| Mod+F, Mod+M, Mod+N | fullscreen, maximize, minimize |
| Mod+Space | toggle floating for the window |
| Mod+T | toggle tiling on this workspace |
| Mod+A / Mod+S | always on top / on all workspaces |
| Mod+Tab | cycle windows |
| Mod+←/→, Mod+↑/↓ | half screen, maximize/restore |
| Mod+1..9, Mod+Shift+1..9 | switch workspace on this monitor / move window |
| Mod+H / Mod+L | shrink / grow the master area |
| Print / Mod+Shift+S | screenshot of the screen / of the window |
| Mod+Shift+R / Mod+Shift+E | reload config / exit |
| Mod+drag (left/right) | move / resize from anywhere in the window |

## waylandxctl

```sh
waylandxctl list [--plain]                 # every window (Wayland and X11)
waylandxctl info -w 3                      # id, app_id/class, pid, x11_window, geometry, state
waylandxctl move -w 3 100 200              # also resize, geometry, close, kill, focus
waylandxctl above on -w focused            # maximize|fullscreen|minimize|above|below|sticky|floating
waylandxctl workspace 2 -o DP-1            # workspaces are per monitor
waylandxctl type "héllo wörld"             # any Unicode text, independent of the keyboard layout
waylandxctl key ctrl+shift+t Return        # keydown/keyup too
waylandxctl mousemove 500 300; waylandxctl click 1 2   # X11 button numbers (4/5 = scroll)
waylandxctl screenshot ~/shot.png [-o DP-1 | -w 3 | -r 0,0,800,600]
waylandxctl bind super+F1 notify-send hi   # global hotkey running a command
waylandxctl subscribe                      # JSON event stream
waylandxctl help
```

The socket protocol is plain text: one command per line, the reply is JSON ended by an empty line.

## X11 tool shims

Inside the session, `share/waylandx/shims` is put in front of `PATH`:

- `xdotool`: type, key, keydown/up, mousemove(_relative), click, search (with chaining), getactivewindow,
  windowactivate/move/size/close/kill/minimize, getwindowname/pid/geometry, desktops.
- `wmctrl`: `-l [-p -G -x]`, `-a`, `-c`, `-r … -e g,x,y,w,h`, `-r … -b add|remove|toggle,above|sticky|…`, `-s`, `-t`, `-d`.
- `xclip`/`xsel`: read and write the clipboard from anywhere.

They work on Wayland **and** X11 windows. Anything not covered falls through to the real binary.

## The `waylandx-unstable-v1` protocol

`protocol/waylandx-unstable-v1.xml` lets a Wayland toplevel do what X11 windows always could:
`set_position`, `set_size`, `set_layer(below|normal|above)`, `set_sticky`, `set_skip_taskbar`,
and receive `position` events. See `tests/probe-wl.c` for a minimal client.

## Configuration

See `config/default.ini`. Sections: `[general]`, `[input]`, `[output:NAME]`, `[keybind]`, `[rule:NAME]`, `[autostart]`.
Reload live with `waylandxctl reload`, Mod+Shift+R or SIGHUP. A file with errors is rejected as a whole and the previous configuration stays active.

## Layout

```
src/main.c        setup, signals, spawn, reload
src/output.c      monitors, mode/scale/VRR, tearing, output management, background
src/input.c       keyboard, pointer, touch, tablets, bindings, move/resize/snap, gestures
src/ime.c         input method relay (text-input-v3 <-> input-method-v2)
src/seat.c        focus, clipboard, drag and drop, cursor
src/view.c        unified window (xdg + X11): states, layers, placement, monitor tracking
src/xdg.c         xdg-shell, popups, decoration negotiation
src/xwayland.c    Xwayland: requested positions, override-redirect, EWMH
src/deco.c        server-side decorations (cairo/pango)
src/layer.c       layer-shell (panels, wallpapers), usable area
src/layout.c      master/stack tiling
src/workspace.c   per-monitor workspaces + ext-workspace
src/rules.c       window rules
src/config.c      INI parser
src/ipc*.c        IPC socket and commands
src/inject.c      keyboard/mouse injection (keymap generated on the fly)
src/capture.c     screenshots
src/wxproto.c     waylandx-unstable-v1
src/protocols.c   the remaining Wayland protocols
src/lock.c        ext-session-lock
panel/            waylandx-panel (bars + launcher)
ctl/              waylandxctl
shims/            xdotool, wmctrl, xclip, xsel
session/          waylandx-session, waylandx-remote, .desktop, portal configs
tests/            unit tests, Wayland/X11/IME probes, headless end-to-end suites
```

## Known limitations

- **X11 apps on monitors with scale > 1** are upscaled (blurry); wlroots 0.20 has no Xwayland scaling API.
- **Unfocused X11 clipboard access:** wlroots only lets X11 clients read/write the clipboard while an X11
  window has focus. Focused X11 apps work normally and scripts use the shims; a background X11 clipboard
  manager would need a wlroots patch.
- **Tablet pads** (buttons/rings/strips on the tablet itself) and laptop lid switches are not routed yet.
- **Not tested on real hardware so far:** TTY/DRM session, VRR and tearing on a physical monitor, physical
  multi-monitor, touchscreens and drawing tablets. The test suites run headless and nested; touch and tablet
  code paths are compiled but have not been exercised by a real device.

## License

MIT — see [LICENSE](LICENSE).
