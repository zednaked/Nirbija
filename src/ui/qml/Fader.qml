pragma ComponentBehavior: Bound

import QtQuick
import Nirbija

// A vertical fader with the level meter running alongside it, the way a mixer
// strip reads: the meter answers the fader right next to it.
//
// The fader is dragged relative to where it was grabbed, not jumped to wherever
// the pointer landed. Jumping is how a click near the top of a strip becomes a
// sudden +6 dB, and it also makes small corrections impossible: the travel is
// 76 dB over a couple of hundred pixels, so a pixel is a third of a decibel.
// Holding Shift while dragging divides that by ten, the wheel steps by a
// decibel, and the arrow keys do the same without a pointer at all.
Item {
    id: root

    property real gain: 1.0
    property real positionLeft: 0
    property real positionRight: 0
    property real holdLeft: 0
    property real holdRight: 0
    property color accent: Skin.accent
    // Numbered marks beside the travel. Off in a channel strip, where there is
    // no room for them, on in the master, where there is.
    property bool showScale: false
    property string tip: qsTr("Level, in decibels rather than in amplitude, so most of the travel covers the top of the range. Drag to move it, Shift-drag for fine, scroll for one decibel, double-click for unity.")

    signal gainRequested(real gain)

    // Fader travel is in decibels, so the handle position is not the gain.
    readonly property real position: Mixer.gainToFader(root.gain)
    // A decibel as a fraction of the whole travel: the step every keyboard and
    // wheel gesture is built from.
    readonly property real decibel: 1.0 / 76.0

    implicitWidth: Skin.px(62)
    activeFocusOnTab: true

    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Level")
    // The reading goes in the description: the attached Accessible type has no
    // property for the value of a ranged control.
    Accessible.description: Mixer.gainLabel(root.gain) + ". " + root.tip

    function moveTo(newPosition) {
        root.gainRequested(Mixer.faderToGain(Math.max(0, Math.min(1, newPosition))))
    }
    function nudge(steps) {
        root.moveTo(root.position + steps * root.decibel)
    }

    HoverHandler {
        id: faderHover
    }

    Tip {
        text: root.tip
        visible: root.tip.length > 0 && faderHover.hovered
    }

    Row {
        anchors.fill: parent
        spacing: Skin.spacingS

        // --- the dB scale ----------------------------------------------------
        Item {
            id: scale
            visible: root.showScale
            width: visible ? Skin.px(22) : 0
            height: parent.height

            Repeater {
                model: root.showScale ? Skin.faderTicks() : []

                Text {
                    id: tickLabel
                    required property var modelData
                    width: scale.width - Skin.spacingXS
                    horizontalAlignment: Text.AlignRight
                    text: modelData.label
                    color: modelData.major ? Skin.textDim : Skin.disabled
                    font.pixelSize: Skin.fontXS
                    font.family: Skin.monoFamily
                    y: track.height - modelData.position * track.height
                       - height / 2
                }
            }
        }

        // --- fader track -----------------------------------------------------
        Item {
            id: track
            width: Skin.px(26)
            height: parent.height

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Skin.px(5)
                height: parent.height
                radius: width / 2
                color: Skin.meterTrack
                border.width: 1
                border.color: Skin.line
            }

            // Marks along the travel, so the fader can be read at a glance
            // rather than only in the number under it. Unity is drawn wider —
            // it is the one a hand looks for.
            Repeater {
                model: Skin.faderTicks()

                Rectangle {
                    required property var modelData
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: modelData.major ? Skin.px(16) : Skin.px(10)
                    height: 1
                    color: modelData.major ? Skin.textDim : Skin.border
                    opacity: modelData.major ? 0.7 : 0.5
                    y: track.height - modelData.position * track.height
                }
            }

            // The travelled part, so the fader reads from the bottom up even
            // when the handle is hard to see against the panel.
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Skin.px(5)
                radius: width / 2
                color: root.accent
                opacity: 0.28
                y: handle.y + handle.height / 2
                height: Math.max(0, track.height - y)
            }

            Rectangle {
                id: handle
                width: Skin.px(24)
                height: Skin.px(14)
                radius: Skin.radiusS
                color: drag.active || fineDrag.active ? Skin.slotHover : Skin.slot
                border.width: root.activeFocus ? 2 : 1
                border.color: root.activeFocus ? Skin.focus : root.accent
                anchors.horizontalCenter: parent.horizontalCenter
                y: Math.max(0, Math.min(track.height - height,
                                        track.height - root.position * track.height
                                        - height / 2))

                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width - Skin.spacing
                    height: 2
                    color: root.accent
                }

                Behavior on color {
                    ColorAnimation { duration: Skin.fast }
                }
            }

            // --- gestures ----------------------------------------------------
            // Two drag handlers, differing only in the modifier they accept:
            // that is how a pointer handler is told about Shift, and it keeps
            // the fine and coarse laws side by side instead of behind an `if`.
            DragHandler {
                id: drag
                target: null
                xAxis.enabled: false
                dragThreshold: 0
                acceptedModifiers: Qt.NoModifier
                property real startPosition: 0
                onActiveChanged: {
                    if (!active) return
                    startPosition = root.position
                    root.forceActiveFocus(Qt.MouseFocusReason)
                }
                onCentroidChanged: {
                    if (!active) return
                    const travelled = centroid.pressPosition.y - centroid.position.y
                    root.moveTo(startPosition + travelled / track.height)
                }
            }

            DragHandler {
                id: fineDrag
                target: null
                xAxis.enabled: false
                dragThreshold: 0
                acceptedModifiers: Qt.ShiftModifier
                property real startPosition: 0
                onActiveChanged: {
                    if (!active) return
                    startPosition = root.position
                    root.forceActiveFocus(Qt.MouseFocusReason)
                }
                onCentroidChanged: {
                    if (!active) return
                    const travelled = centroid.pressPosition.y - centroid.position.y
                    root.moveTo(startPosition + travelled / track.height * 0.1)
                }
            }

            TapHandler {
                acceptedButtons: Qt.LeftButton
                onDoubleTapped: root.gainRequested(1.0)
                onSingleTapped: root.forceActiveFocus(Qt.MouseFocusReason)
            }

            WheelHandler {
                acceptedModifiers: Qt.NoModifier
                onWheel: event => root.nudge(event.angleDelta.y > 0 ? 1 : -1)
            }
            WheelHandler {
                acceptedModifiers: Qt.ShiftModifier
                onWheel: event => root.nudge(event.angleDelta.y > 0 ? 0.2 : -0.2)
            }
        }

        // --- meter -----------------------------------------------------------
        Row {
            spacing: Skin.spacingXS
            height: parent.height

            Meter {
                width: Skin.px(7)
                height: track.height
                position: root.positionLeft
                hold: root.holdLeft
            }

            Meter {
                width: Skin.px(7)
                height: track.height
                position: root.positionRight
                hold: root.holdRight
            }
        }
    }

    Keys.onPressed: event => {
        switch (event.key) {
        case Qt.Key_Up:
            root.nudge((event.modifiers & Qt.ShiftModifier) ? 0.1 : 1)
            event.accepted = true
            break
        case Qt.Key_Down:
            root.nudge((event.modifiers & Qt.ShiftModifier) ? -0.1 : -1)
            event.accepted = true
            break
        case Qt.Key_PageUp:
            root.nudge(6); event.accepted = true; break
        case Qt.Key_PageDown:
            root.nudge(-6); event.accepted = true; break
        case Qt.Key_Home:
            root.gainRequested(1.0); event.accepted = true; break
        case Qt.Key_End:
            root.gainRequested(0.0); event.accepted = true; break
        }
    }
}
