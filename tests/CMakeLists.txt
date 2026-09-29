add_executable(nirbija_scan scan_main.cpp)
target_link_libraries(nirbija_scan PRIVATE nirbija_core)
add_test(NAME scan COMMAND nirbija_scan)

add_executable(nirbija_graph_routing graph_routing.cpp)
target_link_libraries(nirbija_graph_routing PRIVATE nirbija_core)
add_test(NAME graph_routing COMMAND nirbija_graph_routing)

add_executable(nirbija_state_locale state_locale.cpp)
target_link_libraries(nirbija_state_locale PRIVATE nirbija_core)
add_test(NAME state_locale COMMAND nirbija_state_locale)

add_executable(nirbija_graph_smooth graph_smooth.cpp)
target_link_libraries(nirbija_graph_smooth PRIVATE nirbija_core)
add_test(NAME graph_smooth COMMAND nirbija_graph_smooth)

add_executable(nirbija_strip_reactivate strip_reactivate.cpp)
target_link_libraries(nirbija_strip_reactivate PRIVATE nirbija_core)
add_test(NAME strip_reactivate COMMAND nirbija_strip_reactivate)

add_executable(nirbija_pdc_crossfade pdc_crossfade.cpp)
target_link_libraries(nirbija_pdc_crossfade PRIVATE nirbija_core)
add_test(NAME pdc_crossfade COMMAND nirbija_pdc_crossfade)

add_executable(nirbija_chain_fallback chain_fallback.cpp)
target_link_libraries(nirbija_chain_fallback PRIVATE nirbija_core)
add_test(NAME chain_fallback COMMAND nirbija_chain_fallback)

add_executable(nirbija_midi_order midi_order.cpp)
target_link_libraries(nirbija_midi_order PRIVATE nirbija_core)
add_test(NAME midi_order COMMAND nirbija_midi_order)

add_executable(nirbija_scan_cache scan_cache.cpp)
target_link_libraries(nirbija_scan_cache PRIVATE nirbija_core)
add_test(NAME scan_cache COMMAND nirbija_scan_cache)

add_executable(nirbija_scene_conductor scene_conductor.cpp)
target_link_libraries(nirbija_scene_conductor PRIVATE nirbija_core)
add_test(NAME scene_conductor COMMAND nirbija_scene_conductor)

add_executable(nirbija_beat_grid beat_grid.cpp)
target_link_libraries(nirbija_beat_grid PRIVATE nirbija_core)
add_test(NAME beat_grid COMMAND nirbija_beat_grid)

add_executable(nirbija_graph_click graph_click.cpp)
target_link_libraries(nirbija_graph_click PRIVATE nirbija_core)
add_test(NAME graph_click COMMAND nirbija_graph_click)

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
  set_tests_properties(lv2_process PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(NIRBIJA_CLAP)
  add_executable(nirbija_clap_process clap_process.cpp)
  target_link_libraries(nirbija_clap_process PRIVATE nirbija_core)
  add_test(NAME clap_process COMMAND nirbija_clap_process)
  set_tests_properties(clap_process PROPERTIES SKIP_RETURN_CODE 77)
  add_dependencies(nirbija_clap_process nirbija_drone_clap)

  # MIDI into the event a CLAP note port asked for, checked without a plugin.
  add_executable(nirbija_clap_events clap_events.cpp)
  target_link_libraries(nirbija_clap_events PRIVATE nirbija_core)
  target_include_directories(nirbija_clap_events PRIVATE ${clap_SOURCE_DIR}/include)
  add_test(NAME clap_events COMMAND nirbija_clap_events)
endif()

if(NIRBIJA_UI)
  add_executable(nirbija_ui_smoke ui_smoke.cpp)
  target_link_libraries(nirbija_ui_smoke PRIVATE nirbija_ui)
  add_test(NAME ui_smoke COMMAND nirbija_ui_smoke)

  add_executable(nirbija_session_dirty session_dirty.cpp)
  target_link_libraries(nirbija_session_dirty PRIVATE nirbija_ui)
  add_test(NAME session_dirty COMMAND nirbija_session_dirty)

  add_executable(nirbija_session_autosave_live session_autosave_live.cpp)
  target_link_libraries(nirbija_session_autosave_live PRIVATE nirbija_ui)
  add_test(NAME session_autosave_live COMMAND nirbija_session_autosave_live)

  # Undo and redo change only what differs, park nothing, keep the plugins
  # they did not touch.
  add_executable(nirbija_session_undo_live session_undo_live.cpp)
  target_link_libraries(nirbija_session_undo_live PRIVATE nirbija_ui)
  add_test(NAME session_undo_live COMMAND nirbija_session_undo_live)
endif()

if(NIRBIJA_UI)
  # Manual tool, not a ctest target: it needs a real X11 display.
  add_executable(nirbija_gui_probe gui_probe.cpp)
  target_link_libraries(nirbija_gui_probe PRIVATE nirbija_ui)

  # Manual tool: the whole mixer, one built-in plugin's editor open, one PNG.
  # Linked like the app itself, since it loads the same QML module; the Qt
  # targets are found again here because src/ui found them in its own scope.
  find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Widgets)
  add_executable(nirbija_editor_probe editor_probe.cpp)
  target_link_libraries(nirbija_editor_probe
    PRIVATE nirbija_ui nirbija_uiplugin Qt6::Quick Qt6::QuickControls2 Qt6::Widgets)
endif()

add_executable(nirbija_midi_synth midi_synth.cpp)
target_link_libraries(nirbija_midi_synth PRIVATE nirbija_core)
add_test(NAME midi_synth COMMAND nirbija_midi_synth)
set_tests_properties(midi_synth PROPERTIES SKIP_RETURN_CODE 77)

add_executable(nirbija_midi_jack midi_jack.cpp)
target_link_libraries(nirbija_midi_jack PRIVATE nirbija_core)
add_test(NAME midi_jack COMMAND nirbija_midi_jack)
set_tests_properties(midi_jack PROPERTIES SKIP_RETURN_CODE 77)

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
  target_compile_definitions(nirbija_session_roundtrip PRIVATE
    NIRBIJA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME session_roundtrip COMMAND nirbija_session_roundtrip)

  add_executable(nirbija_session_scenes session_scenes.cpp)
  target_link_libraries(nirbija_session_scenes PRIVATE nirbija_ui)
  add_test(NAME session_scenes COMMAND nirbija_session_scenes)
endif()

add_executable(nirbija_recorder_take recorder_take.cpp)
target_link_libraries(nirbija_recorder_take PRIVATE nirbija_core PkgConfig::SNDFILE)
add_test(NAME recorder_take COMMAND nirbija_recorder_take)
set_tests_properties(recorder_take PROPERTIES SKIP_RETURN_CODE 77)

add_executable(nirbija_file_player file_player_test.cpp)
target_link_libraries(nirbija_file_player PRIVATE nirbija_core PkgConfig::SNDFILE)
add_test(NAME file_player COMMAND nirbija_file_player)

add_executable(nirbija_looper looper_test.cpp)
target_link_libraries(nirbija_looper PRIVATE nirbija_core)
add_test(NAME looper COMMAND nirbija_looper)

add_executable(nirbija_looper_rt looper_rt_test.cpp)
target_link_libraries(nirbija_looper_rt PRIVATE nirbija_core)
add_test(NAME looper_rt COMMAND nirbija_looper_rt)

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

if(NIRBIJA_CLAP)
  # The Drone .clap opened directly, no host backend in between: what it
  # pushes back to a host (the Root a played note set), how it takes a note
  # at velocity zero, and that reset() leaves it silent without allocating.
  add_executable(nirbija_drone_clap_events drone_clap_events.cpp)
  target_include_directories(nirbija_drone_clap_events PRIVATE ${clap_SOURCE_DIR}/include)
  target_link_libraries(nirbija_drone_clap_events PRIVATE nirbija_core ${CMAKE_DL_LIBS})
  target_compile_definitions(nirbija_drone_clap_events PRIVATE
    NIRBIJA_DRONE_CLAP_DIR="${CMAKE_BINARY_DIR}/clap")
  add_dependencies(nirbija_drone_clap_events nirbija_drone_clap)
  add_test(NAME drone_clap_events COMMAND nirbija_drone_clap_events)
endif()

add_executable(nirbija_chord chord_test.cpp)
target_link_libraries(nirbija_chord PRIVATE nirbija_core)
add_test(NAME chord COMMAND nirbija_chord)

add_executable(nirbija_drone drone_test.cpp)
target_link_libraries(nirbija_drone PRIVATE nirbija_core)
add_test(NAME drone COMMAND nirbija_drone)

if(NIRBIJA_CLAP)
  # The extracted Drone, loaded back through the host's own CLAP backend:
  # the one place the wrapper's ports, params, events and state are checked
  # against a host that did not write them.
  add_executable(nirbija_drone_clap_test drone_clap.cpp)
  target_link_libraries(nirbija_drone_clap_test PRIVATE nirbija_core)
  target_compile_definitions(nirbija_drone_clap_test PRIVATE
    NIRBIJA_DRONE_CLAP_DIR="${CMAKE_BINARY_DIR}/clap")
  add_dependencies(nirbija_drone_clap_test nirbija_drone_clap)
  add_test(NAME drone_clap COMMAND nirbija_drone_clap_test)
endif()

if(NIRBIJA_VST3)
  add_executable(nirbija_vst3_process vst3_process.cpp)
  target_link_libraries(nirbija_vst3_process PRIVATE nirbija_core)
  add_test(NAME vst3_process COMMAND nirbija_vst3_process)
  set_tests_properties(vst3_process PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(NIRBIJA_UI)
  # Manual tool, not a ctest target: loads the session NIRBIJA_SESSION names,
  # presses play and prints the master and strip peaks - the quickest way to
  # tell "silent because of the code" from "silent because of the session".
  add_executable(nirbija_probe_sound probe_sound.cpp)
  target_link_libraries(nirbija_probe_sound PRIVATE nirbija_ui)
endif()

# How long a block takes, worst case. Always runs and prints; only fails
# with NIRBIJA_BENCH_STRICT set, from a Release tree.
add_executable(nirbija_render_bench render_bench.cpp)
target_link_libraries(nirbija_render_bench PRIVATE nirbija_core)
add_test(NAME render_bench COMMAND nirbija_render_bench)

# --- labels and timeouts -----------------------------------------------------
# Every test is `quick` unless it needs something the machine may not have:
#   plugins  opens plugins installed on this machine (minutes under ASan,
#            and a different answer on every box)
#   jack     needs a running JACK/PipeWire server (skips with 77 without one)
#   ui       needs Qt (offscreen)
# The pre-push hook runs `-L quick`; CI runs everything. No test may run
# without a timeout: a stuck audio server used to hold a push for the
# ctest default of 25 minutes.
get_property(nirbija_all_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS nirbija_all_tests)
  set_tests_properties(${test} PROPERTIES LABELS quick TIMEOUT 120)
endforeach()
foreach(test IN ITEMS scan lv2_process vst3_process midi_synth)
  if(TEST ${test})
    set_tests_properties(${test} PROPERTIES LABELS plugins TIMEOUT 900)
  endif()
endforeach()
foreach(test IN ITEMS engine_smoke midi_jack midi_clock_follow recorder_take)
  if(TEST ${test})
    set_tests_properties(${test} PROPERTIES LABELS jack TIMEOUT 120)
  endif()
endforeach()
foreach(test IN ITEMS ui_smoke session_dirty session_autosave_live session_undo_live
                      session_roundtrip session_scenes)
  if(TEST ${test})
    set_tests_properties(${test} PROPERTIES LABELS "quick;ui" TIMEOUT 300
                         SKIP_RETURN_CODE 77)
  endif()
endforeach()
