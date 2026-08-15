pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// The session at a glance: every strip with its chain, one line each. Clicking
// a strip scrolls the mixer to it; clicking a plugin opens its editor. The eye
// closes every open editor at once, which is the fastest way out of a desk
// buried in plugin windows.
Popup {
    id: root

    signal jumpTo(int row)
    signal openInsert(int row, int slot)

    width: Skin.px(380)
    height: Math.min(Skin.px(560), Skin.px(100) + list.count * Skin.px(48))
    modal: true
    padding: Skin.spacing
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

    contentItem: ColumnLayout {
        spacing: Skin.spacingS

        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingS

            Text {
                text: qsTr("Session")
                color: Skin.text
                font.pixelSize: Skin.fontL
                font.bold: true
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("%n strip(s)", "", Mixer.rowCount())
                color: Skin.textDim
                font.pixelSize: Skin.fontS
            }

            StripButton {
                Layout.preferredWidth: Skin.px(84)
                label: qsTr("close UIs")
                tip: qsTr("Close every plugin editor this session has open.")
                onClicked: Mixer.closeAllEditors()
            }
        }

        Text {
            Layout.fillWidth: true
            visible: list.count === 0
            text: qsTr("Nothing here yet. The square at the end of the mixer adds a strip.")
            color: Skin.textDim
            font.pixelSize: Skin.font
            wrapMode: Text.WordWrap
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Skin.spacingS
            model: Mixer
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true

            ScrollBar.vertical: ScrollBar {
                policy: list.contentHeight > list.height ? ScrollBar.AsNeeded
                                                         : ScrollBar.AlwaysOff
            }

            delegate: Rectangle {
                id: strip
                // Bound explicitly rather than picked up from the delegate's
                // context: the inner Repeater declares an `index` of its own,
                // and the old code had to stash this one in a property before
                // it was shadowed.
                required property int index
                required property string name
                required property bool isBus
                required property string outputLabel
                required property color accent
                required property var insertDetails

                width: list.width
                height: Skin.px(44)
                radius: Skin.radius
                color: rowHover.hovered ? Skin.slot : Skin.slotEmpty
                border.width: 1
                border.color: Skin.line

                HoverHandler {
                    id: rowHover
                }

                TapHandler {
                    onSingleTapped: {
                        root.jumpTo(strip.index)
                        root.close()
                    }
                }

                Rectangle {
                    id: chip
                    width: Skin.px(4)
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.margins: Skin.spacingXS + 1
                    radius: Skin.radiusS
                    color: strip.accent
                }

                Column {
                    anchors.left: chip.right
                    anchors.right: parent.right
                    anchors.leftMargin: Skin.spacing
                    anchors.rightMargin: Skin.spacingS
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Skin.spacingXS

                    Row {
                        spacing: Skin.spacingS

                        Text {
                            text: strip.name
                            color: Skin.text
                            font.pixelSize: Skin.font
                        }

                        Text {
                            visible: strip.isBus
                            text: qsTr("bus")
                            color: Skin.textDim
                            font.pixelSize: Skin.fontS
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            text: "→ " + strip.outputLabel
                            color: Skin.textDim
                            font.pixelSize: Skin.fontS
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    // The chain, each plugin its own tap target.
                    Row {
                        spacing: Skin.spacingS

                        Repeater {
                            model: strip.insertDetails

                            Rectangle {
                                id: chainEntry
                                required property int index
                                required property var modelData

                                visible: modelData.name.length > 0
                                width: visible ? insertLabel.implicitWidth
                                                 + Skin.spacing : 0
                                height: Skin.px(16)
                                radius: Skin.radiusS
                                color: insertHover.hovered ? Skin.accent
                                     : modelData.bypassed ? Skin.slotEmpty
                                     : Skin.slot
                                border.width: modelData.bypassed ? 1 : 0
                                border.color: Skin.mute

                                HoverHandler {
                                    id: insertHover
                                }

                                Text {
                                    id: insertLabel
                                    anchors.centerIn: parent
                                    text: chainEntry.modelData.name
                                    color: insertHover.hovered ? Skin.onAccent
                                         : chainEntry.modelData.bypassed
                                           ? Skin.disabled : Skin.text
                                    font.pixelSize: Skin.fontXS
                                }

                                TapHandler {
                                    onSingleTapped: root.openInsert(strip.index,
                                                                   chainEntry.index)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
