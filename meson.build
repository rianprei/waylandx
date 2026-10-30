project('waylandx', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=0.60.0',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=false', 'buildtype=debugoptimized'])

add_project_arguments(
  '-DWLR_USE_UNSTABLE',
  '-D_POSIX_C_SOURCE=200809L',
  '-D_DEFAULT_SOURCE',
  '-DWX_VERSION="@0@"'.format(meson.project_version()),
  '-DWX_DATADIR="@0@"'.format(get_option('prefix') / get_option('datadir')),
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  language: 'c')

wlroots = dependency('wlroots-0.20')
wayland_server = dependency('wayland-server')
wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols')
xkbcommon = dependency('xkbcommon')
libinput = dependency('libinput')
pixman = dependency('pixman-1')
xcb = dependency('xcb')
xcb_icccm = dependency('xcb-icccm')
cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
math = meson.get_compiler('c').find_library('m')

wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')
scanner = find_program(dependency('wayland-scanner', native: true).get_variable('wayland_scanner'))

protocols = {
  'xdg-shell': wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  'ext-workspace-v1': wl_protocol_dir / 'staging/ext-workspace/ext-workspace-v1.xml',
  'wlr-layer-shell-unstable-v1': 'protocol/wlr-layer-shell-unstable-v1.xml',
  'wlr-output-power-management-unstable-v1': 'protocol/wlr-output-power-management-unstable-v1.xml',
  'wlr-foreign-toplevel-management-unstable-v1': 'protocol/wlr-foreign-toplevel-management-unstable-v1.xml',
  'waylandx-unstable-v1': 'protocol/waylandx-unstable-v1.xml',
  'text-input-unstable-v3': wl_protocol_dir / 'unstable/text-input/text-input-unstable-v3.xml',
  'input-method-unstable-v2': 'protocol/input-method-unstable-v2.xml',
}

server_proto = {}
client_proto = {}
foreach name, xml : protocols
  code = custom_target(name + '-protocol.c', input: xml, output: name + '-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  sh = custom_target(name + '-protocol.h', input: xml, output: name + '-protocol.h',
    command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  ch = custom_target(name + '-client-protocol.h', input: xml, output: name + '-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  server_proto += {name: [code, sh]}
  client_proto += {name: [code, ch]}
endforeach

server_srcs = files(
  'src/main.c', 'src/output.c', 'src/input.c', 'src/seat.c', 'src/xdg.c',
  'src/xwayland.c', 'src/layer.c', 'src/view.c', 'src/deco.c', 'src/layout.c',
  'src/workspace.c', 'src/config.c', 'src/ipc.c', 'src/ipc_cmd.c',
  'src/inject.c', 'src/capture.c', 'src/wxproto.c', 'src/rules.c', 'src/json.c',
  'src/protocols.c', 'src/lock.c', 'src/ime.c')

executable('waylandx', server_srcs,
  server_proto['xdg-shell'], server_proto['wlr-layer-shell-unstable-v1'],
  server_proto['wlr-output-power-management-unstable-v1'],
  server_proto['waylandx-unstable-v1'],
  include_directories: include_directories('src'),
  dependencies: [wlroots, wayland_server, xkbcommon, libinput, pixman, xcb, xcb_icccm,
    cairo, pangocairo, math],
  install: true)

executable('waylandxctl', 'ctl/waylandxctl.c', install: true)

executable('waylandx-panel', 'panel/panel.c', 'panel/launcher.c',
  client_proto['xdg-shell'], client_proto['wlr-layer-shell-unstable-v1'],
  client_proto['wlr-foreign-toplevel-management-unstable-v1'],
  client_proto['ext-workspace-v1'],
  dependencies: [wayland_client, xkbcommon, cairo, pangocairo],
  install: true)

install_data('session/waylandx-remote', 'session/waylandx-session',
  install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
install_data('shims/xdotool', 'shims/wmctrl', 'shims/xclip', 'shims/xsel',
  install_dir: get_option('datadir') / 'waylandx/shims', install_mode: 'rwxr-xr-x')
install_data('config/default.ini', install_dir: get_option('datadir') / 'waylandx')

subdir('tests')

foreach e2e : ['headless', 'multihead']
  test(e2e + '-e2e', find_program('tests/run-' + e2e + '.sh'),
    args: [meson.project_build_root()],
    timeout: 300,
    is_parallel: false)
endforeach
