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
    property bool mapping: false
    property int waitingPad: -1
    property var amounts: [0, 0, 0, 0, 0, 0, 0, 0,
                           0, 0, 0, 0, 0, 0, 0, 0]
    property var mapped: [false, false, false, false, false, false, false, false,
                          false, false, false, false, false, false, false, false]
    property bool holdMapped: false
    // Set once, the first time this ever opens - after that the popup stays
    // wherever it was last dragged, the same as a real tool window would.
    property bool positioned: false

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
    // Not modal: the mixer behind it stays live, so a fader or the transport
    // is still reachable with this open - the whole point of it being a tool
    // window rather than a dialog. Dragging the empty background moves it;
    // see the DragHandler below.
    modal: false
    padding: Skin.spacingL
    closePolicy: (root.mapping || Mixer.learning)
                 ? Popup.NoAutoClose
                 : Popup.CloseOnEscape

    background: Rectangle {
        color: Skin.popup
        border.width: 1
        border.color: Skin.border
        radius: Skin.radiusL
        HoverHandler {}
        TapHandler {}
        // Empty chrome is not a handler on its own, so without this a press
        // on the padding falls through onto the strip behind - and, now
        // that the popup can sit anywhere, doubles as how it moves.
        DragHandler {
            target: null
            grabPermissions: PointerHandler.TakeOverForbidden
            onCentroidChanged: if (active) {
                const nx = root.x + centroid.position.x - centroid.pressPosition.x
                const ny = root.y + centroid.position.y - centroid.pressPosition.y
                const maxX = Overlay.overlay
                    ? Math.max(0, Overlay.overlay.width - root.width) : nx
                const maxY = Overlay.overlay
                    ? Math.max(0, Overlay.overlay.height - root.height) : ny
                root.x = Math.max(0, Math.min(nx, maxX))
                root.y = Math.max(0, Math.min(ny, maxY))
            }
        }
    }

    function openFor(row, slot) {
        root.targetRow = row
        root.targetSlot = slot
        root.refresh()
        if (!root.positioned) {
            root.x = Math.round((Overlay.overlay.width - root.width) / 2)
            root.y = Math.round((Overlay.overlay.height - root.height) / 2)
            root.positioned = true
        }
        root.open()
    }

    onOpened: poll.start()
    onClosed: {
        poll.stop()
        root.stopMapping()
    }

    function refresh() {
        root.hold = Mixer.fxPadHold(root.targetRow, root.targetSlot)
        const next = []
        const mappedNext = []
        for (let i = 0; i < 16; ++i) {
            next.push(Mixer.fxPadAmount(root.targetRow, root.targetSlot, i))
            mappedNext.push(Mixer.insertParamMapped(root.targetRow, root.targetSlot, i))
        }
        root.amounts = next
        root.mapped = mappedNext
        root.holdMapped = Mixer.insertParamMapped(root.targetRow, root.targetSlot, 16)
    }

    function armPad(index) {
        const bi = Mixer.fxPadBipolar(index)
        Mixer.learnInsertParam(root.targetRow, root.targetSlot, index,
                               bi ? -1 : 0, 1)
        root.waitingPad = index
    }

    function stopMapping() {
        Mixer.cancelLearn()
        root.mapping = false
        root.waitingPad = -1
    }

    Connections {
        target: Mixer
        function onLearnChanged() {
            if (!Mixer.learning) {
                root.waitingPad = -1
                root.refresh()
            }
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: root.mapping || Mixer.learning
        onActivated: root.stopMapping()
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
                        readonly property bool isMapped: root.mapped[pad.padIndex] === true
                        readonly property bool waiting: root.waitingPad === pad.padIndex

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: Skin.radiusL
                        color: Skin.slotEmpty
                        border.width: 2
                        border.color: pad.waiting ? Skin.solo
                                     : pad.on ? Qt.lighter(Skin.accent, 1.25)
                                     : root.mapping ? Skin.focus
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

                        Rectangle {
                            visible: pad.isMapped
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: Skin.spacingXS
                            width: Px.px(7)
                            height: Px.px(7)
                            radius: width / 2
                            color: pad.waiting ? Skin.solo : Skin.focus
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

                        TapHandler {
                            enabled: root.mapping
                            acceptedButtons: Qt.LeftButton
                            onTapped: root.armPad(pad.padIndex)
                        }

                        DragHandler {
                            id: drag
                            enabled: !root.mapping
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
            Layout.preferredHeight: Px.px(58)

            Column {
                anchors.centerIn: parent
                spacing: Skin.spacingXS

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Skin.spacingS

                    StripButton {
                        width: Px.px(88)
                        height: Px.px(36)
                        label: qsTr("MAP")
                        active: root.mapping || Mixer.learning
                        activeColor: Skin.solo
                        tip: qsTr("Bind a pad to a knob on the MIDI input of this strip. Press MAP, tap a pad, then turn the control. MAP stays on so the next pad can follow.")
                        onClicked: {
                            if (root.mapping || Mixer.learning) {
                                root.stopMapping()
                            } else {
                                root.mapping = true
                                root.waitingPad = -1
                            }
                        }
                    }

                    StripButton {
                        width: Px.px(120)
                        height: Px.px(36)
                        label: qsTr("HOLD")
                        active: root.hold
                        activeColor: Skin.focus
                        tip: root.mapping
                             ? qsTr("Tap to bind Hold to the next control.")
                             : qsTr("Latch the depths that are down so you can take your hands off. Turning Hold off drops them all. Tap a latched pad to clear it.")
                        onClicked: {
                            if (root.mapping) {
                                Mixer.learnInsertParam(root.targetRow, root.targetSlot,
                                                       16, 0, 1)
                                root.waitingPad = 16
                                return
                            }
                            root.hold = !root.hold
                            Mixer.setFxPadHold(root.targetRow, root.targetSlot, root.hold)
                            if (!root.hold) {
                                const next = []
                                for (let i = 0; i < 16; ++i) next.push(0)
                                root.amounts = next
                            }
                        }

                        Rectangle {
                            visible: root.holdMapped
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 4
                            width: Px.px(7)
                            height: Px.px(7)
                            radius: width / 2
                            color: root.waitingPad === 16 ? Skin.solo : Skin.focus
                        }
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.waitingPad >= 0
                          ? qsTr("Turn a knob on this strip's MIDI input… Esc cancels.")
                          : root.mapping
                            ? qsTr("Tap the pad you want, then turn a knob.")
                            : qsTr("Top is full. Pitch, Filter, Comb and Ring go both ways from the middle.")
                    color: root.mapping || Mixer.learning ? Skin.solo : Skin.textDim
                    font.pixelSize: Skin.fontXS
                }
            }
        }
    }
}
