project('gst-template', 'c', version : '1.19.0.1', license : 'LGPL')

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')

cc = meson.get_compiler('c')

gst_version = meson.project_version()

api_version = '1.0'

gst_dep = dependency('gstreamer-1.0', version : '>=1.19',
    required : true, fallback : ['gstreamer', 'gst_dep'])
gstbase_dep = dependency('gstreamer-base-1.0', version : '>=1.19',
  fallback : ['gstreamer', 'gst_base_dep'])

vpi_dep = dependency('vpi', required: true)

nvbufsurface_dep = declare_dependency(
  include_directories: include_directories('/usr/src/jetson_multimedia_api/include'),
  dependencies: [
    cc.find_library('nvbufsurface', dirs: ['/usr/lib/aarch64-linux-gnu/nvidia']),
  ]
)
subdir('gst-plugin')
