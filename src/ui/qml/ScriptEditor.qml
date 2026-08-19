pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// Where you write the Script plugin's Lua.
//
// Applying is a deliberate act rather than something that happens as you type:
// compiling runs the script, and a half-written line is a script that does the
// wrong thing rather than one that fails to compile.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property string message: ""
    property bool failed: false
    // Set once, the first time this ever opens - after that the popup stays
    // wherever it was last dragged, the same as a real tool window would.
    property bool positioned: false

    // 680x560 is bigger than the window's own minimum (660x520), so at a high
    // UI scale, or a window resized down toward that minimum, this popup
    // could ask for more room than the window has to give it.
    width: Math.min(Px.px(680),
                    Overlay.overlay ? Overlay.overlay.width - Px.px(24)
                                     : Px.px(680))
    height: Math.min(Px.px(560),
                     Overlay.overlay ? Overlay.overlay.height - Px.px(24)
                                      : Px.px(560))
    // Not modal: the mixer behind it stays live, so a fader or the transport
    // is still reachable with this open - the whole point of it being a tool
    // window rather than a dialog. Dragging the empty background moves it;
    // see the DragHandler below.
    modal: false
    padding: Skin.spacingL
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        color: Skin.popup
        border.width: 1
        border.color: Skin.border
        radius: Skin.radiusL

        // Empty chrome is not a handler on its own, so without this a press
        // on the padding falls through onto the strip behind - and, now
        // that the popup can sit anywhere, doubles as how it moves.
        HoverHandler {}
        TapHandler {}
        DragHandler {
            target: null
            grabPermissions: PointerHandler.TakeOverForbidden
            onCentroidChanged: if (active) {
                const nx = root.x + centroid.position.x - centroid.pressPosition.x
                const ny = root.y + centroid.position.y - centroid.pressPosition.y
                const maxX = Overlay.overlay
                    ? Math.max(0, Overlay.overlay.width - root.width) : nx
                const maxY = Overlay.overlay
                    ? Math.max(0, Overlay.overlay.height - root.height) : ny
                root.x = Math.max(0, Math.min(nx, maxX))
                root.y = Math.max(0, Math.min(ny, maxY))
            }
        }
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Skin.fast }
    }

    function openFor(row, slot) {
        root.targetRow = row
        root.targetSlot = slot
        editor.text = Mixer.insertScript(row, slot)
        const failure = Mixer.insertScriptError(row, slot)
        root.failed = failure.length > 0
        root.message = failure.length > 0 ? failure : qsTr("running")
        if (!root.positioned) {
            root.x = Math.round((Overlay.overlay.width - root.width) / 2)
            root.y = Math.round((Overlay.overlay.height - root.height) / 2)
            root.positioned = true
        }
        root.open()
    }

    function apply() {
        const ok = Mixer.setInsertScript(root.targetRow, root.targetSlot,
                                         editor.text)
        const failure = Mixer.insertScriptError(root.targetRow, root.targetSlot)
        root.failed = !ok
        root.message = ok ? qsTr("running") : failure
    }

    contentItem: ColumnLayout {
        spacing: Skin.spacingS

        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacing

            Text {
                text: qsTr("Script")
                color: Skin.text
                font.pixelSize: Skin.fontL
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            Text {
                text: qsTr("Ctrl+Return applies")
                color: Skin.textDim
                font.pixelSize: Skin.fontS
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Skin.slotEmpty
            radius: Skin.radius
            border.width: 1
            border.color: editor.activeFocus ? Skin.focus : Skin.border
            clip: true

            ScrollView {
                anchors.fill: parent
                anchors.margins: Skin.spacingS

                TextArea {
                    id: editor
                    color: Skin.text
                    font.family: Skin.monoFamily
                    font.pixelSize: Skin.font
                    selectByMouse: true
                    wrapMode: TextEdit.NoWrap
                    background: null

                    Keys.onPressed: event => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                && (event.modifiers & Qt.ControlModifier)) {
                            root.apply()
                            event.accepted = true
                        }
                    }
                }
            }
        }

        // Whatever Lua said, in its own words, including the line number. A
        // paraphrase would be less use than the message the language wrote.
        Text {
            Layout.fillWidth: true
            text: root.message
            color: root.failed ? Skin.mute : Skin.textDim
            font.family: Skin.monoFamily
            font.pixelSize: Skin.fontS
            wrapMode: Text.WordWrap
            maximumLineCount: 3
            elide: Text.ElideRight
        }

        // The four knobs the script is handed. They are ordinary parameters, so
        // MIDI learn reaches them and a controller can play the script.
        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: Skin.spacing

            Repeater {
                model: 4

                ValueTrack {
                    id: knob
                    required property int index

                    Layout.fillWidth: true
                    Layout.preferredHeight: Px.px(22)
                    label: qsTr("knob %1").arg(knob.index + 1)
                    valueText: knob.value.toFixed(2)
                    value: 0
                    onMoved: v => {
                        knob.value = v
                        Mixer.setInsertParameter(root.targetRow, root.targetSlot,
                                                 knob.index, v)
                        // Moving a knob rebuilds the tables, which can fail on
                        // a script that only breaks at some values.
                        const failure = Mixer.insertScriptError(root.targetRow,
                                                                root.targetSlot)
                        root.failed = failure.length > 0
                        root.message = root.failed ? failure : qsTr("running")
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacing

            Item { Layout.fillWidth: true }

            StripButton {
                Layout.preferredWidth: Px.px(80)
                label: qsTr("apply")
                onClicked: root.apply()
            }

            StripButton {
                Layout.preferredWidth: Px.px(80)
                label: qsTr("close")
                onClicked: root.close()
            }
        }
    }
}
