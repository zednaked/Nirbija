import QtQuick

// AUM's top bar, left to right: transport with tempo, the master meter in the
// middle, then the navigator, MIDI matrix and menu buttons on the right.
Rectangle {
    id: root

    property real tempo: 120
    property real peakLeft: 0
    property real peakRight: 0
    property string status: ""
    property string masterSink: ""

    signal masterOutputClicked
    signal navigatorClicked
    signal matrixClicked
    signal menuClicked

    height: Skin.barHeight
    color: Skin.bar

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Skin.line
    }

    Row {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 8
        spacing: 6

        StripButton {
            width: 30
            height: 28
            label: "▶"
            activeColor: Skin.meterLow
        }

        StripButton {
            width: 30
            height: 28
            label: "●"
            activeColor: Skin.arm
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.tempo.toFixed(0) + " BPM"
            color: Skin.textDim
            font.pixelSize: 12
        }
    }

    // --- master meter -------------------------------------------------------
    // Tapping it chooses where the master goes, which is the one routing
    // decision that is not per channel.
    MouseArea {
        anchors.fill: masterMeter
        onClicked: root.masterOutputClicked()
    }

    Column {
        id: masterMeter
        anchors.centerIn: parent
        spacing: 3

        Repeater {
            model: [root.peakLeft, root.peakRight]

            Rectangle {
                width: 168
                height: 6
                radius: 1
                color: Skin.line

                Rectangle {
                    width: parent.width * mixer.gainToFader(modelData)
                    height: parent.height
                    radius: 1
                    color: Skin.meterColor(mixer.gainToFader(modelData))

                    Behavior on width {
                        NumberAnimation {
                            duration: 60
                        }
                    }
                }
            }
        }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 2
        text: root.status
        color: Skin.textDim
        font.pixelSize: 9
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 3
        text: root.masterSink.length > 0 ? root.masterSink : qsTr("no output")
        color: Skin.textDim
        font.pixelSize: 9
    }

    Row {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.rightMargin: 8
        spacing: 6

        StripButton {
            width: 30
            height: 28
            label: "▤"
            onClicked: root.navigatorClicked()
        }

        StripButton {
            width: 30
            height: 28
            label: "Z"
            onClicked: root.matrixClicked()
        }

        StripButton {
            width: 30
            height: 28
            label: "≡"
            onClicked: root.menuClicked()
        }
    }
}
