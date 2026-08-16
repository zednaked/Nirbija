import QtQuick
import QtQuick.Controls.Basic
import Nirbija

// One effect slot in a strip's insert chain. Empty slots read as a thin outline
// and fill on tap; right-click or a long press opens what can be done to a
// loaded one, matching AUM's gesture for acting on a slot without a menu.
AbstractButton {
    id: root

    property string pluginName: ""
    property bool bypassed: false
    property bool postFader: false
    readonly property bool empty: pluginName.length === 0

    signal menuRequested

    implicitHeight: Skin.slotHeight
    padding: Skin.spacingS
    // Clears the bypass mark down the left edge.
    leftPadding: Skin.spacing + Skin.spacingXS
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.role: Accessible.Button
    Accessible.name: root.empty ? qsTr("Empty insert slot") : root.pluginName
    Accessible.onPressAction: root.clicked()

    // An empty slot and a filled one answer different questions: one is "what
    // goes here", the other "what is this and how do I get at it".
    Tip {
        text: root.empty
              ? qsTr("Empty insert slot. Click to load a plugin here.")
              : qsTr("%1 — click to open its editor. Right-click or hold for bypass, reorder and remove.").arg(root.pluginName)
        visible: root.hovered
    }

    background: Rectangle {
        radius: Skin.radius
        color: root.down || root.hovered
               ? Skin.slotHover
               : (root.empty ? Skin.slotEmpty : Skin.slot)
        border.width: 1
        border.color: root.visualFocus ? Skin.focus
                    : root.empty ? Skin.line
                    : Skin.border

        Behavior on color {
            ColorAnimation { duration: Skin.fast }
        }

        // A bypassed insert is still in the chain and still costs the CPU it
        // costs; it just is not heard. Saying so on the slot is cheaper than
        // opening the menu to find out.
        Rectangle {
            visible: !root.empty && root.bypassed
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 1
            width: Skin.px(3)
            radius: Skin.radiusS
            color: Skin.mute
        }
    }

    contentItem: Item {
        Text {
            id: nameText
            anchors.left: parent.left
            anchors.right: tag.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: Skin.spacingXS
            text: root.empty ? "+" : root.pluginName
            color: root.empty ? Skin.textDim
                 : root.bypassed ? Skin.disabled
                 : Skin.text
            font.pixelSize: root.empty ? Skin.fontXL : Skin.font
            horizontalAlignment: root.empty ? Text.AlignHCenter : Text.AlignLeft
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            wrapMode: Text.NoWrap
        }

        // Where the insert sits against the fader. Pre is the common case and
        // says nothing; post is worth a mark, because it is the one that makes
        // an insert follow the fader instead of feeding it.
        Text {
            id: tag
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            visible: !root.empty && root.postFader
            width: visible ? implicitWidth : 0
            text: qsTr("post")
            color: Skin.textDim
            font.pixelSize: Skin.fontXS
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onSingleTapped: root.menuRequested()
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onLongPressed: root.menuRequested()
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Menu) {
            root.menuRequested()
            event.accepted = true
        }
    }
}
