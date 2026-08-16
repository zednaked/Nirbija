import QtQuick
import QtQuick.Controls.Basic
import Nirbija

// The small toggles the mixer is made of: mute, solo, record-arm, and every
// button in the top bar.
//
// An AbstractButton rather than a Rectangle with a MouseArea, which is what
// this was. The control brings the things a hand-rolled rectangle has to be
// told to do one at a time: it takes Space and Return, it can be reached with
// Tab, it reports itself to a screen reader, and it separates "the pointer is
// over it" from "it is being pressed" so both can be shown.
AbstractButton {
    id: root

    property bool active: false
    property color activeColor: Skin.accent
    // What the button does, in words. These are one-glyph buttons; without a
    // hint the only way to learn one is to press it and listen.
    property string tip: ""
    property alias label: root.text
    // Destructive or loud toggles get a hotter resting state so the eye finds
    // them before the hand does.
    property bool danger: false
    // Draws as plain text until it is pointed at. For buttons that are mostly
    // labels — a channel title — where a resting outline would be noise.
    property bool flat: false

    implicitWidth: Math.max(Skin.touchTarget,
                            textItem.implicitWidth + 2 * Skin.spacing)
    implicitHeight: Skin.buttonHeight

    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.role: Accessible.Button
    Accessible.name: root.text
    Accessible.description: root.tip
    Accessible.checkable: true
    Accessible.checked: root.active
    Accessible.onPressAction: root.clicked()

    // The mixer's own hint rather than the style's, so every hint in the app
    // looks like the same thing.
    Tip {
        text: root.tip
        visible: root.tip.length > 0 && root.hovered
    }

    background: Rectangle {
        radius: Skin.radius
        color: root.active ? root.activeColor
             : root.down ? Skin.slotHover
             : root.hovered ? Skin.slotHover
             : root.flat ? "transparent"
             : Skin.slot
        border.width: root.flat && !root.hovered && !root.visualFocus
                      && !root.active ? 0 : 1
        border.color: root.visualFocus ? Skin.focus
                    : root.active ? Qt.lighter(root.activeColor, 1.25)
                    : root.danger ? Qt.darker(Skin.mute, 1.6)
                    : Skin.border
        opacity: root.enabled ? 1.0 : 0.45

        // Pressing has to read as pressing even when the colour barely moves,
        // which on a dark panel it does.
        scale: root.down ? 0.96 : 1.0

        Behavior on color {
            ColorAnimation { duration: Skin.fast }
        }
        Behavior on scale {
            NumberAnimation { duration: Skin.fast; easing.type: Easing.OutQuad }
        }
    }

    contentItem: Text {
        id: textItem
        text: root.text
        color: root.active ? Skin.onAccent
             : root.enabled ? (root.hovered ? Skin.text : Skin.textDim)
             : Skin.disabled
        font.pixelSize: Skin.font
        font.bold: root.active
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight

        Behavior on color {
            ColorAnimation { duration: Skin.fast }
        }
    }
}
