import QtQuick

// The master bus, pinned to the right of the mixer rather than scrolling with
// the channels: it is the one strip you always want in reach.
Rectangle {
    id: root

    property real gain: 1.0
    property real peakLeft: 0
    property real peakRight: 0
    property string sink: ""

    signal outputClicked

    width: Skin.stripWidth
    color: Skin.bar

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 3
        color: Skin.text
        opacity: 0.5
    }

    Column {
        anchors.fill: parent
        anchors.margins: Skin.gap
        anchors.topMargin: Skin.gap + 3
        spacing: Skin.gap

        Text {
            width: parent.width
            height: 20
            text: qsTr("MASTER")
            color: Skin.text
            font.pixelSize: 11
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        Fader {
            width: parent.width - 20
            anchors.horizontalCenter: parent.horizontalCenter
            height: root.height - 150
            gain: root.gain
            peakLeft: root.peakLeft
            peakRight: root.peakRight
            accent: Skin.text
            onGainRequested: value => mixer.masterGain = value
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: mixer.gainLabel(root.gain)
            color: Skin.textDim
            font.pixelSize: 10
        }

        NodeSlot {
            width: parent.width
            label: root.sink.length > 0 ? root.sink : qsTr("no output")
            filled: root.sink.length > 0
            level: Math.max(root.peakLeft, root.peakRight)
            onClicked: root.outputClicked()
            onMenuRequested: root.outputClicked()
        }
    }
}
