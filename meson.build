project(
    'libiap',
    ['c', 'cpp'],
    version: '1.0.0',
    default_options: [
        'warning_level=3',
        'werror=false',
        'cpp_std=c++23',
        'c_std=gnu11',
    ],
)
add_project_arguments('-Wfatal-errors', language: 'cpp')
add_project_arguments('-Wno-missing-field-initializers', language: 'cpp')
add_project_arguments('-Wno-variadic-macro-arguments-omitted', language: 'c')
add_project_arguments('-Wno-c99-extensions', language: 'c')


cpp = meson.get_compiler('cpp')
deps = [
    dependency('flac++'),
    dependency('Magick++'),
    cpp.find_library('asound'),
]

executable(
    'app',
    files(
        'src/artwork.cpp',
        'src/flac.cpp',
        'src/iap/debug.c',
        'src/iap/fid-token-values.c',
        'src/iap/hid.c',
        'src/iap/iap.c',
        'src/iap/notification.c',
        'src/iap/span.c',
        'src/iap/vector.c',
        'src/main.cpp',
        'src/platform.cpp',
        'src/track.cpp',
    ),
    dependencies: deps,
    install: true,
)

executable(
    'simulator',
    files(
        'src/simulator.cpp',
    ),
)
