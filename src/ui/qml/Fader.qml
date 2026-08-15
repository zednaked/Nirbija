import QtQuick

// A vertical fader with the level meter running alongside it, the way a mixer
// strip reads: the meter answers the fader right next to it.
Item {
    id: root

    property real gain: 1.0
    property real peakLeft: 0
    property real peakRight: 0
    property color accent: Skin.accent

    signal gainRequested(real gain)

    // Fader travel is in decibels, so the handle position is not the gain.
    readonly property real position: mixer.gainToFader(gain)

    Row {
        anchors.fill: parent
        spacing: 6

        // --- fader track -----------------------------------------------------
        Item {
            id: track
            width: 26
            height: parent.height

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 4
                height: parent.height
                radius: 2
                color: Skin.line
            }

            // Unity is where the ear expects the fader to rest, so it gets a
            // tick rather than being just another point on the travel.
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 14
                height: 1
                color: Skin.textDim
                opacity: 0.5
                y: track.height - mixer.gainToFader(1.0) * track.height
            }

            Rectangle {
                id: handle
                width: 22
                height: 14
                radius: 2
                color: Skin.slot
                border.width: 1
                border.color: root.accent
                anchors.horizontalCenter: parent.horizontalCenter
                y: Math.max(0, Math.min(track.height - height,
                                        track.height - root.position * track.height - height / 2))

                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width - 8
                    height: 2
                    color: root.accent
                }
            }

            MouseArea {
                anchors.fill: parent
                preventStealing: true
                onPressed: mouse => setFromY(mouse.y)
                onPositionChanged: mouse => {
                    if (pressed)
                        setFromY(mouse.y)
                }
                onDoubleClicked: root.gainRequested(1.0)

                function setFromY(y) {
                    const clamped = Math.max(0, Math.min(track.height, y))
                    root.gainRequested(mixer.faderToGain(1 - clamped / track.height))
                }
            }
        }

        // --- meter -----------------------------------------------------------
        Row {
            spacing: 2
            height: parent.height

            Repeater {
                model: [root.peakLeft, root.peakRight]

                Rectangle {
                    width: 6
                    height: track.height
                    color: Skin.line
                    radius: 1

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        // Meters read in decibels too, or everything below a
                        // quarter of full scale collapses into the floor.
                        height: parent.height * mixer.gainToFader(modelData)
                        color: Skin.meterColor(mixer.gainToFader(modelData))
                        radius: 1

                        Behavior on height {
                            NumberAnimation {
                                duration: 60
                            }
                        }
                    }
                }
            }
        }
    }
}
