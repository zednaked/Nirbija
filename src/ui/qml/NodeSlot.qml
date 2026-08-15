import QtQuick
import QtQuick.Controls.Basic
import Nirbija

// The input and output node slots that cap a channel strip. Each carries a
// label and its own three-dot peak meter.
AbstractButton {
    id: root

    property alias label: root.text
    // Where this slot's signal sits on the fader's travel, for the dots.
    property real position: 0
    property bool filled: true
    property string tip: ""

    signal menuRequested

    implicitHeight: Skin.slotHeight
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.role: Accessible.Button
    Accessible.name: root.text
    Accessible.description: root.tip
    Accessible.onPressAction: root.clicked()

    Tip {
        text: root.tip
        visible: root.tip.length > 0 && root.hovered
    }

    background: Rectangle {
        radius: Skin.radius
        color: root.down ? Skin.slotHover
             : root.hovered ? Skin.slotHover
             : root.filled ? Skin.slot
             : Skin.slotEmpty
        border.width: 1
        border.color: root.visualFocus ? Skin.focus
                    : root.filled ? Skin.border
                    : Skin.line

        Behavior on color {
            ColorAnimation { duration: Skin.fast }
        }
    }

    contentItem: Column {
        spacing: Skin.spacingXS

        Text {
            width: parent.width
            text: root.text
            color: root.filled ? Skin.text : Skin.textDim
            font.pixelSize: Skin.font
            elide: Text.ElideRight
        }

        PeakDots {
            position: root.position
        }
    }

    // Right-click and long press both reach the menu, so the gesture works with
    // a mouse and with a touchscreen.
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
