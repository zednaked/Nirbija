import QtQuick
import QtQuick.Controls.Basic

// The list that opens when an empty insert slot is tapped. Filtering happens on
// the QML side because the scan is already in memory.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1

    width: 380
    height: 460
    modal: true
    anchors.centerIn: Overlay.overlay
    padding: 0

    background: Rectangle {
        color: Skin.strip
        border.width: 1
        border.color: Skin.line
        radius: Skin.radius
    }

    onOpened: {
        search.text = ""
        search.forceActiveFocus()
    }

    Column {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        TextField {
            id: search
            width: parent.width
            placeholderText: qsTr("Search %1 plugins").arg(mixer.plugins.count)
            color: Skin.text
            placeholderTextColor: Skin.textDim
            font.pixelSize: 13

            background: Rectangle {
                color: Skin.slotEmpty
                radius: Skin.radius
                border.width: 1
                border.color: Skin.line
            }
        }

        ListView {
            id: list
            width: parent.width
            height: parent.height - search.height - 8
            clip: true
            model: mixer.plugins

            delegate: Rectangle {
                width: list.width
                height: visible ? 38 : 0
                // An empty query shows everything, which is the common case when
                // the user just wants to browse.
                visible: search.text.length === 0
                         || name.toLowerCase().includes(search.text.toLowerCase())
                color: hover.hovered ? Skin.slot : "transparent"

                HoverHandler {
                    id: hover
                }

                Column {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 8
                    spacing: 1

                    Text {
                        text: name
                        color: Skin.text
                        font.pixelSize: 12
                    }

                    Text {
                        text: vendor.length > 0 ? vendor : qsTr("unknown vendor")
                        color: Skin.textDim
                        font.pixelSize: 10
                    }
                }

                Text {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: 8
                    text: format
                    color: Skin.textDim
                    font.pixelSize: 10
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        mixer.addInsert(root.targetRow, index)
                        root.close()
                    }
                }
            }
        }
    }
}
