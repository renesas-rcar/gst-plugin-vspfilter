# gstreamer-vspfilter package version number
m4_define([gst_vspfilter_major_version], [1])
m4_define([gst_vspfilter_minor_version], [0])
m4_define([gst_vspfilter_micro_version], [0])
m4_define([gst_vspfilter_version],
          [gst_vspfilter_major_version.gst_vspfilter_minor_version.gst_vspfilter_micro_version])

# glib version number
m4_define([glib_version], [2.32])

AC_PREREQ([2.68])
AC_INIT([gst_vspfilter], [gst_vspfilter_version])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])

AC_CANONICAL_TARGET

AM_INIT_AUTOMAKE([1.11 tar-ustar no-dist-gzip dist-bzip2])

dnl Use pretty build output with automake >= 1.11
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])], [
  AM_DEFAULT_VERBOSITY=1
  AC_SUBST(AM_DEFAULT_VERBOSITY)
])

dnl Check for tools
AC_PROG_CC
AM_PROG_CC_C_O

dnl Initialize libtool
LT_PREREQ([2.2])
LT_INIT

dnl Check for GLib
GLIB_VERSION_REQUIRED=glib_version
PKG_CHECK_MODULES([GLIB], [glib-2.0 >= $GLIB_VERSION_REQUIRED])
AC_SUBST(GLIB_VERSION_REQUIRED)


dnl ---------------------------------------------------------------------------
dnl -- GStreamer                                                             --
dnl ---------------------------------------------------------------------------

GST_API_VERSION=1.0
AC_SUBST(GST_API_VERSION)
AC_DEFINE_UNQUOTED(GST_API_VERSION, "$GST_API_VERSION",
  [GStreamer API Version])
gst_pkg_version="1.0"

if $PKG_CONFIG --exists "gstreamer-$gst_pkg_version"; then
  gst_version=`$PKG_CONFIG --modversion "gstreamer-$gst_pkg_version"`
  gst_major_version=`echo "$gst_version" | cut -d'.' -f1`
  gst_minor_version=`echo "$gst_version" | cut -d'.' -f2`
  GST_API_VERSION="${gst_major_version}.${gst_minor_version}"
  GST_PKG_VERSION="$gst_pkg_version"
fi

if test -z "$GST_PKG_VERSION"; then
    AC_MSG_ERROR([version $GST_API_VERSION not found])
fi
AC_MSG_RESULT([$GST_API_VERSION])
AC_SUBST([GST_PKG_VERSION])

dnl set license and copyright notice
GST_LICENSE="LGPL"
AC_DEFINE_UNQUOTED(GST_LICENSE, "$GST_LICENSE", [GStreamer license])
AC_SUBST(GST_LICENSE)

dnl set package name
GST_PACKAGE_NAME="GStreamer vspfilter Plug-in source release"
AC_DEFINE_UNQUOTED(GST_PACKAGE_NAME, "$GST_PACKAGE_NAME",
                   [GStreamer package name])
AC_SUBST(GST_PACKAGE_NAME)

dnl set package origin
GST_PACKAGE_ORIGIN="Unknown package origin"
AC_DEFINE_UNQUOTED(GST_PACKAGE_ORIGIN, "$GST_PACKAGE_ORIGIN",
                   [GStreamer package origin])
AC_SUBST(GST_PACKAGE_ORIGIN)

dnl *** required versions of GStreamer stuff ***
GST_REQ=1.2.0
GSTPB_REQ=1.2.3

dnl Version Checks for GStreamer and plugins-base
PKG_CHECK_MODULES([GST],
    [gstreamer-$GST_PKG_VERSION >= $GST_REQ])
PKG_CHECK_MODULES([GST_BASE],
    [gstreamer-base-$GST_PKG_VERSION >= $GSTPB_REQ])
PKG_CHECK_MODULES([GST_VIDEO],
    [gstreamer-video-$GST_PKG_VERSION >= $GSTPB_REQ])
PKG_CHECK_MODULES([GST_ALLOCATORS],
    [gstreamer-allocators-$GST_PKG_VERSION >= $GSTPB_REQ])

dnl Check for the GStreamer plugins directory
AC_ARG_VAR([GST_PLUGIN_PATH], [installation path for gstreamer-vspfilter plugin elements])
AC_MSG_CHECKING([for GStreamer plugins directory])
if test -d "$GST_PLUGIN_PATH"; then
    GST_PLUGINS_DIR="$GST_PLUGIN_PATH"
else
    GST_PLUGINS_DIR="\${libdir}/gstreamer-$GST_PKG_VERSION"
fi
AC_MSG_RESULT([$GST_PLUGINS_DIR])
plugindir="$GST_PLUGINS_DIR"
AC_SUBST(plugindir)

dnl GST_ALL_LDFLAGS:
dnl LDFLAGS really should only contain flags, not libs - they get added before
dnl whatevertarget_LIBS and -L flags here affect the rest of the linking
GST_ALL_LDFLAGS="-no-undefined"
AC_SUBST(GST_ALL_LDFLAGS)

dnl GST_PLUGIN_LDFLAGS:
dnl this really should only contain flags, not libs - they get added before
dnl whatevertarget_LIBS and -L flags here affect the rest of the linking
GST_PLUGIN_LDFLAGS="-module -avoid-version -export-symbols-regex '^([_]*gst_plugin_desc|gst_.*_get_type)\$\$' $GST_ALL_LDFLAGS"
AC_SUBST(GST_PLUGIN_LDFLAGS)

AC_CONFIG_FILES([
    Makefile
    gst/Makefile
    gst/vspfilter/Makefile
    common/Makefile
    common/m4/Makefile
])
AC_OUTPUT
