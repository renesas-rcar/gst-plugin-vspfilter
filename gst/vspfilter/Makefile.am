plugin_LTLIBRARIES = libgstvspfilter.la

libgstvspfilter_la_SOURCES =  \
	gstvspfilter.c \
	vspfilterpool.c \
	vspfilterutils.c

libgstvspfilter_la_CFLAGS = \
	$(GST_PLUGINS_BASE_CFLAGS) \
	$(GST_VIDEO_CFLAGS) \
	$(GST_ALLOCATORS_CFLAGS) \
	$(GST_CFLAGS)
libgstvspfilter_la_LIBADD = \
	$(GST_VIDEO_LIBS) \
	$(GST_ALLOCATORS_LIBS) \
	$(GST_BASE_LIBS) \
	$(GST_LIBS)
libgstvspfilter_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)

noinst_HEADERS = \
	gstvspfilter.h \
	vspfilterpool.h \
	vspfilterutils.h
