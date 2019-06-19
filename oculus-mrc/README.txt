HOW TO BUILD

cd C:\Projects\obs-studio\plugins
mklink /j oculus-mrc <the-current-folder>

ADD THIS LINE TO C:\Projects\obs-studio\plugins\CMakeLists.txt
add_subdirectory(oculus-mrc)