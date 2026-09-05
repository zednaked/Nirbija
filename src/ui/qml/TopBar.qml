pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import Nirbija

// The top bar, left to right: transport with tempo, what the master is doing,
// the master meter in the middle, then the navigator, MIDI matrix and menu.
//
// The three master toggles used to read "D", "M" and "Ø". One-letter buttons
// are only legible to whoever wrote them, and this row is where a mix gets
// silenced by accident, so they say what they do.
Rectangle {
    id: root

    property real tempo: 120
    property bool playing: false
    property bool recording: false
    property bool metronome: false
    property string recordingLabel: ""
    property real positionLeft: 0
    property real positionRight: 0
    property real holdLeft: 0
    property real holdRight: 0
    property string status: ""
    property string masterSink: ""

    signal masterOutputClicked
    signal menuRequested(var item)
    signal navigatorClicked
    signal matrixClicked
    signal helpClicked

    implicitHeight: Skin.barHeight
    height: Skin.barHeight
    color: Skin.bar

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Skin.line
    }

    component Separator: Rectangle {
        Layout.preferredWidth: 1
        Layout.preferredHeight: Px.px(22)
        Layout.alignment: Qt.AlignVCenter
        color: Skin.border
        opacity: 0.6
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Skin.spacing
        anchors.rightMargin: Skin.spacing
        spacing: Skin.spacingS

        // --- transport --------------------------------------------------------
        StripButton {
            Layout.preferredWidth: Px.px(32)
            label: root.playing ? "■" : "▶"
            tip: qsTr("Play or stop the transport (Space). Sequencers and tempo-synced plugins follow it.")
            active: root.playing
            activeColor: Skin.meterLow
            onClicked: Mixer.togglePlay()
        }

        StripButton {
            Layout.preferredWidth: Px.px(32)
            label: "⏮"
            tip: qsTr("Rewind to the start. Plugins are told the position jumped.")
            onClicked: Mixer.rewind()
        }

        StripButton {
            Layout.preferredWidth: Px.px(32)
            label: "●"
            tip: qsTr("Record every armed channel plus the master, one file each, into a new folder.")
            active: root.recording
            activeColor: Skin.arm
            onClicked: Mixer.toggleRecord()
        }

        // Bar and beat, plus how long the take has been running. Fixed width so
        // the row does not shuffle sideways every time the beat rolls over.
        ColumnLayout {
            Layout.preferredWidth: Px.px(62)
            spacing: 0

            Text {
                Layout.fillWidth: true
                text: Mixer.positionLabel
                color: Mixer.masterClip ? Skin.arm : Skin.text
                font.pixelSize: Skin.font
                font.family: Skin.monoFamily
                font.bold: Mixer.masterClip
                horizontalAlignment: Text.AlignHCenter

                HoverHandler {
                    id: positionHover
                }

                // The colour change is the only warning that the master
                // clipped, and nothing on screen says so.
                Tip {
                    text: Mixer.masterClip
                          ? qsTr("Bar and beat. Red: the master hit full scale since the last look.")
                          : qsTr("Bar and beat of the transport.")
                    visible: positionHover.hovered
                }
            }

            Text {
                Layout.fillWidth: true
                visible: root.recording
                text: root.recordingLabel
                color: Skin.arm
                font.pixelSize: Skin.fontXS
                font.family: Skin.monoFamily
                horizontalAlignment: Text.AlignHCenter
            }
        }

        // Tempo is dragged rather than typed: it is a value you nudge while
        // listening, and a text field would take the focus off the mixer.
        Item {
            Layout.preferredWidth: Px.px(74)
            Layout.preferredHeight: Skin.buttonHeight
            activeFocusOnTab: true

            Accessible.role: Accessible.Slider
            Accessible.name: qsTr("Tempo")
            Accessible.description: root.tempo.toFixed(1) + " BPM" 

            Rectangle {
                anchors.fill: parent
                radius: Skin.radius
                color: tempoDrag.active ? Skin.slotHover
                     : tempoHover.hovered ? Skin.slot
                     : "transparent"
                border.width: parent.activeFocus ? 1 : 0
                border.color: Skin.focus

                Behavior on color {
                    ColorAnimation { duration: Skin.fast }
                }
            }

            Text {
                anchors.centerIn: parent
                text: root.tempo.toFixed(1) + " BPM"
                color: tempoDrag.active ? Skin.text : Skin.textDim
                font.pixelSize: Skin.font
                font.family: Skin.monoFamily
            }

            HoverHandler {
                id: tempoHover
                cursorShape: Qt.SizeVerCursor
            }

            Tip {
                text: qsTr("Tempo. Drag up and down or scroll to change it; Shift for tenths. Double-click rewinds.")
                visible: tempoHover.hovered
            }

            DragHandler {
                id: tempoDrag
                target: null
                xAxis.enabled: false
                dragThreshold: 0
                property real startTempo: 120
                onActiveChanged: if (active) startTempo = root.tempo
                onCentroidChanged: {
                    if (!active) return
                    // Up is faster, and a quarter BPM per pixel is fine enough
                    // to land on a number without being slow to cross the range.
                    Mixer.tempo = startTempo
                        + (centroid.pressPosition.y - centroid.position.y) * 0.25
                }
            }

            TapHandler {
                onDoubleTapped: Mixer.rewind()
            }

            WheelHandler {
                acceptedModifiers: Qt.NoModifier
                onWheel: event => Mixer.tempo = root.tempo
                                  + (event.angleDelta.y > 0 ? 1 : -1)
            }
            WheelHandler {
                acceptedModifiers: Qt.ShiftModifier
                onWheel: event => Mixer.tempo = root.tempo
                                  + (event.angleDelta.y > 0 ? 0.1 : -0.1)
            }

            Keys.onPressed: event => {
                const amount = (event.modifiers & Qt.ShiftModifier) ? 0.1 : 1
                if (event.key === Qt.Key_Up) {
                    Mixer.tempo = root.tempo + amount
                    event.accepted = true
                } else if (event.key === Qt.Key_Down) {
                    Mixer.tempo = root.tempo - amount
                    event.accepted = true
                }
            }
        }

        StripButton {
            Layout.preferredWidth: Px.px(32)
            label: "♩"
            tip: qsTr("Metronome: a click on every beat, a fifth higher on the downbeat. Independent of Play — the click runs with the session stopped, and does not start sequencers. Sits after the master fader.")
            active: root.metronome
            activeColor: Skin.solo
            onClicked: Mixer.toggleMetronome()
        }

        StripButton {
            Layout.preferredWidth: Px.px(40)
            label: qsTr("clk")
            tip: qsTr("Send MIDI clock from the clock_out port, 24 pulses per quarter note, while the transport runs.")
            active: Mixer.midiClock
            onClicked: Mixer.toggleMidiClock()
        }

        StripButton {
            Layout.preferredWidth: Px.px(40)
            label: qsTr("ext")
            tip: qsTr("Follow a MIDI clock arriving on clock_in: start, stop, position and tempo all come from it, and the transport here stops being in charge.")
            active: Mixer.followMidiClock
            activeColor: Skin.arm
            onClicked: Mixer.toggleFollowMidiClock()
        }

        Separator {}

        // --- what the master is doing -----------------------------------------
        StripButton {
            Layout.preferredWidth: Px.px(42)
            label: qsTr("DIM")
            // Was hard to tell this had done anything: with no activeColor
            // of its own it lit up the same generic blue as plain focus, a
            // 12 dB cut you can only hear on a loud source through real
            // speakers - on a quiet one, or through this box's own meters
            // sitting well under 0 dBFS already, the button was the only
            // sign it fired at all, so it needs its own unmistakable colour.
            tip: qsTr("Dim the master by 12 dB without moving the fader.")
            active: Mixer.masterDim
            activeColor: Skin.focus
            onClicked: Mixer.toggleMasterDim()
        }
        StripButton {
            Layout.preferredWidth: Px.px(48)
            label: qsTr("MUTE")
            tip: qsTr("Mute the master. Meters keep reading what would have played.")
            active: Mixer.masterMute
            activeColor: Skin.mute
            onClicked: Mixer.toggleMasterMute()
        }
        StripButton {
            Layout.preferredWidth: Px.px(48)
            label: qsTr("MONO")
            tip: qsTr("Sum the master to mono. A mix that thins out or loses a part here has phase cancellation in it. Not a polarity flip.")
            active: Mixer.masterMono
            onClicked: Mixer.toggleMasterMono()
        }
        StripButton {
            Layout.preferredWidth: Px.px(40)
            label: qsTr("LIM")
            tip: qsTr("Brickwall limiter on the master: nothing leaves above -0.3 dBFS, nothing under it is touched. Lights hot while it is holding something back. Costs 1.5 ms on the master.")
            active: Mixer.masterLimiter
            activeColor: Mixer.limiterWorking ? Skin.arm : Skin.accent
            onClicked: Mixer.toggleMasterLimiter()
        }

        Item {
            Layout.fillWidth: true
            Layout.minimumWidth: Skin.spacing
        }

        // --- master meter -----------------------------------------------------
        // Tapping it chooses where the master goes, which is the one routing
        // decision that is not per channel.
        ColumnLayout {
            Layout.preferredWidth: Px.px(190)
            Layout.alignment: Qt.AlignVCenter
            spacing: Skin.spacingXS

            Text {
                Layout.fillWidth: true
                text: Mixer.learning
                      ? qsTr("MIDI learn: move a control… (Esc cancels)")
                      : Mixer.xruns > 0
                      ? root.status + qsTr(" · %n xrun(s)", "", Mixer.xruns)
                      : root.status
                color: Mixer.learning ? Skin.solo
                     : Mixer.xruns > 0 ? Skin.arm
                     : Skin.textDim
                font.pixelSize: Skin.fontXS
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }

            Meter {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(6)
                vertical: false
                position: root.positionLeft
                hold: root.holdLeft
            }

            Meter {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(6)
                vertical: false
                position: root.positionRight
                hold: root.holdRight
            }

            Text {
                Layout.fillWidth: true
                text: root.masterSink.length > 0 ? root.masterSink
                                                 : qsTr("no output")
                color: root.masterSink.length > 0 ? Skin.textDim : Skin.disabled
                font.pixelSize: Skin.fontXS
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideMiddle
            }

            HoverHandler {
                id: meterHover
            }

            // Nothing about a pair of bars says they are also a button.
            Tip {
                text: qsTr("Master output level, left over right, with the loudest recent peak marked. Click to choose where the master goes.")
                visible: meterHover.hovered
            }

            TapHandler {
                onSingleTapped: root.masterOutputClicked()
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.minimumWidth: Skin.spacing
        }

        // A mix that clipped since the last glance, said out loud rather than
        // left to a colour change on the bar counter.
        Rectangle {
            Layout.preferredWidth: Px.px(40)
            Layout.preferredHeight: Px.px(18)
            Layout.alignment: Qt.AlignVCenter
            radius: Skin.radiusS
            visible: Mixer.masterClip
            color: Skin.mute

            Text {
                anchors.centerIn: parent
                text: qsTr("CLIP")
                color: Skin.onAccent
                font.pixelSize: Skin.fontXS
                font.bold: true
            }
        }

        // --- the rest of the session ------------------------------------------
        StripButton {
            Layout.preferredWidth: Px.px(34)
            label: "▤"
            tip: qsTr("Navigator: every strip and its chain on one line. Jump to a strip, open an editor, or close them all.")
            onClicked: root.navigatorClicked()
        }

        StripButton {
            Layout.preferredWidth: Px.px(46)
            label: qsTr("MIDI")
            tip: qsTr("MIDI matrix: sources down the side, channels across the top, a cell to connect each pair.")
            onClicked: root.matrixClicked()
        }

        StripButton {
            Layout.preferredWidth: Px.px(30)
            label: "?"
            tip: qsTr("Keyboard shortcuts (F1).")
            onClicked: root.helpClicked()
        }

        StripButton {
            id: menuButton
            Layout.preferredWidth: Px.px(32)
            label: "≡"
            tip: qsTr("Session menu: undo, plugins, recordings, save and load.")
            onClicked: root.menuRequested(menuButton)
        }
    }
}
