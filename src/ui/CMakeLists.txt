find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2)
# Plugin editor windows are created with Xlib directly, not through Qt.
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
qt_standard_project_setup(REQUIRES 6.5)

# The models live in their own library so a headless test can drive them
# without the QML front end.
qt_add_library(nirbija_ui STATIC
  mixer_model.cpp
  plugin_list_model.cpp
  plugin_window.cpp
  skin.h
)
target_link_libraries(nirbija_ui PUBLIC nirbija_core Qt6::Quick PRIVATE PkgConfig::X11)
target_include_directories(nirbija_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

qt_add_executable(nirbija
  main.cpp
)

qt_add_qml_module(nirbija
  URI Nirbija
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/TopBar.qml
    qml/ChannelStrip.qml
    qml/NodeSlot.qml
    qml/InsertSlot.qml
    qml/Fader.qml
    qml/PeakDots.qml
    qml/StripButton.qml
    qml/PluginPicker.qml
)

target_link_libraries(nirbija PRIVATE nirbija_ui Qt6::Quick Qt6::QuickControls2)
target_include_directories(nirbija PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
