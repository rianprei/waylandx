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
| Sharp X11 apps on HiDPI | X11 renders at the largest monitor scale with `Xft.dpi` set, like KWin |

| From X11 (now also for Wayland apps) | How |
|---|---|
| Apps choose their own position | `waylandx-unstable-v1` protocol (Wayland) and configure requests (X11) |
| Always on top / below / sticky | `_NET_WM_STATE` (X11), our protocol (Wayland), `waylandxctl above` |
| Screenshots without a dialog | `waylandxctl screenshot`, screencopy, ext-image-copy-capture, portal without chooser |
| Input automation (xdotool) | `waylandxctl type/key/click/mousemove`, open virtual keyboard/pointer, `xdotool` shim |
| Introspection and control (wmctrl/xprop) | `waylandxctl list/info/move/close…`, `wmctrl` shim, foreign-toplevel |
| Global hotkeys for any program | `waylandxctl bind COMBO CMD` |
| Clipboard reachable from anywhere | background X11 apps read **and set** it (the content even outlives the app); data-control (wl-clipboard); `xclip`/`xsel` shims |
| Remote app, local window (`ssh -X`) | `waylandx-remote host app` (waypipe), `ssh -X` through Xwayland |
| Classic WM: floating, title bars, Mod+drag | server-side decorations, Mod+drag, snapping; optional tiling |
| Laptop lid / docking | lid closes → built-in panel off when an external monitor is attached |
| Open policy | any local client may capture and inject; only the IPC socket is restricted to your user (0600) |
| Injected keys trigger WM shortcuts (XTest) | `waylandxctl keydown alt; key Tab` drives Alt+Tab like `xdotool` did on X11 |

| What a full desktop (KDE/GNOME) gives you | How |
|---|---|
| Notifications | `waylandx-panel` is the freedesktop notification server: icons, markup, actions, critical stays |
| System tray | StatusNotifierItem + DBusMenu menus (Discord, Steam, KDE Connect, nm-applet…) |
| Volume, network, battery in the bar | PipeWire (`wpctl`), NetworkManager, sysfs; click/scroll to change |
| Volume and brightness OSD, media keys | work out of the box, also on the lock screen; brightness through logind (no root) |
| Alt+Tab switcher | hold Alt (or Mod), Tab through windows in MRU order, release to switch |
| Lock and screen off when idle | built in; video players and games that inhibit idle pause it |
| Lock before suspend, `loginctl lock-session` | logind delay inhibitor, released once the locker is on screen |
| Night light | `night_light = 20:00-07:00`, 30 minute fades, hardware gamma ramps |
| App icons | in the task list, notifications, tray and switcher, from your icon theme |

## Build and test

Dependencies (Arch): `wlroots0.20 wayland wayland-protocols libxkbcommon libinput pixman libxcb xcb-util-wm cairo pango xorg-xwayland meson ninja`.
For the end-to-end tests and extras: `xclip wl-clipboard waypipe file`, optionally `swaylock xdotool wmctrl xdg-desktop-portal-wlr`.

```sh
meson setup build --prefix="$HOME/.local"
ninja -C build
meson test -C build
```

`meson test` runs unit tests plus five headless end-to-end suites that start WaylandX without a
screen and drive real Wayland and X11 clients:

- `run-headless.sh`: input injection, IME, screenshots, rules, shims, clipboard both ways, tiling,
  panel, waypipe, idle and session lock, live config reload;
- `run-multihead.sh`: per-monitor workspaces, hotplug, laptop lid;
- `run-hidpi.sh`: sharp X11 on a scale-2 monitor, input mapping, native-resolution screenshots;
- `run-session.sh`: lock before suspend, lock-session, brightness and MPRIS against a mock logind;
- `run-desktop.sh`: notifications, OSD, Alt+Tab, tray items and their menus on a private D-Bus.

CI runs them normally and under ASan/UBSan. Nothing touches your real session: the D-Bus suites use
a private bus, fake `wpctl` and a fake backlight.

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
| Alt+Tab / Mod+Tab (+Shift) | window switcher: hold, Tab through, release |
| Mod+←/→, Mod+↑/↓ | half screen, maximize/restore |
| Mod+1..9, Mod+Shift+1..9 | switch workspace on this monitor / move window |
| Mod+H / Mod+L | shrink / grow the master area |
| Print / Mod+Shift+S | screenshot of the screen / of the window |
| Mod+Shift+R / Mod+Shift+E | reload config / exit |
| Ctrl+Alt+L | lock the screen |
| Volume, brightness, play/pause/next/prev keys | built in, with OSD |
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
waylandxctl lock; waylandxctl screens off  # lock now / screens off until input
waylandxctl brightness +5%                 # volume up|down|mute, media play-pause|next…
waylandxctl nightlight on                  # on|off|auto (auto follows night_light)
waylandxctl switch next                    # Alt+Tab from scripts
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
src/x11clip.c     X11 clipboard both ways without focus
src/xwlscale.c    sharp X11 on HiDPI (scaled X screen, xdg-output for Xwayland, Xft.dpi)
src/lock.c        ext-session-lock
src/idle.c        lock and screens off when idle
src/logind.c      lock before suspend, lock-session, brightness, MPRIS
src/nightlight.c  night light
src/switcher.c    Alt+Tab
panel/            waylandx-panel: bars, launcher, notifications, tray, status, OSD, switcher
ctl/              waylandxctl
shims/            xdotool, wmctrl, xclip, xsel
session/          waylandx-session, waylandx-remote, .desktop, portal configs
tests/            unit tests, Wayland/X11/IME probes, headless end-to-end suites
```

## Known limitations

- **X11 apps that ignore `Xft.dpi`** (old Xlib/Motif apps) look small on HiDPI with the default
  `xwayland_scale = auto`; set `xwayland_scale = 1` to stretch them instead. X11 cursors are drawn at
  the right size but not at full resolution.
- **Night light** needs hardware gamma ramps (any real monitor on DRM) or the Vulkan renderer; nested
  sessions on the GLES2 renderer get no tint.
- **Tray menus** are mouse-driven (no keyboard navigation yet); submenus open in place of their parent.
- **Notifications** have no history panel and no inline images in the body.
- **Tablet pads** pair with the most recently connected tablet (fine for one tablet; several tablets
  at once are not distinguished).
- **Not tested on real hardware so far:** TTY/DRM session, VRR and tearing on a physical monitor, physical
  multi-monitor, touchscreens and drawing tablets. The test suites run headless and nested; touch and tablet
  code paths are compiled but have not been exercised by a real device.

## License

MIT — see [LICENSE](LICENSE).
