pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-only

import QtQuick
import QtQuick.Controls.Basic
import Nirbija

// A hover hint, skinned to the mixer rather than to the platform.
//
// Built on ToolTip because that is a Popup: it draws in the window's overlay,
// so it is not clipped by the strip or the insert list it belongs to. A plain
// Rectangle child would be cut off by the first ancestor with `clip: true`,
// which on this layout is nearly all of them.
//
// Wrapped in a Loader that only exists while the hint is wanted. Every button,
// slot and fader carries one of these, and a Popup is not a small thing: with
// sixteen strips on screen the mixer held two hundred of them, each with its
// own overlay item, waiting for a hover that mostly never comes. The Loader
// costs nothing until `visible` goes true; the ToolTip is then built, shows
// after its delay, fades out when `visible` drops, and is torn down once the
// fade is over.
Loader {
    id: root

    property string text: ""

    // Takes no room and no input: the hint is drawn in the overlay, this is
    // just where it lives in the tree.
    width: 0
    height: 0
    // The item this hint explains, which a ToolTip positions against.
    readonly property Item host: root.parent

    active: root.visible || linger.running
    onVisibleChanged: if (!visible) linger.restart()

    // Long enough for the exit transition to be seen before the item goes.
    Timer {
        id: linger
        interval: Skin.fast + 60
    }

    sourceComponent: ToolTip {
        id: tip

        parent: root.host
        text: root.text
        visible: root.visible

        // Long enough not to fire while the pointer crosses a row of buttons.
        delay: Skin.tipDelay
        timeout: 9000
        padding: Skin.spacing

        // A hint never takes input: no click of it, no key of it, no focus.
        closePolicy: Popup.NoAutoClose

        // Centred under whatever it explains, flipping above when the window
        // has no room left underneath — the strips run to the bottom edge, so
        // the buttons down there would otherwise point their hint off-screen.
        x: root.host ? (root.host.width - width) / 2 : 0
        y: {
            if (!root.host) return 0
            const gap = Skin.gap
            const below = root.host.height + gap
            const top = root.host.mapToItem(null, 0, 0).y
            const room = root.host.Window.height - top - root.host.height
            return room > implicitHeight + gap ? below : -implicitHeight - gap
        }

        implicitWidth: Math.min(Px.px(300), contentWidth + leftPadding + rightPadding)

        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Skin.fast }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Skin.fast }
        }

        contentItem: Text {
            text: tip.text
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
}
