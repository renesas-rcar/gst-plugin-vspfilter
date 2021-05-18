project('gst-vspfilter', 'c',
  version : '1.0.2',
  meson_version : '>= 0.48',
  default_options : [ 'warning_level=1',
                      'buildtype=debugoptimized' ])

pkg_version = meson.project_version()
api_version = '1.0'

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')
plugins = []

cc = meson.get_compiler('c')

if cc.has_link_argument('-Wl,-Bsymbolic-functions')
  add_project_link_arguments('-Wl,-Bsymbolic-functions', language : 'c')
endif

# Symbol visibility
if cc.has_argument('-fvisibility=hidden')
  add_project_arguments('-fvisibility=hidden', language: 'c')
endif

# Disable strict aliasing
if cc.has_argument('-fno-strict-aliasing')
  add_project_arguments('-fno-strict-aliasing', language: 'c')
endif

cdata = configuration_data()

check_headers = [
  ['HAVE_DLFCN_H', 'dlfcn.h'],
  ['HAVE_FCNTL_H', 'fcntl.h'],
  ['HAVE_INTTYPES_H', 'inttypes.h'],
  ['HAVE_MEMORY_H', 'memory.h'],
  ['HAVE_PROCESS_H', 'process.h'],
  ['HAVE_STDINT_H', 'stdint.h'],
  ['HAVE_STDLIB_H', 'stdlib.h'],
  ['HAVE_STRINGS_H', 'strings.h'],
  ['HAVE_STRING_H', 'string.h'],
  ['HAVE_SYS_IOCTL_H', 'sys/ioctl.h'],
  ['HAVE_SYS_PARAM_H', 'sys/param.h'],
  ['HAVE_SYS_SOCKET_H', 'sys/socket.h'],
  ['HAVE_SYS_STAT_H', 'sys/stat.h'],
  ['HAVE_SYS_TIME_H', 'sys/time.h'],
  ['HAVE_SYS_TYPES_H', 'sys/types.h'],
  ['HAVE_UNISTD_H', 'unistd.h'],
]

foreach h : check_headers
  if cc.has_header(h.get(1))
    cdata.set(h.get(0), 1)
  endif
endforeach

libm = cc.find_library('m', required : false)

cdata.set('SIZEOF_CHAR', cc.sizeof('char'))
cdata.set('SIZEOF_INT', cc.sizeof('int'))
cdata.set('SIZEOF_LONG', cc.sizeof('long'))
cdata.set('SIZEOF_SHORT', cc.sizeof('short'))
cdata.set('SIZEOF_VOIDP', cc.sizeof('void*'))
cdata.set('SIZEOF_OFF_T', cc.sizeof('off_t'))

cdata.set_quoted('VERSION', pkg_version)
cdata.set_quoted('PACKAGE_VERSION', pkg_version)
cdata.set_quoted('GST_LICENSE', 'LGPL')
cdata.set_quoted('PACKAGE', 'gst-vspfilter')

warning_flags = [
  '-Wmissing-declarations',
  '-Wredundant-decls',
  '-Wwrite-strings',
  '-Winit-self',
  '-Wmissing-include-dirs',
  '-Wno-multichar',
  '-Wvla',
  '-Wpointer-arith',
  '-Wmissing-prototypes',
  '-Wdeclaration-after-statement',
  '-Wold-style-definition',
  '-Waggregate-return',
]

foreach extra_arg : warning_flags
  if cc.has_argument (extra_arg)
    add_project_arguments([extra_arg], language: 'c')
  endif
endforeach

# GStreamer package name and origin url
cdata.set_quoted('GST_PACKAGE_NAME', get_option('package-name'))
cdata.set_quoted('GST_PACKAGE_ORIGIN', get_option('package-origin'))

# Mandatory GST deps
gst_req = '>= 1.2.0'
gst_dep = dependency('gstreamer-1.0', version : gst_req)
gstbase_dep = dependency('gstreamer-base-1.0', version : gst_req)
gstallocators_dep = dependency('gstreamer-allocators-1.0', version : gst_req)
gstvideo_dep = dependency('gstreamer-video-1.0', version : gst_req)

vspfilter_args = ['-DHAVE_CONFIG_H']
configinc = include_directories('.')

subdir('gst')

configure_file(output : 'config.h', configuration : cdata)

python3 = import('python').find_installation()
run_command(python3, '-c', 'import shutil; shutil.copy("hooks/pre-commit.hook", ".git/hooks/pre-commit")')
