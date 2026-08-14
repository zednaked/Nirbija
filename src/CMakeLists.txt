add_library(nirbija_core
  core/audio_graph.cpp
  core/channel_strip.cpp
  core/engine.cpp
  core/plugin.cpp
)
target_include_directories(nirbija_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nirbija_core PUBLIC PkgConfig::JACK)

if(NIRBIJA_LV2)
  target_sources(nirbija_core PRIVATE hosting/lv2_backend.cpp)
  target_link_libraries(nirbija_core PRIVATE PkgConfig::LILV)
  target_compile_definitions(nirbija_core PUBLIC NIRBIJA_HAVE_LV2=1)
endif()

if(NIRBIJA_CLAP)
  target_sources(nirbija_core PRIVATE hosting/clap_backend.cpp)
  target_link_libraries(nirbija_core PRIVATE clap)
  target_compile_definitions(nirbija_core PUBLIC NIRBIJA_HAVE_CLAP=1)
endif()

if(NIRBIJA_VST3)
  target_sources(nirbija_core PRIVATE hosting/vst3_backend.cpp)
  target_compile_definitions(nirbija_core PUBLIC NIRBIJA_HAVE_VST3=1)
endif()

if(NIRBIJA_UI)
  add_subdirectory(ui)
endif()
