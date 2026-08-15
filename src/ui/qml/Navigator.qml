import QtQuick
import QtQuick.Controls.Basic

// The session at a glance: every strip with its chain, one line each. Clicking
// a strip scrolls the mixer to it; clicking a plugin opens its editor. The eye
// closes every open editor at once, which is the fastest way out of a desk
// buried in plugin windows.
Popup {
    id: root

    signal jumpTo(int row)
    signal openInsert(int row, int slot)

    width: 340
    height: Math.min(520, 90 + list.count * 46)
    modal: true
    padding: 8

    background: Rectangle {
        color: Skin.strip
        border.width: 1
        border.color: Skin.line
        radius: Skin.radius
    }

    Column {
        anchors.fill: parent
        spacing: 6

        Row {
            width: parent.width
            spacing: 6

            Text {
                text: qsTr("Session")
                color: Skin.text
                font.pixelSize: 13
                font.bold: true
                anchors.verticalCenter: parent.verticalCenter
            }

            Item {
                width: parent.width - 140
                height: 1
            }

            StripButton {
                width: 60
                height: 24
                label: qsTr("close UIs")
                onClicked: mixer.closeAllEditors()
            }
        }

        ListView {
            id: list
            width: parent.width
            height: parent.height - 34
            clip: true
            spacing: 4
            model: mixer
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                // The inner Repeater shadows `index`, so the row's own index is
                // pinned here before it disappears.
                readonly property int rowIndex: index
                width: list.width
                height: 42
                radius: Skin.radius
                color: rowHover.hovered ? Skin.slot : Skin.slotEmpty
                border.width: 1
                border.color: Skin.line

                HoverHandler {
                    id: rowHover
                }

                Rectangle {
                    id: chip
                    width: 4
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.margins: 3
                    radius: 2
                    color: model.accent
                }

                Column {
                    anchors.left: chip.right
                    anchors.right: parent.right
                    anchors.leftMargin: 8
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    Row {
                        spacing: 6

                        Text {
                            text: model.name
                            color: Skin.text
                            font.pixelSize: 12
                        }

                        Text {
                            visible: model.isBus
                            text: qsTr("bus")
                            color: Skin.textDim
                            font.pixelSize: 10
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            text: "→ " + model.outputLabel
                            color: Skin.textDim
                            font.pixelSize: 10
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    // The chain, each plugin its own tap target.
                    Row {
                        spacing: 4

                        Repeater {
                            model: inserts

                            Rectangle {
                                visible: modelData.length > 0
                                width: visible ? insertLabel.implicitWidth + 10 : 0
                                height: 16
                                radius: 2
                                color: insertHover.hovered ? Skin.accent : Skin.slot

                                HoverHandler {
                                    id: insertHover
                                }

                                Text {
                                    id: insertLabel
                                    anchors.centerIn: parent
                                    text: modelData
                                    color: Skin.text
                                    font.pixelSize: 9
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: root.openInsert(rowIndex, index)
                                }
                            }
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    z: -1
                    onClicked: {
                        root.jumpTo(index)
                        root.close()
                    }
                }
            }
        }
    }
}
