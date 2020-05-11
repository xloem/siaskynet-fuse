
##### fusepp

find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)

set (FUSEPP_INCLUDE_DIRS ${FUSE3_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/fusepp CACHE INTERNAL "")
set (FUSEPP_LIBRARIES ${FUSE3_LIBRARIES} CACHE INTERNAL "")
set (FUSEPP_CFLAGS ${FUSE3_CFLAGS_OTHER} CACHE INTERNAL "")


##### siaskynetpp

add_subdirectory (siaskynetpp)

