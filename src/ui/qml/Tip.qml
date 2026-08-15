import QtQuick
import QtQuick.Controls.Basic
import Nirbija

// A hover hint, skinned to the mixer rather than to the platform.
//
// Built on ToolTip because that is a Popup: it draws in the window's overlay,
// so it is not clipped by the strip or the insert list it belongs to. A plain
// Rectangle child would be cut off by the first ancestor with `clip: true`,
// which on this layout is nearly all of them.
ToolTip {
    id: root

    // Long enough not to fire while the pointer crosses a row of buttons.
    delay: Skin.tipDelay
    timeout: 9000
    padding: Skin.spacing

    // A hint never takes input: no click of it, no key of it, no focus.
    closePolicy: Popup.NoAutoClose

    // Centred under whatever it explains, flipping above when the window has
    // no room left underneath — the strips run to the bottom edge, so the
    // buttons down there would otherwise point their hint off-screen.
    x: (parent.width - width) / 2
    y: {
        const gap = Skin.gap
        const below = parent.height + gap
        const top = parent.mapToItem(null, 0, 0).y
        const room = parent.Window.height - top - parent.height
        return room > implicitHeight + gap ? below : -implicitHeight - gap
    }

    implicitWidth: Math.min(Skin.px(300), contentWidth + leftPadding + rightPadding)

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Skin.fast }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Skin.fast }
    }

    contentItem: Text {
        text: root.text
        color: Skin.text
        font.pixelSize: Skin.font
        wrapMode: Text.WordWrap
        lineHeight: 1.25
    }

    background: Rectangle {
        color: Skin.popup
        border.width: 1
        border.color: Skin.border
        radius: Skin.radius
    }
}
