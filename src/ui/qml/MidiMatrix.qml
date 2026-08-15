import QtQuick
import QtQuick.Controls.Basic

// The MIDI matrix: sources down the side, channels across the top, a cell per
// crossing. Any source can feed any number of channels and the other way
// round, which the per-strip slot cannot say.
Popup {
    id: root

    property var sources: []

    width: Math.min(720, 200 + mixer.rowCount() * 44)
    height: Math.min(480, 90 + sources.length * 34)
    modal: true
    padding: 10

    background: Rectangle {
        color: Skin.strip
        border.width: 1
        border.color: Skin.line
        radius: Skin.radius
    }

    // Fetched on open: the port list is whatever the server offers right now.
    onAboutToShow: sources = mixer.sources(true)

    Column {
        anchors.fill: parent
        spacing: 6

        Text {
            text: qsTr("MIDI matrix")
            color: Skin.text
            font.pixelSize: 13
            font.bold: true
        }

        // Column headers: one per channel, buses excluded since they take no
        // MIDI.
        Row {
            spacing: 4

            Item {
                width: 180
                height: 30
            }

            Repeater {
                model: mixer

                Item {
                    visible: !model.isBus
                    width: visible ? 40 : 0
                    height: 30

                    Text {
                        anchors.centerIn: parent
                        width: 38
                        text: model.name
                        color: Skin.textDim
                        font.pixelSize: 9
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                }
            }
        }

        ListView {
            id: sourceList
            width: parent.width
            height: parent.height - 70
            clip: true
            spacing: 4
            model: root.sources
            boundsBehavior: Flickable.StopAtBounds

            delegate: Row {
                spacing: 4

                readonly property string sourcePort: modelData

                Text {
                    width: 180
                    height: 30
                    text: mixer.shortPortName(sourcePort)
                    color: Skin.text
                    font.pixelSize: 11
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideMiddle
                }

                Repeater {
                    model: mixer

                    Rectangle {
                        visible: !model.isBus
                        width: visible ? 40 : 0
                        height: 30
                        radius: Skin.radius
                        color: linked ? Skin.accent : Skin.slotEmpty
                        border.width: 1
                        border.color: Skin.line

                        // Re-read when any routing changes, so two views of the
                        // same link cannot disagree.
                        property bool linked: false
                        function refresh() { linked = mixer.midiLinked(index, sourcePort) }
                        Component.onCompleted: refresh()

                        Connections {
                            target: mixer
                            function onRoutingChanged() { refresh() }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: mixer.setMidiLink(index, sourcePort, !parent.linked)
                        }
                    }
                }
            }
        }
    }
}
