import QtQuick

// One send: the bus it feeds and how much of the strip goes there. The bar is
// the level, dragged sideways, so the amount is visible without a number.
Rectangle {
    id: root

    property string busName: ""
    property real level: 0

    signal levelRequested(real level)
    signal menuRequested

    height: 20
    radius: Skin.radius
    color: Skin.slotEmpty
    border.width: 1
    border.color: Skin.line

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: 1
        width: Math.max(0, Math.min(1, root.level)) * (parent.width - 2)
        radius: Skin.radius
        color: Skin.accent
        opacity: 0.35
    }

    Text {
        anchors.fill: parent
        anchors.leftMargin: 5
        anchors.rightMargin: 5
        verticalAlignment: Text.AlignVCenter
        text: "→ " + root.busName
        color: Skin.text
        font.pixelSize: 10
        elide: Text.ElideRight
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton

        onPressed: mouse => {
            if (mouse.button === Qt.RightButton) {
                root.menuRequested()
                return
            }
            root.levelRequested(mouse.x / width)
        }
        onPositionChanged: mouse => {
            if (pressed)
                root.levelRequested(Math.max(0, Math.min(1, mouse.x / width)))
        }
        onPressAndHold: root.menuRequested()
    }
}
