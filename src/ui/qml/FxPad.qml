pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// Sixteen performance pads. Each one is an amount, not a switch: where the
// finger is on the pad is how much of the effect you get. Pitch, Filter, Comb
// and Ring are bipolar — centre is off, up and down go opposite ways — so they
// cannot be a latch that only knows on. HOLD still latches the depth so both
// hands can leave the glass.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property bool hold: false
    property var amounts: [0, 0, 0, 0, 0, 0, 0, 0,
                           0, 0, 0, 0, 0, 0, 0, 0]

    readonly property var names: [
        "CRUSH", "PITCH", "COMB", "RING",
        "REVERB", "STUTTER", "GATE", "FILTER",
        "CUTTER", "REVERSE", "DUB", "TEMPO DELAY",
        "TALKBOX", "VIBROFLANGE", "DIRTY", "COMPRESSOR"
    ]

    readonly property var tips: [
        qsTr("Bit depth and sample hold. The top is crushed, the bottom is off."),
        qsTr("Pitch. Up raises, down drops, the middle is off."),
        qsTr("Feedback comb. Up is short and metallic, down is long and hollow."),
        qsTr("Ring modulator. Up is a high growl, down is a low one."),
        qsTr("Room to hall. Higher is wetter and longer."),
        qsTr("Loop a captured slice. Higher cuts it shorter."),
        qsTr("Tremolo gate. Higher is faster and narrower."),
        qsTr("Filter. Down closes a lowpass, up opens a highpass."),
        qsTr("On-off chop. Higher is more often, and less stays open."),
        qsTr("Last second backwards. Higher is more reverse."),
        qsTr("Long dark delay. Higher is more feedback."),
        qsTr("A clean eighth. Higher is more repeats."),
        qsTr("Swept vowel. Higher travels further."),
        qsTr("Short modulated delay. Higher is deeper and faster."),
        qsTr("Saturation. Higher is more drive."),
        qsTr("Flattens peaks. Higher is a lower threshold.")
    ]

    width: Px.px(760)
    height: Px.px(440)
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
            next.push(Mixer.fxPadAmount(root.targetRow, root.targetSlot, i))
        root.amounts = next
    }

    function applyAmount(index, value) {
        Mixer.setFxPadAmount(root.targetRow, root.targetSlot, index, value)
        const next = root.amounts.slice()
        next[index] = value
        root.amounts = next
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
                        readonly property real amount: {
                            const v = root.amounts[pad.padIndex]
                            return typeof v === "number" ? v : 0
                        }
                        readonly property bool bipolar: Mixer.fxPadBipolar(pad.padIndex)
                        readonly property bool on: Math.abs(pad.amount) > 0.02

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: Skin.radiusL
                        color: Skin.slotEmpty
                        border.width: 2
                        border.color: pad.on ? Qt.lighter(Skin.accent, 1.25)
                                             : pad.activeFocus ? Skin.focus
                                             : Skin.border
                        clip: true

                        // Where the finger is on the pad is the amount. Relative
                        // drag, the way a fader works, would make a slap in the
                        // middle land wherever the pad happened to be left —
                        // useless on a performance grid. Jumping is the point.
                        function amountAt(y) {
                            const t = 1 - Math.max(0, Math.min(1, y / Math.max(1, pad.height)))
                            if (pad.bipolar) return t * 2 - 1
                            return t
                        }

                        function setAmount(value) {
                            let v = value
                            if (pad.bipolar) v = Math.max(-1, Math.min(1, v))
                            else v = Math.max(0, Math.min(1, v))
                            root.applyAmount(pad.padIndex, v)
                        }

                        function nudge(delta) {
                            pad.setAmount(pad.amount + delta)
                        }

                        // Unipolar fills from the bottom; bipolar fills from
                        // the middle so up and down can be read at a glance.
                        Rectangle {
                            visible: !pad.bipolar
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: Math.max(0, Math.min(1, pad.amount)) * parent.height
                            color: Skin.accent
                            opacity: 0.82
                        }

                        Rectangle {
                            visible: pad.bipolar
                            anchors.left: parent.left
                            anchors.right: parent.right
                            y: pad.amount >= 0
                               ? parent.height / 2 - Math.abs(pad.amount) * parent.height / 2
                               : parent.height / 2
                            height: Math.abs(pad.amount) * parent.height / 2
                            color: Skin.accent
                            opacity: 0.82
                        }

                        Rectangle {
                            visible: pad.bipolar
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            height: 1
                            color: Skin.border
                        }

                        Text {
                            anchors.centerIn: parent
                            rotation: -90
                            text: root.names[pad.padIndex]
                            color: pad.on ? Skin.onAccent : Skin.text
                            font.pixelSize: Skin.fontS
                            font.bold: true
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: Skin.spacingXS
                            visible: pad.on
                            text: pad.bipolar
                                  ? (pad.amount > 0 ? "+" : "") + Math.round(pad.amount * 100)
                                  : Math.round(pad.amount * 100)
                            color: pad.on ? Skin.onAccent : Skin.textDim
                            font.pixelSize: Skin.fontXS
                            font.family: Skin.monoFamily
                        }

                        HoverHandler {
                            id: padHover
                            cursorShape: Qt.SizeVerCursor
                        }

                        Tip {
                            text: root.tips[pad.padIndex]
                            visible: padHover.hovered
                        }

                        activeFocusOnTab: true
                        Accessible.role: Accessible.Slider
                        Accessible.name: root.names[pad.padIndex]
                        Accessible.description: root.tips[pad.padIndex]

                        DragHandler {
                            id: drag
                            target: null
                            dragThreshold: 0
                            grabPermissions: PointerHandler.CanTakeOverFromAnything
                                             | PointerHandler.ApprovesTakeOverByNothing
                            property real pressY: 0
                            property real pressAmount: 0
                            property bool armedToggle: false

                            onActiveChanged: {
                                if (active) {
                                    pad.forceActiveFocus(Qt.MouseFocusReason)
                                    pressY = centroid.position.y
                                    pressAmount = pad.amount
                                    // A tap on a latched pad clears it. A drag
                                    // from the same press is a new amount —
                                    // otherwise Hold would trap you at whatever
                                    // depth you first landed on.
                                    armedToggle = root.hold && Math.abs(pressAmount) > 0.02
                                    if (!armedToggle)
                                        pad.setAmount(pad.amountAt(centroid.position.y))
                                } else if (armedToggle &&
                                           Math.abs(centroid.position.y - pressY) < Px.px(8)) {
                                    pad.setAmount(0)
                                } else if (!root.hold) {
                                    pad.setAmount(0)
                                }
                            }
                            onCentroidChanged: {
                                if (!active) return
                                if (armedToggle &&
                                        Math.abs(centroid.position.y - pressY) >= Px.px(8))
                                    armedToggle = false
                                if (!armedToggle)
                                    pad.setAmount(pad.amountAt(centroid.position.y))
                            }
                        }

                        WheelHandler {
                            acceptedModifiers: Qt.NoModifier
                            onWheel: event => pad.nudge(event.angleDelta.y > 0 ? 0.05 : -0.05)
                        }
                        WheelHandler {
                            acceptedModifiers: Qt.ShiftModifier
                            onWheel: event => pad.nudge(event.angleDelta.y > 0 ? 0.01 : -0.01)
                        }

                        Keys.onPressed: event => {
                            const step = (event.modifiers & Qt.ShiftModifier) ? 0.01 : 0.05
                            switch (event.key) {
                            case Qt.Key_Up:
                                pad.nudge(step); event.accepted = true; break
                            case Qt.Key_Down:
                                pad.nudge(-step); event.accepted = true; break
                            case Qt.Key_Home:
                                pad.setAmount(pad.bipolar ? 0 : 1)
                                event.accepted = true
                                break
                            case Qt.Key_End:
                                pad.setAmount(0); event.accepted = true; break
                            }
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Px.px(52)

            Column {
                anchors.centerIn: parent
                spacing: Skin.spacingXS

                StripButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Px.px(120)
                    height: Px.px(36)
                    label: qsTr("HOLD")
                    active: root.hold
                    activeColor: Skin.focus
                    tip: qsTr("Latch the depths that are down so you can take your hands off. Turning Hold off drops them all. Tap a latched pad to clear it.")
                    onClicked: {
                        root.hold = !root.hold
                        Mixer.setFxPadHold(root.targetRow, root.targetSlot, root.hold)
                        if (!root.hold) {
                            const next = []
                            for (let i = 0; i < 16; ++i) next.push(0)
                            root.amounts = next
                        }
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Top is full. Pitch, Filter, Comb and Ring go both ways from the middle.")
                    color: Skin.textDim
                    font.pixelSize: Skin.fontXS
                }
            }
        }
    }
}
