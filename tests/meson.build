test_config = executable('test_config', 'test_config.c', '../src/config.c',
  include_directories: include_directories('../src'))
test('config', test_config, args: [meson.project_source_root() / 'config/default.ini'])

test_json = executable('test_json', 'test_json.c', '../src/json.c',
  include_directories: include_directories('../src'))
test('json', test_json)

test_layout = executable('test_layout', 'test_layout.c', '../src/layout.c',
  include_directories: include_directories('../src'),
  c_args: ['-DWX_LAYOUT_TEST'],
  dependencies: [wlroots, wayland_server, xkbcommon])
test('layout', test_layout)

test_nightlight = executable('test_nightlight', 'test_nightlight.c', '../src/nightlight.c',
  include_directories: include_directories('../src'),
  dependencies: [wlroots, wayland_server, xkbcommon, pixman, math])
test('nightlight', test_nightlight)

e2e_helpers = []
e2e_helpers += executable('probe-wl', 'probe-wl.c',
  client_proto['xdg-shell'], client_proto['waylandx-unstable-v1'],
  client_proto['text-input-unstable-v3'], client_proto['idle-inhibit-unstable-v1'],
  dependencies: [wayland_client, xkbcommon])

e2e_helpers += executable('probe-im', 'probe-im.c', client_proto['input-method-unstable-v2'],
  dependencies: [wayland_client])

e2e_helpers += executable('probe-x11', 'probe-x11.c', dependencies: [xcb, xcb_icccm])

if sdbus.found()
  e2e_helpers += executable('mock-logind', 'mock-logind.c', dependencies: [sdbus])
endif

if sdbus.found()
  e2e_helpers += executable('mock-sni', 'mock-sni.c', dependencies: [sdbus])
endif
