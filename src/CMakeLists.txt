add_library(nirbija_core
  core/audio_graph.cpp
  core/channel_strip.cpp
  core/engine.cpp
  core/plugin.cpp
  core/state_text.cpp
  core/recorder.cpp
  core/file_player.cpp
  core/looper.cpp
  core/fx_pad.cpp
  core/step_sequencer.cpp
  core/arpeggiator.cpp
  core/script_plugin.cpp
  core/keyboard_instrument.cpp
  core/sampler.cpp
  core/chord.cpp
  core/drone.cpp
  core/ble_midi.cpp
)
target_include_directories(nirbija_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(nirbija_core PUBLIC PkgConfig::JACK
  PRIVATE PkgConfig::SNDFILE PkgConfig::LUA Threads::Threads)

# The DSP never wants errno set by sqrt(), never wants a trap on a NaN, and is
# happy to have a*b+c fused: none of that changes a result a listener could
# hear, and all of it lets the compiler vectorise the per-sample loops. Kept
# off the UI, where Qt and Lua may care.
target_compile_options(nirbija_core PRIVATE
  -fno-math-errno -fno-trapping-math -ffp-contract=fast)

# Bluetooth LE MIDI goes through BlueZ over D-Bus, and the D-Bus client is
# sd-bus. That makes libsystemd a hard dependency of a mixer, which a distro
# without systemd cannot meet; off, ble_midi.cpp compiles to a stub that never
# finds a device and the rest of the host does not notice.
if(NIRBIJA_BLE_MIDI)
  pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
  target_link_libraries(nirbija_core PRIVATE PkgConfig::SYSTEMD)
  target_compile_definitions(nirbija_core PUBLIC NIRBIJA_HAVE_BLE_MIDI=1)
endif()

if(NIRBIJA_LV2)
  target_sources(nirbija_core PRIVATE hosting/lv2_backend.cpp)
  target_link_libraries(nirbija_core PRIVATE PkgConfig::LILV)
  target_compile_definitions(nirbija_core PUBLIC NIRBIJA_HAVE_LV2=1)
endif()

if(NIRBIJA_CLAP)
  target_sources(nirbija_core PRIVATE hosting/clap_backend.cpp)
  # Headers only - clap's CMake is deliberately not configured, so there is no
  # imported target to link against.
  target_include_directories(nirbija_core PRIVATE ${clap_SOURCE_DIR}/include)
  target_compile_definitions(nirbija_core PUBLIC NIRBIJA_HAVE_CLAP=1)

  # The Drone, extracted: the same drone.cpp the host runs, wrapped as a CLAP
  # so it plays in Ardour, Reaper, Bitwig. Built into build/clap/; copy the
  # file into ~/.clap to use it. Not installed with the host on purpose -
  # the install lays down the app and nothing else, and a plugin in the
  # system CLAP path is a separate decision.
  add_library(nirbija_drone_clap MODULE
    clap/drone_clap.cpp
    core/drone.cpp
    core/state_text.cpp)
  target_include_directories(nirbija_drone_clap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${clap_SOURCE_DIR}/include)
  set_target_properties(nirbija_drone_clap PROPERTIES
    PREFIX ""
    OUTPUT_NAME "Nirbija Drone"
    SUFFIX ".clap"
    CXX_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/clap)
endif()

if(NIRBIJA_VST3)
  target_sources(nirbija_core PRIVATE hosting/vst3_backend.cpp)
  target_include_directories(nirbija_core PRIVATE ${NIRBIJA_VST3_INCLUDE_DIR})
  target_compile_definitions(nirbija_core PUBLIC NIRBIJA_HAVE_VST3=1)
endif()

if(NIRBIJA_UI)
  add_subdirectory(ui)
endif()
