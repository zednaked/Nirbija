pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// The MIDI matrix: sources down the side, channels across the top, a cell per
// crossing. Any source can feed any number of channels and the other way
// round, which the per-strip slot cannot say.
Popup {
    id: root

    property var sources: []

    width: Math.min(Skin.px(760), Skin.px(220) + Mixer.rowCount() * Skin.px(46))
    height: Math.min(Skin.px(500), Skin.px(110) + sources.length * Skin.px(36))
    modal: true
    padding: Skin.spacingL
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

    // Fetched on open: the port list is whatever the server offers right now.
    onAboutToShow: root.sources = Mixer.sources(true)

    readonly property int columnWidth: Skin.px(42)
    readonly property int labelWidth: Skin.px(180)

    contentItem: ColumnLayout {
        spacing: Skin.spacingS

        Text {
            Layout.fillWidth: true
            text: qsTr("MIDI matrix")
            color: Skin.text
            font.pixelSize: Skin.fontL
            font.bold: true
        }

        Text {
            Layout.fillWidth: true
            visible: root.sources.length === 0
            text: qsTr("No MIDI source is offering ports right now.")
            color: Skin.textDim
            font.pixelSize: Skin.font
            wrapMode: Text.WordWrap
        }

        // Column headers: one per channel, buses excluded since they take no
        // MIDI.
        Row {
            Layout.fillWidth: true
            spacing: Skin.spacingS

            Item {
                width: root.labelWidth
                height: Skin.px(28)
            }

            Repeater {
                model: Mixer

                Item {
                    id: header
                    required property string name
                    required property bool isBus

                    visible: !isBus
                    width: visible ? root.columnWidth : 0
                    height: Skin.px(28)

                    Text {
                        anchors.centerIn: parent
                        width: parent.width - Skin.spacingXS
                        text: header.name
                        color: Skin.textDim
                        font.pixelSize: Skin.fontXS
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                }
            }
        }

        ListView {
            id: sourceList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Skin.spacingS
            model: root.sources
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar {
                policy: sourceList.contentHeight > sourceList.height
                        ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
            }

            delegate: Row {
                id: sourceRow
                required property string modelData
                readonly property string sourcePort: modelData

                spacing: Skin.spacingS

                Text {
                    width: root.labelWidth
                    height: Skin.rowHeight
                    text: Mixer.shortPortName(sourceRow.sourcePort)
                    color: Skin.text
                    font.pixelSize: Skin.font
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideMiddle
                }

                Repeater {
                    model: Mixer

                    Rectangle {
                        id: cell
                        required property int index
                        required property bool isBus

                        visible: !isBus
                        width: visible ? root.columnWidth : 0
                        height: Skin.rowHeight
                        radius: Skin.radius
                        color: linked ? Skin.accent
                             : cellHover.hovered ? Skin.slotHover
                             : Skin.slotEmpty
                        border.width: 1
                        border.color: linked ? Qt.lighter(Skin.accent, 1.2)
                                             : Skin.border

                        Behavior on color {
                            ColorAnimation { duration: Skin.fast }
                        }

                        // Re-read when any routing changes, so two views of the
                        // same link cannot disagree.
                        property bool linked: false
                        function refresh() {
                            linked = Mixer.midiLinked(cell.index, sourceRow.sourcePort)
                        }
                        Component.onCompleted: refresh()

                        Connections {
                            target: Mixer
                            function onRoutingChanged() { cell.refresh() }
                        }

                        HoverHandler {
                            id: cellHover
                        }

                        TapHandler {
                            onSingleTapped: Mixer.setMidiLink(cell.index,
                                                              sourceRow.sourcePort,
                                                              !cell.linked)
                        }
                    }
                }
            }
        }
    }
}
