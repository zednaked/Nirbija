add_executable(nirbija_scan scan_main.cpp)
target_link_libraries(nirbija_scan PRIVATE nirbija_core)
add_test(NAME scan COMMAND nirbija_scan)

add_executable(nirbija_graph_routing graph_routing.cpp)
target_link_libraries(nirbija_graph_routing PRIVATE nirbija_core)
add_test(NAME graph_routing COMMAND nirbija_graph_routing)

add_executable(nirbija_insert_reclaim insert_reclaim.cpp)
target_link_libraries(nirbija_insert_reclaim PRIVATE nirbija_core)
add_test(NAME insert_reclaim COMMAND nirbija_insert_reclaim)

add_executable(nirbija_engine_smoke engine_smoke.cpp)
target_link_libraries(nirbija_engine_smoke PRIVATE nirbija_core)
add_test(NAME engine_smoke COMMAND nirbija_engine_smoke)
set_tests_properties(engine_smoke PROPERTIES SKIP_RETURN_CODE 77)

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

  add_executable(nirbija_session_dirty session_dirty.cpp)
  target_link_libraries(nirbija_session_dirty PRIVATE nirbija_ui)
  add_test(NAME session_dirty COMMAND nirbija_session_dirty)
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

add_executable(nirbija_midi_clock_follow midi_clock_follow.cpp)
target_link_libraries(nirbija_midi_clock_follow PRIVATE nirbija_core)
add_test(NAME midi_clock_follow COMMAND nirbija_midi_clock_follow)
set_tests_properties(midi_clock_follow PROPERTIES SKIP_RETURN_CODE 77)

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

add_executable(nirbija_looper looper_test.cpp)
target_link_libraries(nirbija_looper PRIVATE nirbija_core)
add_test(NAME looper COMMAND nirbija_looper)

add_executable(nirbija_fx_pad fx_pad_test.cpp)
target_link_libraries(nirbija_fx_pad PRIVATE nirbija_core)
add_test(NAME fx_pad COMMAND nirbija_fx_pad)

add_executable(nirbija_step_sequencer step_sequencer_test.cpp)
target_link_libraries(nirbija_step_sequencer PRIVATE nirbija_core)
add_test(NAME step_sequencer COMMAND nirbija_step_sequencer)

add_executable(nirbija_arpeggiator arpeggiator_test.cpp)
target_link_libraries(nirbija_arpeggiator PRIVATE nirbija_core)
add_test(NAME arpeggiator COMMAND nirbija_arpeggiator)

add_executable(nirbija_keyboard_instrument keyboard_instrument_test.cpp)
target_link_libraries(nirbija_keyboard_instrument PRIVATE nirbija_core)
add_test(NAME keyboard_instrument COMMAND nirbija_keyboard_instrument)

add_executable(nirbija_script_plugin script_plugin_test.cpp)
target_link_libraries(nirbija_script_plugin PRIVATE nirbija_core)
add_test(NAME script_plugin COMMAND nirbija_script_plugin)

add_executable(nirbija_sampler sampler_test.cpp)
target_link_libraries(nirbija_sampler PRIVATE nirbija_core PkgConfig::SNDFILE)
add_test(NAME sampler COMMAND nirbija_sampler)

add_executable(nirbija_chord chord_test.cpp)
target_link_libraries(nirbija_chord PRIVATE nirbija_core)
add_test(NAME chord COMMAND nirbija_chord)

if(NIRBIJA_VST3)
  add_executable(nirbija_vst3_process vst3_process.cpp)
  target_link_libraries(nirbija_vst3_process PRIVATE nirbija_core)
  add_test(NAME vst3_process COMMAND nirbija_vst3_process)
endif()
