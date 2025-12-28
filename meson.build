project(
    'wayfire-shader-dock',
    'cpp',
    version: '0.1.0',
    license: 'MIT',
    meson_version: '>=0.51.0',
    default_options: [
        'cpp_std=c++17',
        'warning_level=2',
        'werror=false',
    ],
)

wayfire = dependency('wayfire', version: '>=0.9.0')
wlroots = dependency('wlroots', version: '>=0.17.0')
wfconfig = dependency('wf-config', version: '>=0.9.0')
libpng = dependency('libpng')
glesv2 = dependency('glesv2')

add_project_arguments(['-DWLR_USE_UNSTABLE'], language: ['cpp'])

shared_module(
    'shader-dock',
    'shader-dock.cpp',
    dependencies: [wayfire, wlroots, wfconfig, libpng, glesv2],
    install: true,
    install_dir: wayfire.get_variable(pkgconfig: 'plugindir'),
)

# Install metadata
install_data(
    'metadata/shader-dock.xml',
    install_dir: wayfire.get_variable(pkgconfig: 'metadatadir'),
)
