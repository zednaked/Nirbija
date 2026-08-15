pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// What the keyboard and the mouse can do, in one place.
//
// The mixer grew a fader you can drive with the arrow keys, a wheel that steps
// by a decibel and a Shift that makes every drag ten times finer, and none of
// that announces itself. A hint on hover explains one control; this explains
// the instrument.
Popup {
    id: root

    width: Skin.px(520)
    height: Math.min(Skin.px(560), Skin.px(90) + rows.length * Skin.px(26))
    modal: true
    anchors.centerIn: Overlay.overlay
    padding: Skin.spacingL
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Skin.popup
        border.width: 1
        border.color: Skin.border
        radius: Skin.radiusL
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Skin.fast }
    }

    // A heading is a row with no key on it.
    readonly property var rows: [
        { key: "", what: qsTr("Transport") },
        { key: qsTr("Space"), what: qsTr("Play or stop") },
        { key: qsTr("Ctrl+Z / Ctrl+Shift+Z"), what: qsTr("Undo, redo") },
        { key: qsTr("Esc"), what: qsTr("Cancel a MIDI learn") },
        { key: qsTr("F1"), what: qsTr("This sheet") },

        { key: "", what: qsTr("Faders, pans, sends and plugin parameters") },
        { key: qsTr("Drag"), what: qsTr("Move the control") },
        { key: qsTr("Shift+drag"), what: qsTr("Ten times finer") },
        { key: qsTr("Wheel"), what: qsTr("One step — a decibel on a fader") },
        { key: qsTr("Shift+wheel"), what: qsTr("A fifth of a step") },
        { key: qsTr("Double-click"), what: qsTr("Back to unity, or centre for a pan") },
        { key: qsTr("Tab"), what: qsTr("Move between controls") },
        { key: qsTr("↑ ↓ ← →"), what: qsTr("Move the focused control") },
        { key: qsTr("Page ↑ / ↓"), what: qsTr("Six decibels at a time") },
        { key: qsTr("Home / End"), what: qsTr("Unity, or silence") },

        { key: "", what: qsTr("Slots") },
        { key: qsTr("Click"), what: qsTr("Open the editor, or fill an empty slot") },
        { key: qsTr("Right-click, or hold"), what: qsTr("Bypass, reorder, remove, route") },
        { key: qsTr("Scroll sideways"), what: qsTr("Move along the mixer") }
    ]

    contentItem: ColumnLayout {
        spacing: Skin.spacing

        RowLayout {
            Layout.fillWidth: true

            Text {
                Layout.fillWidth: true
                text: qsTr("Shortcuts")
                color: Skin.text
                font.pixelSize: Skin.fontL
                font.bold: true
            }

            StripButton {
                Layout.preferredWidth: Skin.px(64)
                label: qsTr("Close")
                onClicked: root.close()
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.rows
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar {
                policy: list.contentHeight > list.height ? ScrollBar.AsNeeded
                                                         : ScrollBar.AlwaysOff
            }

            delegate: Item {
                id: entry
                required property var modelData
                readonly property bool heading: modelData.key.length === 0

                width: list.width
                height: entry.heading ? Skin.px(30) : Skin.px(22)

                Text {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: entry.heading ? Skin.spacingXS : 0
                    width: Skin.px(180)
                    visible: !entry.heading
                    text: entry.modelData.key
                    color: Skin.textDim
                    font.pixelSize: Skin.fontS
                    font.family: Skin.monoFamily
                }

                Text {
                    anchors.left: entry.heading ? parent.left : undefined
                    anchors.leftMargin: entry.heading ? 0 : 0
                    x: entry.heading ? 0 : Skin.px(190)
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: entry.heading ? Skin.spacingXS : 0
                    text: entry.modelData.what
                    color: entry.heading ? Skin.text : Skin.textDim
                    font.pixelSize: entry.heading ? Skin.font : Skin.fontS
                    font.bold: entry.heading
                }

                Rectangle {
                    visible: entry.heading
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: Skin.border
                    opacity: 0.5
                }
            }
        }
    }
}
