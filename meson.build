project('xfce4-claude-usage-plugin', 'c',
  version: '0.1.0',
  license: 'GPL-2.0-or-later',
  meson_version: '>= 0.60',
  default_options: ['warning_level=2', 'c_std=gnu11'])

add_project_arguments('-Wno-unused-parameter', language: 'c')

i18n = import('i18n')
gettext_package = meson.project_name()
add_project_arguments(
  '-DGETTEXT_PACKAGE="@0@"'.format(gettext_package),
  '-DPACKAGE_LOCALE_DIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
  language: 'c')
subdir('po')

glib = dependency('glib-2.0')
gio = dependency('gio-2.0')
json = dependency('json-glib-1.0')
soup = dependency('libsoup-3.0')
m = meson.get_compiler('c').find_library('m')
gtk = dependency('gtk+-3.0')

core_sources = files('src/pricing.c', 'src/usage-api.c', 'src/local-stats.c')

core_lib = static_library('claude-core', core_sources,
  dependencies: [glib, gio, json, soup, m],
  pic: true)
core_dep = declare_dependency(link_with: core_lib,
  include_directories: include_directories('src'),
  dependencies: [glib, gio, json, soup, m])

test('pricing', executable('test-pricing', 'tests/test-pricing.c', dependencies: core_dep))
test('usage-api', executable('test-usage-api', 'tests/test-usage-api.c',
  dependencies: core_dep,
  c_args: '-DFIXTURE_DIR="@0@"'.format(meson.current_source_dir() / 'tests' / 'fixtures')))
test('local-stats', executable('test-local-stats', 'tests/test-local-stats.c', dependencies: core_dep))

giounix = dependency('gio-unix-2.0')
launcher_lib = static_library('claude-launcher', 'src/launcher.c', dependencies: [gio, giounix], pic: true)
launcher_dep = declare_dependency(link_with: launcher_lib,
  include_directories: include_directories('src'),
  dependencies: [gio, giounix])

test('launcher', executable('test-launcher', 'tests/test-launcher.c', dependencies: launcher_dep))

bars_lib = static_library('claude-bars', 'src/bars.c', dependencies: [gtk, m], pic: true)
bars_dep = declare_dependency(link_with: bars_lib,
  include_directories: include_directories('src'),
  dependencies: [gtk, m])

test('bars', executable('test-bars', 'tests/test-bars.c', dependencies: bars_dep))

panel = dependency('libxfce4panel-2.0')
xfui = dependency('libxfce4ui-2')
xfutil = dependency('libxfce4util-1.0')

shared_module('claude-usage', 'src/plugin.c',
  dependencies: [core_dep, bars_dep, launcher_dep, panel, xfui, xfutil],
  install: true,
  install_dir: get_option('libdir') / 'xfce4' / 'panel' / 'plugins')

i18n.merge_file(
  input: 'data/claude-usage.desktop.in',
  output: 'claude-usage.desktop',
  type: 'desktop',
  po_dir: 'po',
  install: true,
  install_dir: get_option('datadir') / 'xfce4' / 'panel' / 'plugins')

install_data('data/icons/scalable/xfce4-claude-usage-plugin.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')

gnome = import('gnome')
gnome.post_install(gtk_update_icon_cache: true)
