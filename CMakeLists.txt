cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME apt)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

set(EXTENSION_SOURCES
    src/apt_extension.cpp
    src/apt_common.cpp
    src/apt_history.cpp
    src/apt_indexes.cpp
    src/apt_sources.cpp
    src/apt_versions.cpp
    src/dpkg_status.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# lz4, to read the lz4-compressed apt indexes: from vcpkg (CI), or else an
# installed lz4 library (set LZ4_ROOT to its prefix if CMake does not find it,
# e.g. LZ4_ROOT=/home/linuxbrew/.linuxbrew)
find_package(lz4 CONFIG QUIET)
if(TARGET lz4::lz4)
  set(APT_LZ4_LIBRARY lz4::lz4)
else()
  find_path(LZ4_INCLUDE_DIR lz4frame.h HINTS $ENV{LZ4_ROOT}/include REQUIRED)
  find_library(
    LZ4_LIBRARY
    NAMES liblz4.a lz4
    HINTS $ENV{LZ4_ROOT}/lib REQUIRED)
  set(APT_LZ4_LIBRARY ${LZ4_LIBRARY})
  target_include_directories(${EXTENSION_NAME} PRIVATE ${LZ4_INCLUDE_DIR})
  target_include_directories(${LOADABLE_EXTENSION_NAME}
                             PRIVATE ${LZ4_INCLUDE_DIR})
endif()

# Link libraries in both the static library as the loadable extension
target_link_libraries(${EXTENSION_NAME} ${APT_LZ4_LIBRARY})
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${APT_LZ4_LIBRARY})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
