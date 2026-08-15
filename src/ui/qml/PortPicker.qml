import QtQuick
import QtQuick.Controls.Basic

// Picks a JACK port for a node slot: what feeds a channel, or where the master
// goes. The list is whatever the server is offering right now, so it is fetched
// on open rather than held.
Popup {
    id: root

    // "audio", "midi" or "sink" — what kind of port is being chosen.
    property string kind: "audio"
    property int targetRow: -1
    property var ports: []

    signal picked(string port)

    width: 460
    height: 420
    modal: true
    padding: 0

    background: Rectangle {
        color: Skin.strip
        border.width: 1
        border.color: Skin.line
        radius: Skin.radius
    }

    // Anchored to the slot that asked for it rather than centred on the window:
    // plugin editors are separate top-level windows and sit over the middle of
    // the screen, where a centred picker would open underneath them.
    function openFor(kind, row, item) {
        root.kind = kind
        root.targetRow = row
        root.ports = kind === "sink" ? mixer.sinks() : mixer.sources(kind === "midi")

        if (item !== undefined && item !== null) {
            const point = item.mapToItem(Overlay.overlay, 0, item.height)
            root.x = Math.max(4, Math.min(point.x, Overlay.overlay.width - root.width - 4))
            root.y = Math.max(4, Math.min(point.y, Overlay.overlay.height - root.height - 4))
        } else {
            root.x = (Overlay.overlay.width - root.width) / 2
            root.y = (Overlay.overlay.height - root.height) / 2
        }
        root.open()
    }

    Column {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        Text {
            text: root.kind === "midi" ? qsTr("MIDI source")
                                       : root.kind === "sink" ? qsTr("Master output")
                                                              : qsTr("Audio input")
            color: Skin.text
            font.pixelSize: 13
        }

        ListView {
            id: list
            width: parent.width
            height: parent.height - 36
            clip: true
            model: root.ports

            // Disconnecting has to be reachable too, so it heads the list.
            header: Rectangle {
                width: list.width
                height: 34
                color: disconnectHover.hovered ? Skin.slot : "transparent"

                HoverHandler {
                    id: disconnectHover
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    text: qsTr("— disconnect —")
                    color: Skin.textDim
                    font.pixelSize: 12
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        root.picked("")
                        root.close()
                    }
                }
            }

            delegate: Rectangle {
                width: list.width
                height: 34
                color: hover.hovered ? Skin.slot : "transparent"

                HoverHandler {
                    id: hover
                }

                // A monitor is the echo of what is being played to a device,
                // not an input; it shares the device's name, so it is tagged or
                // a guitar ends up connected to silence.
                readonly property bool isMonitor: modelData.indexOf(":monitor_") >= 0

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: monitorTag.left
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    text: modelData
                    color: isMonitor ? Skin.textDim : Skin.text
                    font.pixelSize: 12
                    elide: Text.ElideMiddle
                }

                Text {
                    id: monitorTag
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: 8
                    visible: isMonitor
                    width: visible ? implicitWidth : 0
                    text: qsTr("monitor")
                    color: Skin.textDim
                    font.pixelSize: 10
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        root.picked(modelData)
                        root.close()
                    }
                }
            }
        }
    }
}
