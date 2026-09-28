// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import Nirbija

// The state of an editor's plugin on a chip in its header: a dot in the
// state's colour, the word for it, and whatever else that editor wants to
// say beside it - the loop's bar.beat, the pad in hand, the pattern queued.
// The dot pulses while something is about to happen or being written.
Rectangle {
    id: root

    // The colour the glow, the tape's border and the playhead agree on.
    property color hue: Skin.accent
    // Pulse: recording, counting in, armed - the states a player watches.
    property bool busy: false
    // Steady and bright, as opposed to dimmed: playing, sounding, running.
    property bool lit: false
    property string label: ""
    property real labelSize: Skin.fontL
    // Where an editor's own readouts go, after the dot and the word.
    default property alias content: row.data

    property real pulse: 1

    implicitHeight: Skin.buttonHeight + Px.px(6)
    radius: Skin.radius
    color: Skin.slotEmpty
    border.width: 1
    border.color: Qt.rgba(root.hue.r, root.hue.g, root.hue.b, 0.6)

    SequentialAnimation on pulse {
        running: root.busy
        loops: Animation.Infinite
        NumberAnimation { from: 1; to: 0.35; duration: 420; easing.type: Easing.InOutSine }
        NumberAnimation { from: 0.35; to: 1; duration: 420; easing.type: Easing.InOutSine }
    }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: Skin.spacing
        anchors.rightMargin: Skin.spacing
        spacing: Skin.spacingS

        Rectangle {
            Layout.preferredWidth: Px.px(10)
            Layout.preferredHeight: Px.px(10)
            radius: width / 2
            color: root.hue
            opacity: root.busy ? root.pulse : root.lit ? 1 : 0.5
        }

        Text {
            text: root.label
            color: root.hue
            font.pixelSize: root.labelSize
            font.bold: true
            font.letterSpacing: Px.px(1)
        }
    }
}
