// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls.Basic
import Nirbija

// The window every built-in editor is: a panel over the mixer that is not
// modal - faders and the transport stay reachable with it open - which can
// be dragged anywhere by its empty chrome, and which glows along its bottom
// edge in the colour of what its plugin is doing, as bright as that is loud.
//
// The looper, the sampler, the drone, the sequencer and the pads each used
// to carry this chrome in full: the same gradient, the same drag maths, the
// same wheel-eating handlers, six times over. What differs per editor is
// only the glow's colour and how bright it is, and both are bindings here.
Popup {
    id: root

    // The glow's colour and strength. 0 opacity is no glow at all.
    property color glowHue: Skin.accent
    property real glowOpacity: 0
    // While true the popup ignores Escape and clicks outside - for a MIDI
    // learn in progress, where either would cancel it by accident.
    property bool holdOpen: false
    // Whether the panel eats the wheel over its empty chrome, so scrolling
    // over a bar does not also scroll the mixer under the window.
    property bool eatsWheel: true
    // Set once, the first time this ever opens - after that the popup stays
    // wherever it was last dragged, the same as a real tool window would.
    property bool positioned: false

    modal: false
    padding: Skin.spacingL
    closePolicy: root.holdOpen ? Popup.NoAutoClose : Popup.CloseOnEscape

    // Centres the panel over the overlay the first time, and keeps it inside
    // the overlay every time after - a window resized down since the last
    // open can leave a remembered position past its own edge. Not clamped on
    // the first placement: a Popup reports the position it was last shown
    // at, which before it has ever been shown is 0, and clamping through
    // that read wrote 0 back over the centre just computed (this is the bug
    // that used to open the sequencer in the corner).
    function place() {
        const ov = Overlay.overlay
        if (!root.positioned) {
            // Clamped: opened before the window has its size, the centre of
            // a zero-sized overlay is off the top-left corner.
            root.x = Math.max(0, Math.round(((ov ? ov.width : 0) - root.width) / 2))
            root.y = Math.max(0, Math.round(((ov ? ov.height : 0) - root.height) / 2))
            root.positioned = true
            return
        }
        root.clampPos()
    }

    function clampPos() {
        const ov = Overlay.overlay
        if (!ov) return
        root.x = Math.max(0, Math.min(root.x, ov.width - root.width))
        root.y = Math.max(0, Math.min(root.y, ov.height - root.height))
    }

    background: Rectangle {
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.lighter(Skin.popup, 1.08) }
            GradientStop { position: 1.0; color: Skin.popup }
        }
        border.width: 1
        border.color: Skin.border
        radius: Skin.radiusL

        // The glow: light under a door, in the state's colour.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: parent.height * 0.4
            radius: parent.radius
            visible: opacity > 0.001
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: root.glowHue }
            }
            opacity: root.glowOpacity
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        // Empty chrome is not a handler on its own, so without this a press
        // on the padding falls through onto the strip behind - and, since
        // the popup can sit anywhere, doubles as how it moves.
        HoverHandler {}
        TapHandler {}
        DragHandler {
            target: null
            grabPermissions: PointerHandler.TakeOverForbidden
            onCentroidChanged: if (active) {
                const nx = root.x + centroid.position.x - centroid.pressPosition.x
                const ny = root.y + centroid.position.y - centroid.pressPosition.y
                const maxX = Overlay.overlay
                    ? Math.max(0, Overlay.overlay.width - root.width) : nx
                const maxY = Overlay.overlay
                    ? Math.max(0, Overlay.overlay.height - root.height) : ny
                root.x = Math.max(0, Math.min(nx, maxX))
                root.y = Math.max(0, Math.min(ny, maxY))
            }
        }
        WheelHandler {
            enabled: root.eatsWheel
            acceptedModifiers: Qt.NoModifier
            onWheel: event => event.accepted = true
        }
        WheelHandler {
            enabled: root.eatsWheel
            acceptedModifiers: Qt.ShiftModifier
            onWheel: event => event.accepted = true
        }
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Skin.fast }
    }
}
