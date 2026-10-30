test_config = executable('test_config', 'test_config.c', '../src/config.c',
  include_directories: include_directories('../src'))
test('config', test_config)

test_json = executable('test_json', 'test_json.c', '../src/json.c',
  include_directories: include_directories('../src'))
test('json', test_json)

test_layout = executable('test_layout', 'test_layout.c', '../src/layout.c',
  include_directories: include_directories('../src'),
  c_args: ['-DWX_LAYOUT_TEST'],
  dependencies: [wlroots, wayland_server, xkbcommon])
test('layout', test_layout)

executable('probe-wl', 'probe-wl.c',
  client_proto['xdg-shell'], client_proto['waylandx-unstable-v1'],
  client_proto['text-input-unstable-v3'],
  dependencies: [wayland_client, xkbcommon])

executable('probe-im', 'probe-im.c', client_proto['input-method-unstable-v2'],
  dependencies: [wayland_client])

executable('probe-x11', 'probe-x11.c', dependencies: [xcb, xcb_icccm])
