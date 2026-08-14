import QtQuick

// One effect slot in a strip's insert chain. Empty slots read as a thin outline
// and fill on tap; a long press removes the plugin, matching AUM's gesture for
// acting on a slot without a menu.
Rectangle {
    id: root

    property string pluginName: ""
    readonly property bool empty: pluginName.length === 0

    signal clicked
    signal longPressed

    height: Skin.slotHeight
    radius: Skin.radius
    color: empty ? Skin.slotEmpty : Skin.slot
    border.width: 1
    border.color: empty ? Skin.line : Qt.lighter(Skin.slot, 1.4)

    Text {
        anchors.fill: parent
        anchors.margins: 5
        verticalAlignment: Text.AlignVCenter
        text: root.empty ? "+" : root.pluginName
        color: root.empty ? Skin.textDim : Skin.text
        font.pixelSize: root.empty ? 15 : 11
        horizontalAlignment: root.empty ? Text.AlignHCenter : Text.AlignLeft
        elide: Text.ElideRight
        wrapMode: Text.NoWrap
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.clicked()
        onPressAndHold: root.longPressed()
    }
}
