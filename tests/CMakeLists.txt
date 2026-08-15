add_executable(nirbija_scan scan_main.cpp)
target_link_libraries(nirbija_scan PRIVATE nirbija_core)
add_test(NAME scan COMMAND nirbija_scan)

add_executable(nirbija_graph_routing graph_routing.cpp)
target_link_libraries(nirbija_graph_routing PRIVATE nirbija_core)
add_test(NAME graph_routing COMMAND nirbija_graph_routing)

add_executable(nirbija_engine_smoke engine_smoke.cpp)
target_link_libraries(nirbija_engine_smoke PRIVATE nirbija_core)
add_test(NAME engine_smoke COMMAND nirbija_engine_smoke)

if(NIRBIJA_LV2)
  add_executable(nirbija_lv2_process lv2_process.cpp)
  target_link_libraries(nirbija_lv2_process PRIVATE nirbija_core)
  add_test(NAME lv2_process COMMAND nirbija_lv2_process)
endif()

if(NIRBIJA_CLAP)
  add_executable(nirbija_clap_process clap_process.cpp)
  target_link_libraries(nirbija_clap_process PRIVATE nirbija_core)
  add_test(NAME clap_process COMMAND nirbija_clap_process)
endif()

if(NIRBIJA_UI)
  add_executable(nirbija_ui_smoke ui_smoke.cpp)
  target_link_libraries(nirbija_ui_smoke PRIVATE nirbija_ui)
  add_test(NAME ui_smoke COMMAND nirbija_ui_smoke)
endif()

if(NIRBIJA_UI)
  # Manual tool, not a ctest target: it needs a real X11 display.
  add_executable(nirbija_gui_probe gui_probe.cpp)
  target_link_libraries(nirbija_gui_probe PRIVATE nirbija_ui)
endif()

add_executable(nirbija_midi_synth midi_synth.cpp)
target_link_libraries(nirbija_midi_synth PRIVATE nirbija_core)
add_test(NAME midi_synth COMMAND nirbija_midi_synth)

add_executable(nirbija_midi_jack midi_jack.cpp)
target_link_libraries(nirbija_midi_jack PRIVATE nirbija_core)
add_test(NAME midi_jack COMMAND nirbija_midi_jack)

# Manual tool, not a ctest target: needs a MIDI source and someone to play it.
add_executable(nirbija_live live_session.cpp)
target_link_libraries(nirbija_live PRIVATE nirbija_core)

if(NIRBIJA_UI)
  add_executable(nirbija_session_roundtrip session_roundtrip.cpp)
  target_link_libraries(nirbija_session_roundtrip PRIVATE nirbija_ui)
  add_test(NAME session_roundtrip COMMAND nirbija_session_roundtrip)
endif()

add_executable(nirbija_recorder_take recorder_take.cpp)
target_link_libraries(nirbija_recorder_take PRIVATE nirbija_core PkgConfig::SNDFILE)
add_test(NAME recorder_take COMMAND nirbija_recorder_take)

add_executable(nirbija_file_player file_player_test.cpp)
target_link_libraries(nirbija_file_player PRIVATE nirbija_core PkgConfig::SNDFILE)
add_test(NAME file_player COMMAND nirbija_file_player)
