import QtQuick

// The small square toggles on a strip: mute, solo, record-arm.
Rectangle {
    id: root

    property string label: ""
    property bool active: false
    property color activeColor: Skin.accent

    signal clicked

    radius: Skin.radius
    color: active ? activeColor : Skin.slot
    border.width: 1
    border.color: active ? Qt.lighter(activeColor, 1.2) : Skin.line

    Text {
        anchors.centerIn: parent
        text: root.label
        color: root.active ? "#14100a" : Skin.textDim
        font.pixelSize: 11
        font.bold: root.active
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.clicked()
    }
}
