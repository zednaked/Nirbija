import QtQuick

// The input and output node slots that cap a channel strip. Each carries a
// label and its own three-dot peak meter.
Rectangle {
    id: root

    property string label: ""
    property real level: 0
    property bool filled: true

    signal clicked
    signal menuRequested

    height: Skin.slotHeight
    radius: Skin.radius
    color: filled ? Skin.slot : Skin.slotEmpty
    border.width: 1
    border.color: Skin.line

    Column {
        anchors.fill: parent
        anchors.margins: 5
        spacing: 3

        Text {
            width: parent.width
            text: root.label
            color: root.filled ? Skin.text : Skin.textDim
            font.pixelSize: 11
            elide: Text.ElideRight
        }

        PeakDots {
            level: root.level
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: mouse => {
            if (mouse.button === Qt.RightButton)
                root.menuRequested()
            else
                root.clicked()
        }
        onPressAndHold: root.menuRequested()
    }
}
