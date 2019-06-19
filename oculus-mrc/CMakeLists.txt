project(oculus-mrc)

if(MSVC)
	set(oculus-mrc_PLATFORM_DEPS
		w32-pthreads)
endif()

find_package(FFmpeg REQUIRED
	COMPONENTS avcodec avfilter avdevice avutil swscale avformat swresample)
include_directories(${FFMPEG_INCLUDE_DIRS})

set(oculus-mrc_SOURCES
	oculus-mrc.cpp
	log.h
	frame.h
	frame.cpp
)

add_library(oculus-mrc MODULE
	${oculus-mrc_SOURCES})
target_link_libraries(oculus-mrc
	libobs
	${oculus-mrc_PLATFORM_DEPS}
	${FFMPEG_LIBRARIES})

install_obs_plugin_with_data(oculus-mrc data)
