cmake_minimum_required(VERSION 3.28)
project(nirbija VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(NIRBIJA_LV2 "Build the LV2 plugin backend" ON)
option(NIRBIJA_CLAP "Build the CLAP plugin backend" ON)
option(NIRBIJA_VST3 "Build the VST3 plugin backend" ON)
option(NIRBIJA_UI "Build the Qt6 QML user interface" OFF)
option(NIRBIJA_TESTS "Build the CLI test harnesses" ON)

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)
# Declared here rather than in src/ so the tests can link it too.
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)

if(NIRBIJA_LV2)
  pkg_check_modules(LILV REQUIRED IMPORTED_TARGET lilv-0)
endif()

include(FetchContent)

if(NIRBIJA_VST3)
  # Only the interface headers, not the whole SDK: a host built on the raw
  # pluginterfaces needs no Steinberg sources compiled at all. Placed so that
  # #include "pluginterfaces/..." resolves naturally.
  FetchContent_Declare(vst3_pluginterfaces
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3_pluginterfaces.git
    # Pinned to the commit this backend was written and tested against.
    GIT_TAG 4f547e8e102b47de4a8b8aaf343c73b700786372
    GIT_SHALLOW TRUE
    SOURCE_DIR ${CMAKE_BINARY_DIR}/vst3/pluginterfaces)
  FetchContent_MakeAvailable(vst3_pluginterfaces)
endif()

if(NIRBIJA_CLAP)
  FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.6
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(clap)
endif()

add_subdirectory(src)

if(NIRBIJA_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
