pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import Nirbija

// Picks a JACK port for a node slot: what feeds a channel, or where the master
// goes. The list is whatever the server is offering right now, so it is fetched
// on open rather than held.
Popup {
    id: root

    // "audio", "midi", "sink" or "channelSink" — what is being chosen.
    property string kind: "audio"
    property int targetRow: -1
    property var ports: []

    signal picked(string port)

    width: Skin.px(470)
    height: Skin.px(440)
    modal: true
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Skin.popup
        border.width: 1
        border.color: Skin.border
        radius: Skin.radiusL
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Skin.fast }
    }

    readonly property string heading: root.kind === "midi" ? qsTr("MIDI source")
        : root.kind === "sink" ? qsTr("Master output")
        : root.kind === "channelSink" ? qsTr("Direct output")
                                      : qsTr("Audio input")

    // Anchored to the slot that asked for it rather than centred on the window:
    // plugin editors are separate top-level windows and sit over the middle of
    // the screen, where a centred picker would open underneath them.
    function openFor(kind, row, item) {
        root.kind = kind
        root.targetRow = row
        root.ports = (kind === "sink" || kind === "channelSink")
                     ? Mixer.sinks() : Mixer.sources(kind === "midi")
        search.text = ""

        if (item !== undefined && item !== null) {
            const point = item.mapToItem(Overlay.overlay, 0, item.height)
            root.x = Math.max(4, Math.min(point.x, Overlay.overlay.width - root.width - 4))
            root.y = Math.max(4, Math.min(point.y, Overlay.overlay.height - root.height - 4))
        } else {
            root.x = (Overlay.overlay.width - root.width) / 2
            root.y = (Overlay.overlay.height - root.height) / 2
        }
        root.open()
        search.forceActiveFocus()
    }

    // A machine running a DAW and a browser offers dozens of ports; typing two
    // letters beats reading the list.
    readonly property var visiblePorts: {
        if (search.text.length === 0) return root.ports
        const needle = search.text.toLowerCase()
        return root.ports.filter(port => port.toLowerCase().includes(needle))
    }

    Column {
        anchors.fill: parent
        anchors.margins: Skin.spacing
        spacing: Skin.spacingS

        Text {
            width: parent.width
            text: root.heading
            color: Skin.text
            font.pixelSize: Skin.fontL
            font.bold: true
        }

        TextField {
            id: search
            width: parent.width
            placeholderText: qsTr("Filter %1 ports").arg(root.ports.length)
            color: Skin.text
            placeholderTextColor: Skin.disabled
            font.pixelSize: Skin.font
            selectByMouse: true

            background: Rectangle {
                color: Skin.slotEmpty
                radius: Skin.radius
                border.width: 1
                border.color: search.activeFocus ? Skin.focus : Skin.border
            }

            Keys.onDownPressed: list.forceActiveFocus()
        }

        ListView {
            id: list
            width: parent.width
            height: parent.height - search.height - Skin.px(24) - 2 * Skin.spacingS
            clip: true
            model: root.visiblePorts
            currentIndex: -1
            keyNavigationEnabled: true
            reuseItems: true
            boundsBehavior: Flickable.StopAtBounds

            Keys.onReturnPressed: {
                if (currentIndex >= 0) {
                    root.picked(root.visiblePorts[currentIndex])
                    root.close()
                }
            }

            ScrollBar.vertical: ScrollBar {
                policy: list.contentHeight > list.height ? ScrollBar.AsNeeded
                                                         : ScrollBar.AlwaysOff
            }

            // Disconnecting has to be reachable too, so it heads the list.
            header: Rectangle {
                width: list.width
                height: Skin.rowHeight
                radius: Skin.radius
                color: disconnectHover.hovered ? Skin.slot : "transparent"

                HoverHandler {
                    id: disconnectHover
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: Skin.spacing
                    text: qsTr("— disconnect —")
                    color: Skin.textDim
                    font.pixelSize: Skin.font
                }

                TapHandler {
                    onSingleTapped: {
                        root.picked("")
                        root.close()
                    }
                }
            }

            delegate: Rectangle {
                id: entry
                required property int index
                required property string modelData

                width: list.width
                height: Skin.rowHeight
                radius: Skin.radius
                color: list.currentIndex === entry.index ? Skin.slot
                     : hover.hovered ? Skin.stripAlt
                     : "transparent"

                // A monitor is the echo of what is being played to a device,
                // not an input; it shares the device's name, so it is tagged or
                // a guitar ends up connected to silence.
                readonly property bool isMonitor: entry.modelData.indexOf(":monitor_") >= 0

                HoverHandler {
                    id: hover
                    onHoveredChanged: if (hovered) list.currentIndex = entry.index
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: monitorTag.left
                    anchors.leftMargin: Skin.spacing
                    anchors.rightMargin: Skin.spacing
                    text: entry.modelData
                    color: entry.isMonitor ? Skin.textDim : Skin.text
                    font.pixelSize: Skin.font
                    elide: Text.ElideMiddle
                }

                Text {
                    id: monitorTag
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: Skin.spacing
                    visible: entry.isMonitor
                    width: visible ? implicitWidth : 0
                    text: qsTr("monitor")
                    color: Skin.disabled
                    font.pixelSize: Skin.fontXS
                }

                TapHandler {
                    onSingleTapped: {
                        root.picked(entry.modelData)
                        root.close()
                    }
                }
            }
        }

        Text {
            width: parent.width
            height: Skin.px(24)
            visible: root.visiblePorts.length === 0
            text: root.ports.length === 0
                  ? qsTr("The audio server is offering nothing of this kind.")
                  : qsTr("No port matches “%1”.").arg(search.text)
            color: Skin.textDim
            font.pixelSize: Skin.font
            wrapMode: Text.WordWrap
        }
    }
}
