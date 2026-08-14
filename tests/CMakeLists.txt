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
