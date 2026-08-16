find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2)
# Plugin editor windows are created with Xlib directly, not through Qt.
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
qt_standard_project_setup(REQUIRES 6.5)

# The module's URI is `Nirbija` but this directory is `ui`, so by default the
# generated qmldir lands somewhere no import path can resolve: qmllint then
# fails to import the module and reports every use of Skin, Mixer and
# PluginFilterModel as an unqualified access. Naming an output root puts the
# module under <root>/Nirbija, which is what the URI says it should be.
# Runtime is unaffected - the QML travels in the binary's resources.
set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml)

# The .qml files are kept in qml/ on disk but belong at the root of the module,
# addressed by type name. Saying so per file is what QTP0004 wants: without it
# Qt treats qml/ as a nested module needing a qmldir of its own, and warns on
# every configure.
qt_policy(SET QTP0004 NEW)

set(nirbija_qml
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
foreach(qml_file IN LISTS nirbija_qml)
  get_filename_component(qml_name ${qml_file} NAME)
  set_source_files_properties(${qml_file}
    PROPERTIES QT_QML_SOURCE_FILE_NAME ${qml_name})
endforeach()

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
  QML_FILES ${nirbija_qml}
)

target_link_libraries(nirbija_ui PUBLIC nirbija_core Qt6::Quick PRIVATE PkgConfig::X11)
target_include_directories(nirbija_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The app icon is baked in as well as installed: the app forces itself onto
# X11, where the taskbar reads _NET_WM_ICON off the window rather than looking
# the .desktop file up, so a build run straight out of the tree still shows the
# mark. PNG rather than the SVG so no Qt SVG plugin is needed at runtime.
qt_add_resources(nirbija_ui "nirbija_icon"
  PREFIX "/"
  BASE ${CMAKE_SOURCE_DIR}/packaging
  FILES ${CMAKE_SOURCE_DIR}/packaging/nirbija.png)

qt_add_executable(nirbija
  main.cpp
)

# The backing library is static, so the module's plugin has to be linked
# explicitly or nothing registers the types at startup.
target_link_libraries(nirbija
  PRIVATE nirbija_ui nirbija_uiplugin Qt6::Quick Qt6::QuickControls2)
target_include_directories(nirbija PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# One source for the version number: project() in the top-level CMakeLists.
target_compile_definitions(nirbija PRIVATE NIRBIJA_VERSION="${PROJECT_VERSION}")

install(TARGETS nirbija RUNTIME DESTINATION bin)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/Nirbija.desktop
        DESTINATION share/applications)
# Both sizes of the same mark: the scalable one is what a modern desktop picks,
# the 256 raster is the fallback for anything that will not render SVG.
install(FILES ${CMAKE_SOURCE_DIR}/packaging/nirbija.svg
        DESTINATION share/icons/hicolor/scalable/apps)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/nirbija.png
        DESTINATION share/icons/hicolor/256x256/apps)
