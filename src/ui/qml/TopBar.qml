import QtQuick

// AUM's top bar, left to right: transport with tempo, the master meter in the
// middle, then the navigator, MIDI matrix and menu buttons on the right.
Rectangle {
    id: root

    property real tempo: 120
    property bool playing: false
    property bool recording: false
    property bool metronome: false
    property string recordingLabel: ""
    property real peakLeft: 0
    property real peakRight: 0
    property string status: ""
    property string masterSink: ""

    signal masterOutputClicked
    signal menuRequested(var item)
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
            label: root.playing ? "■" : "▶"
            active: root.playing
            activeColor: Skin.meterLow
            onClicked: mixer.togglePlay()
        }

        StripButton {
            width: 30
            height: 28
            label: "⏮"
            onClicked: mixer.rewind()
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: mixer.positionLabel
            color: mixer.masterClip ? Skin.arm : Skin.textDim
            font.pixelSize: 12
            font.bold: mixer.masterClip
        }

        StripButton {
            width: 30
            height: 28
            label: "●"
            active: root.recording
            activeColor: Skin.arm
            onClicked: mixer.toggleRecord()
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.recording
            text: root.recordingLabel
            color: Skin.arm
            font.pixelSize: 11
        }

        StripButton {
            width: 30
            height: 28
            label: "♩"
            active: root.metronome
            activeColor: Skin.solo
            onClicked: mixer.toggleMetronome()
        }

        StripButton {
            width: 30
            height: 28
            label: "clk"
            active: mixer.midiClock
            onClicked: mixer.toggleMidiClock()
        }

        StripButton {
            width: 28
            height: 28
            label: "D"
            active: mixer.masterDim
            onClicked: mixer.toggleMasterDim()
        }
        StripButton {
            width: 28
            height: 28
            label: "M"
            active: mixer.masterMute
            activeColor: Skin.mute
            onClicked: mixer.toggleMasterMute()
        }
        StripButton {
            width: 28
            height: 28
            label: "Ø"
            active: mixer.masterMono
            onClicked: mixer.toggleMasterMono()
        }

        // Tempo is dragged rather than typed: it is a value you nudge while
        // listening, and a text field would take the focus off the mixer.
        Text {
            id: tempoLabel
            anchors.verticalCenter: parent.verticalCenter
            text: root.tempo.toFixed(1) + " BPM"
            color: tempoDrag.drag.active ? Skin.text : Skin.textDim
            font.pixelSize: 12

            MouseArea {
                id: tempoDrag
                anchors.fill: parent
                anchors.margins: -6
                property real startY: 0
                property real startTempo: 120

                drag.target: null
                onPressed: mouse => {
                    startY = mouse.y
                    startTempo = root.tempo
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    // Up is faster, and a quarter BPM per pixel is fine enough
                    // to land on a number without being slow to cross the range.
                    mixer.tempo = startTempo + (startY - mouse.y) * 0.25
                }
                onDoubleClicked: mixer.rewind()
            }
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
        text: mixer.learning ? qsTr("MIDI learn: move a control… (Esc cancels)") : root.status
        color: mixer.learning ? Skin.solo : Skin.textDim
        font.pixelSize: 9

        MouseArea {
            anchors.fill: parent
            enabled: mixer.learning
            onClicked: mixer.cancelLearn()
        }
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
            id: menuButton
            width: 30
            height: 28
            label: "≡"
            onClicked: root.menuRequested(menuButton)
        }
    }
}
