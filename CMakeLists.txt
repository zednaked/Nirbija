cmake_minimum_required(VERSION 3.28)
project(nirbija VERSION 0.5.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(NIRBIJA_LV2 "Build the LV2 plugin backend" ON)
option(NIRBIJA_CLAP "Build the CLAP plugin backend" ON)
option(NIRBIJA_VST3 "Build the VST3 plugin backend" ON)
option(NIRBIJA_UI "Build the Qt6 QML user interface" ON)
option(NIRBIJA_TESTS "Build the CLI test harnesses" ON)
option(NIRBIJA_BLE_MIDI "Talk to Bluetooth LE MIDI devices over BlueZ (needs libsystemd)" ON)
option(NIRBIJA_WERROR "Treat warnings as errors (the pre-push hook and CI turn this on)" OFF)
option(NIRBIJA_LTO "Link-time optimisation for Release builds, when the toolchain has it" ON)

# A tree configured with no build type is a Debug tree with no -g: the slowest
# binary there is, and the one a first `cmake -S . -B build` used to hand out.
# Release it is unless asked otherwise; the presets say it explicitly.
get_property(nirbija_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT nirbija_multi_config AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
if(NIRBIJA_WERROR)
  add_compile_options(-Werror)
endif()

# Release only. -fno-plt turns every call into a shared library into a direct
# GOT load, which is what a callback-heavy audio path wants; the visibility
# flags stop the static core's every symbol from being exported by the
# executable, which shrinks the binary and lets the linker drop what nothing
# uses. Plugins never look symbols up in the host, so nothing is lost.
add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:-fno-plt>)
add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:-fvisibility-inlines-hidden>)

if(NIRBIJA_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT nirbija_ipo_ok OUTPUT nirbija_ipo_why LANGUAGES CXX)
  if(nirbija_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
  else()
    message(STATUS "LTO not available: ${nirbija_ipo_why}")
  endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)
# Declared here rather than in src/ so the tests can link it too.
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
# The scriptable MIDI plugin embeds Lua. Named 'lua5.4' on some distributions
# and 'lua' on others, so both are tried before giving up.
pkg_check_modules(LUA IMPORTED_TARGET lua5.4)
if(NOT LUA_FOUND)
  pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua)
endif()

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
  # Where `pluginterfaces/...` resolves from: the parent of wherever the
  # sources actually landed, which is the SOURCE_DIR above unless a
  # -DFETCHCONTENT_SOURCE_DIR_VST3_PLUGINTERFACES override pointed elsewhere
  # (a second tree reusing the first one's checkout does exactly that).
  get_filename_component(NIRBIJA_VST3_INCLUDE_DIR
    "${vst3_pluginterfaces_SOURCE_DIR}" DIRECTORY)
endif()

if(NIRBIJA_CLAP)
  # CLAP is header-only, so its CMake project is not wanted here at all: pointed
  # at a subdirectory that holds no CMakeLists.txt, FetchContent fetches the
  # sources and skips add_subdirectory(). Without this its own install() rules
  # run on `cmake --install` and dump the whole SDK - headers, .pc, cmake
  # config - into the prefix alongside the one binary that belongs there.
  FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.6
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-configure-clap)
  FetchContent_MakeAvailable(clap)
endif()

add_subdirectory(src)

if(NIRBIJA_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
