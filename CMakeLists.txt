cmake_minimum_required (VERSION 3.10)

project (siaskynet-fuse)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory (dependencies)

add_executable (siaskynet-fuse source/main.cpp)

target_link_libraries (siaskynet-fuse ${FUSEPP_LIBRARIES} ${SIASKYNETPP_LIBRARIES})
target_include_directories (siaskynet-fuse PRIVATE ${FUSEPP_INCLUDE_DIRS} ${SIASKYNETPP_INCLUDE_DIRS})
target_compile_options(siaskynet-fuse PRIVATE -Wno-multichar ${FUSEPP_CFLAGS})
