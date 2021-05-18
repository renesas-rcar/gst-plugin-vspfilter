vspfilter_sources = [
	'gstvspfilter.c',
	'vspfilterpool.c',
	'vspfilterutils.c'
]

vspfilter = library('gstvspfilter', vspfilter_sources,
                    c_args: [vspfilter_args],
                    include_directories : [configinc],
                    dependencies: [gst_dep, gstbase_dep, gstallocators_dep, gstvideo_dep],
                    install : true,
                    install_dir : plugins_install_dir)
