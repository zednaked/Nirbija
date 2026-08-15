import QtQuick
import QtQuick.Layouts
import Nirbija

// The master bus, pinned to the right of the mixer rather than scrolling with
// the channels: it is the one strip you always want in reach.
Rectangle {
    id: root

    property real gain: 1.0
    property real positionLeft: 0
    property real positionRight: 0
    property real holdLeft: 0
    property real holdRight: 0
    property string sink: ""

    signal outputClicked

    // Wider than a channel: this is the fader that gets the numbered scale, and
    // the numbers need the room.
    width: Skin.stripWidth + Skin.px(16)
    color: Skin.bar

    // A hairline against the mixer area, so the master reads as pinned rather
    // than as the last strip in the row.
    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: Skin.line
    }

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Skin.px(3)
        color: Skin.text
        opacity: 0.5
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Skin.gap
        anchors.topMargin: Skin.gap + Skin.px(3)
        spacing: Skin.gap

        Text {
            Layout.fillWidth: true
            Layout.preferredHeight: Skin.px(20)
            text: qsTr("MASTER")
            color: Skin.text
            font.pixelSize: Skin.font
            font.bold: true
            font.letterSpacing: 1
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        Fader {
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Skin.px(96)
            gain: root.gain
            positionLeft: root.positionLeft
            positionRight: root.positionRight
            holdLeft: root.holdLeft
            holdRight: root.holdRight
            accent: Skin.text
            showScale: true
            tip: qsTr("Master level, after every strip has been summed. Drag to move it, Shift-drag for fine, scroll for one decibel, double-click for unity.")
            onGainRequested: value => Mixer.masterGain = value
        }

        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: Mixer.gainLabel(root.gain)
            color: Skin.text
            font.pixelSize: Skin.font
            font.family: Skin.monoFamily
        }

        NodeSlot {
            Layout.fillWidth: true
            Layout.preferredHeight: Skin.slotHeight
            label: root.sink.length > 0 ? root.sink : qsTr("no output")
            tip: qsTr("Where the master goes. Click to pick a hardware output.")
            filled: root.sink.length > 0
            position: Math.max(root.positionLeft, root.positionRight)
            onClicked: root.outputClicked()
            onMenuRequested: root.outputClicked()
        }
    }
}
