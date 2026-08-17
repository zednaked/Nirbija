pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// Sixteen performance pads, held like keys. HOLD latches whatever is down.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property bool hold: false
    property var pads: [false, false, false, false, false, false, false, false,
                        false, false, false, false, false, false, false, false]

    readonly property var names: [
        "CRUSH", "PITCH", "COMB", "RING",
        "REVERB", "STUTTER", "GATE", "FILTER",
        "CUTTER", "REVERSE", "DUB", "TEMPO DELAY",
        "TALKBOX", "VIBROFLANGE", "DIRTY", "COMPRESSOR"
    ]

    width: Px.px(720)
    height: Px.px(340)
    modal: true
    anchors.centerIn: Overlay.overlay
    padding: Skin.spacingL
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    Overlay.modal: Rectangle {
        color: Qt.rgba(0, 0, 0, 0.45)
        HoverHandler {}
        TapHandler {
            onTapped: {
                if (root.closePolicy & Popup.CloseOnPressOutside)
                    root.close()
            }
        }
        DragHandler {
            target: null
            grabPermissions: PointerHandler.TakeOverForbidden
        }
    }

    background: Rectangle {
        color: Skin.popup
        border.width: 1
        border.color: Skin.border
        radius: Skin.radiusL
        HoverHandler {}
        TapHandler {}
    }

    function openFor(row, slot) {
        root.targetRow = row
        root.targetSlot = slot
        root.refresh()
        root.open()
    }

    onOpened: poll.start()
    onClosed: poll.stop()

    function refresh() {
        root.hold = Mixer.fxPadHold(root.targetRow, root.targetSlot)
        const next = []
        for (let i = 0; i < 16; ++i)
            next.push(Mixer.fxPadOn(root.targetRow, root.targetSlot, i))
        root.pads = next
    }

    Timer {
        id: poll
        interval: 66
        repeat: true
        onTriggered: root.refresh()
    }

    contentItem: ColumnLayout {
        spacing: Skin.spacing

        Repeater {
            model: 2

            RowLayout {
                id: padRow
                required property int index
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Skin.spacingS

                Repeater {
                    model: 8

                    Rectangle {
                        id: pad
                        required property int index
                        readonly property int padIndex: padRow.index * 8 + index
                        readonly property bool on: root.pads[pad.padIndex] === true

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: Skin.radiusL
                        color: pad.on ? Skin.accent : Skin.slotEmpty
                        border.width: 2
                        border.color: pad.on ? Qt.lighter(Skin.accent, 1.25)
                                             : Skin.border

                        Text {
                            anchors.centerIn: parent
                            rotation: -90
                            text: root.names[pad.padIndex]
                            color: pad.on ? Skin.onAccent : Skin.text
                            font.pixelSize: Skin.fontS
                            font.bold: true
                        }

                        DragHandler {
                            target: null
                            dragThreshold: 0
                            grabPermissions: PointerHandler.CanTakeOverFromAnything
                                             | PointerHandler.ApprovesTakeOverByNothing
                            onActiveChanged: {
                                if (active) {
                                    if (root.hold && pad.on) {
                                        Mixer.setFxPad(root.targetRow, root.targetSlot,
                                                       pad.padIndex, false)
                                        const next = root.pads.slice()
                                        next[pad.padIndex] = false
                                        root.pads = next
                                    } else {
                                        Mixer.setFxPad(root.targetRow, root.targetSlot,
                                                       pad.padIndex, true)
                                        const next = root.pads.slice()
                                        next[pad.padIndex] = true
                                        root.pads = next
                                    }
                                } else if (!root.hold) {
                                    Mixer.setFxPad(root.targetRow, root.targetSlot,
                                                   pad.padIndex, false)
                                    const next = root.pads.slice()
                                    next[pad.padIndex] = false
                                    root.pads = next
                                }
                            }
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Px.px(44)

            StripButton {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Px.px(120)
                height: Px.px(36)
                label: qsTr("HOLD")
                active: root.hold
                activeColor: Skin.focus
                tip: qsTr("Latch the pads that are down so you can take your hands off. Turning Hold off drops them all.")
                onClicked: {
                    root.hold = !root.hold
                    Mixer.setFxPadHold(root.targetRow, root.targetSlot, root.hold)
                    if (!root.hold) {
                        const next = []
                        for (let i = 0; i < 16; ++i) next.push(false)
                        root.pads = next
                    }
                }
            }
        }
    }
}
