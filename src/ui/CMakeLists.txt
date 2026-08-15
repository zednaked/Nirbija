find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2)
# Plugin editor windows are created with Xlib directly, not through Qt.
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
qt_standard_project_setup(REQUIRES 6.5)

# The models live in their own library so a headless test can drive them
# without the QML front end. The QML module is declared on that library rather
# than on the executable: the C++ types QML uses (Mixer, Skin, PluginFilterModel)
# are compiled here, and a type can only register itself into the module its own
# translation unit belongs to.
qt_add_library(nirbija_ui STATIC)

qt_add_qml_module(nirbija_ui
  URI Nirbija
  VERSION 1.0
  SOURCES
    mixer_model.cpp
    mixer_model.h
    mixer_session.cpp
    plugin_list_model.cpp
    plugin_list_model.h
    plugin_window.cpp
    plugin_window.h
    skin.cpp
    skin.h
  QML_FILES
    qml/Main.qml
    qml/TopBar.qml
    qml/ChannelStrip.qml
    qml/MasterStrip.qml
    qml/MidiMatrix.qml
    qml/MidiKeyboard.qml
    qml/Navigator.qml
    qml/NodeSlot.qml
    qml/InsertSlot.qml
    qml/Fader.qml
    qml/Meter.qml
    qml/PeakDots.qml
    qml/StripButton.qml
    qml/Tip.qml
    qml/ParamEditor.qml
    qml/PluginPicker.qml
    qml/PortPicker.qml
    qml/SendRow.qml
    qml/SlotMenu.qml
    qml/RenameDialog.qml
    qml/PanControl.qml
    qml/ValueTrack.qml
    qml/ShortcutSheet.qml
)

target_link_libraries(nirbija_ui PUBLIC nirbija_core Qt6::Quick PRIVATE PkgConfig::X11)
target_include_directories(nirbija_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

qt_add_executable(nirbija
  main.cpp
)

# The backing library is static, so the module's plugin has to be linked
# explicitly or nothing registers the types at startup.
target_link_libraries(nirbija
  PRIVATE nirbija_ui nirbija_uiplugin Qt6::Quick Qt6::QuickControls2)
target_include_directories(nirbija PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS nirbija RUNTIME DESTINATION bin)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/Nirbija.desktop
        DESTINATION share/applications)
