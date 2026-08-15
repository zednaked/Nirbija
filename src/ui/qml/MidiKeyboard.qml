pragma ComponentBehavior: Bound

import QtQuick
import Nirbija

// One octave of notes for the strip it sits in: press a key, hear whether the
// synth or sampler in the chain is actually making sound. Velocity is fixed and
// there are no black keys — at a strip's width a semitone would be six pixels,
// and this is an audition control, not an instrument.
//
// Everything is sized off the strip rather than in fixed pixels. The keys used
// to be 28 px each with a 36 px column of octave buttons beside them, which
// wanted 246 px inside a 102 px strip: the last four keys and both buttons
// landed outside the strip, over the panel next door.
Rectangle {
    id: root

    property int targetRow: 0
    property int octave: 4

    implicitWidth: Skin.stripWidth - 2 * Skin.gap
    implicitHeight: Skin.px(56)

    color: Skin.slotEmpty
    border.width: 1
    border.color: Skin.line
    radius: Skin.radius
    clip: true

    readonly property var whites: [0, 2, 4, 5, 7, 9, 11]
    readonly property var names: ["C", "D", "E", "F", "G", "A", "B"]

    HoverHandler {
        id: hover
    }

    Tip {
        text: qsTr("Test notes for whatever is in this strip's chain, at fixed velocity. The note is injected straight into the channel, so no MIDI source has to be connected. Drag across the keys to run up the octave.")
        visible: hover.hovered
    }

    function noteOn(semitone) {
        Mixer.sendNote(root.targetRow, root.octave * 12 + semitone, 100)
    }
    function noteOff(semitone) {
        Mixer.sendNote(root.targetRow, root.octave * 12 + semitone, 0)
    }

    Column {
        anchors.fill: parent
        anchors.margins: Skin.spacingXS + 1
        spacing: Skin.spacingXS

        Row {
            id: keys
            width: parent.width
            // Whatever is left once the octave row has had its share. Clamped
            // because the strip collapses this to zero height on a bus.
            height: Math.max(0, parent.height - octaveRow.height - parent.spacing)
            spacing: 1

            Repeater {
                model: 7

                Rectangle {
                    id: key
                    required property int index

                    width: (keys.width - keys.spacing * 6) / 7
                    height: keys.height
                    radius: Skin.radiusS
                    color: press.active ? Skin.accent
                         : keyHover.hovered ? Skin.slotHover
                         : Skin.slot

                    Behavior on color {
                        ColorAnimation { duration: Skin.fast }
                    }

                    Text {
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 1
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: root.names[key.index]
                        color: press.active ? Skin.onAccent : Skin.textDim
                        font.pixelSize: Skin.fontXS
                    }

                    HoverHandler {
                        id: keyHover
                    }

                    // A PointHandler rather than a tap: what matters is how long
                    // the key is held, and a handler that does not take an
                    // exclusive grab lets a finger slide from one key to the
                    // next and sound both.
                    PointHandler {
                        id: press
                        acceptedButtons: Qt.LeftButton
                        onActiveChanged: {
                            if (active)
                                root.noteOn(root.whites[key.index])
                            else
                                root.noteOff(root.whites[key.index])
                        }
                    }
                }
            }
        }

        // The octave reads back between its two buttons: pressing oct+ with
        // nothing to show for it left you counting your own clicks.
        Row {
            id: octaveRow
            width: parent.width
            height: Skin.px(16)
            spacing: Skin.spacingXS

            StripButton {
                width: Skin.px(20)
                height: parent.height
                label: "−"
                tip: qsTr("An octave down.")
                onClicked: root.octave = Math.max(1, root.octave - 1)
            }

            Text {
                width: parent.width - 2 * (Skin.px(20) + Skin.spacingXS)
                height: parent.height
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: "C" + root.octave
                color: Skin.textDim
                font.pixelSize: Skin.fontXS
                font.family: Skin.monoFamily
            }

            StripButton {
                width: Skin.px(20)
                height: parent.height
                label: "+"
                tip: qsTr("An octave up.")
                onClicked: root.octave = Math.min(7, root.octave + 1)
            }
        }
    }
}
