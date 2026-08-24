add_library(nirbija_core
  core/audio_graph.cpp
  core/channel_strip.cpp
  core/engine.cpp
  core/plugin.cpp
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
)
target_include_directories(nirbija_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nirbija_core PUBLIC PkgConfig::JACK
  PRIVATE PkgConfig::SNDFILE PkgConfig::LUA)

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
endif()

if(NIRBIJA_VST3)
  target_sources(nirbija_core PRIVATE hosting/vst3_backend.cpp)
  target_include_directories(nirbija_core PRIVATE ${CMAKE_BINARY_DIR}/vst3)
  target_compile_definitions(nirbija_core PUBLIC NIRBIJA_HAVE_VST3=1)
endif()

if(NIRBIJA_UI)
  add_subdirectory(ui)
endif()
