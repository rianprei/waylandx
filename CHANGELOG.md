# Changelog

## v0.3.0 — a whole desktop

What KWin, Plasma and Xorg gave you that WaylandX did not have yet.

### X11 side
- **Sharp X11 apps on HiDPI.** Xwayland gets an X screen in device pixels of the largest monitor
  scale (its own xdg-output, hidden from other clients), X11 surfaces are shown at 1/scale and
  `Xft.dpi` is published, like KWin's "apply scaling themselves". Input, moves, resizes, work areas,
  touch, tablets and relative pointer motion are converted. `xwayland_scale = 1` keeps the old
  stretched mode. Changing monitors moves every X11 window into the new space live.
- **Clipboard ownership from background X11 apps.** An X11 client that takes CLIPBOARD or PRIMARY
  while a Wayland window has focus (the real `xclip -i`, clipboard managers, scripts) now sets the
  Wayland clipboard. Every type is copied into memory (INCR included), so the content outlives the
  app, which plain X11 never did.
- **Injected keys drive compositor shortcuts with held modifiers** (`keydown alt; key Tab`), like XTest.

### Desktop pieces (in waylandx-panel)
- **Notification server** (freedesktop 1.2): app icons, image-data, body markup, action buttons,
  default action on click, critical notifications stay, CloseNotification, replaces_id.
- **System tray**: StatusNotifierWatcher/host, icons from theme, pixmaps or IconThemePath,
  Activate/SecondaryActivate/Scroll, DBusMenu menus with submenus, checkmarks and radios.
- **Status items**: volume (PipeWire, live through `pactl subscribe`; click mutes, scroll changes,
  right click opens the mixer), network (NetworkManager, Wi-Fi strength), battery.
- **OSD** for volume and brightness; **Alt+Tab / Mod+Tab switcher** with icons in MRU order.
- **App icons** from the icon theme (KDE/GTK settings, inheritance, SVG through librsvg) in the
  task list, notifications, tray and switcher.

### Session
- **Idle**: lock after `idle_lock`, screens off after `idle_dpms`; visible idle inhibitors pause
  both; any input wakes the screens. A crashed locker is restarted by any key or `waylandxctl lock`.
- **logind**: lock before suspend (delay inhibitor released once the locker is on screen),
  `loginctl lock-session`, LockedHint, backlight through SetBrightness (no root).
- **Media keys** out of the box, also on the lock screen: volume, mic mute, brightness,
  play/pause/stop/next/previous (MPRIS). Ctrl+Alt+L locks.
- **Night light** on a schedule with 30 minute fades, in the hardware gamma ramps (renderer
  fallback on Vulkan); gammastep-style clients still win while they run.
- New IPC: `lock`, `screens`, `brightness`, `volume`, `media`, `nightlight`, `switch`.

### Fixes
- Screenshots keep every physical pixel on HiDPI outputs and undo output rotation.
- The Vulkan renderer no longer aborts at startup (color-management advertised the deprecated
  sRGB transfer function, which wlroots asserts on).
- Xwayland is no longer restarted while WaylandX shuts down (a stray Xwayland could outlive it).
- With Xwayland started at login (HiDPI), autostart waits for its window manager, so X11 apps
  started with the session are always managed.

### Tests
- Three new end-to-end suites: `run-hidpi.sh`, `run-session.sh` (mock logind + MPRIS on a private
  bus) and `run-desktop.sh` (notifications, OSD, Alt+Tab, tray, all on a private bus with fake
  `wpctl` and backlight). Unit test for the night light schedule.

## v0.2.0

- **Clipboard, X11-style:** X11 clients can read the clipboard and primary selection even while
  a Wayland window has focus (wlroots refuses this by default). Background X11 clipboard managers
  and plain `xclip -o` work again.
- **Tablet pads:** buttons, rings and strips on drawing tablets are delivered through tablet-v2,
  following keyboard focus.
- **Laptop lid:** closing the lid with an external monitor attached turns the built-in panel off
  and moves its windows; opening it brings the panel back. Configurable with `lid_close` and
  `internal_output`; `waylandxctl lid open|closed` triggers the same path (docking scripts).
- **Panel** survives monitors disappearing and coming back (verified through the lid path).

## v0.1.0 — first release

One display server that speaks Wayland and X11 at the same time.

### Wayland side (also benefits X11 apps)
- wlroots 0.20 scene graph: tear-free atomic KMS, damage tracking, dmabuf, explicit sync
  (drm-syncobj), presentation-time, color-management and color-representation.
- Per-output mode, fractional scale, transform, position and VRR; tearing allowed for
  fullscreen apps that ask for it (tearing-control) or by rule.
- Relative/locked pointer for games, pointer gestures, tablets (tablet-v2), touchscreens,
  input methods (text-input-v3 / input-method-v2), session lock, idle notify/inhibit,
  xdg-activation, xdg-foreign, output management/power, DRM leasing, and more.

### X11 side (also available to Wayland apps)
- Integrated rootless Xwayland; X11 windows land exactly where they ask, override-redirect
  windows stay unmanaged, EWMH states (above/below/sticky/fullscreen/maximized) are honoured.
- `waylandx-unstable-v1`: Wayland apps can set their own position, size, layer, stickiness.
- Prompt-free screenshots (`waylandxctl screenshot`, screencopy, ext-image-copy-capture).
- Input automation: type arbitrary Unicode, key combos, mouse moves/clicks/scroll.
- Global hotkeys for any program (`waylandxctl bind`).
- Shims for `xdotool`, `wmctrl`, `xclip`, `xsel` that work on Wayland and X11 windows.
- Remote apps with local windows: `waylandx-remote host app` (waypipe), plus `ssh -X`.

### Window manager
- Floating by default with server-side decorations, Mod+drag move/resize, edge snapping;
  optional master/stack tiling per workspace.
- Workspaces 1–9 per monitor; windows adopt the workspace of the monitor they move to.
- Window rules by app_id/class/title; INI config with live reload (bad files are rejected).
- `waylandx-panel`: one bar per monitor with workspaces, task list, clock and launcher.

### Tooling
- `waylandxctl` IPC client (JSON or plain output), 0600 per-user socket.
- Unit tests plus headless end-to-end suites driving real Wayland and X11 clients
  (single and dual monitor), also run under ASan/UBSan in CI.
