# Changelog

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
