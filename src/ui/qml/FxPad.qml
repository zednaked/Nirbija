pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// Sixteen performance pads, one per effect - same contract as before: each
// pad is an amount, not a switch, where the finger lands is how much of the
// effect you get. Pitch, Filter, Comb and Ring are bipolar - centre is off,
// up and down go opposite ways - so they cannot be a latch that only knows
// on. HOLD still latches the depth so both hands can leave the glass.
//
// Laid out 4x4 rather than two rows of eight, and every pad carries its own
// hue around a colour wheel instead of one accent shared by all sixteen: a
// row of tall, same-coloured bars is the first thing every pad sampler
// reaches for, and it is what made this one look like a copy of the others.
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

    // One hue per pad, evenly spaced round the wheel. The header stripe and
    // every pad's fill and glow all come from this one function, so the
    // sixteen effects read as sixteen distinct instruments rather than
    // sixteen copies of the same blue.
    function padHue(index) { return Qt.hsva(index / 16, 0.58, 0.92, 1.0) }

    width: Px.px(620)
    height: Px.px(640)
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
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.lighter(Skin.popup, 1.08) }
            GradientStop { position: 1.0; color: Skin.popup }
        }
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

        // --- header --------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingS

            Text {
                text: qsTr("FX PADS")
                color: Skin.text
                font.pixelSize: Skin.fontL
                font.bold: true
                font.letterSpacing: Px.px(2)
            }
            Item { Layout.fillWidth: true }
            Text {
                visible: root.mapping || Mixer.learning
                text: qsTr("MAPPING")
                color: Skin.solo
                font.pixelSize: Skin.fontXS
                font.bold: true
                font.letterSpacing: Px.px(1)
            }
        }

        // A thread of every pad's own colour, so the wheel the grid is drawn
        // from is visible before you have touched a single pad. Spelled out
        // rather than built from a Repeater: Gradient.stops only accepts
        // GradientStop instances, not a generator that produces them.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Px.px(3)
            radius: Skin.radiusS
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0 / 15; color: root.padHue(0) }
                GradientStop { position: 1 / 15; color: root.padHue(1) }
                GradientStop { position: 2 / 15; color: root.padHue(2) }
                GradientStop { position: 3 / 15; color: root.padHue(3) }
                GradientStop { position: 4 / 15; color: root.padHue(4) }
                GradientStop { position: 5 / 15; color: root.padHue(5) }
                GradientStop { position: 6 / 15; color: root.padHue(6) }
                GradientStop { position: 7 / 15; color: root.padHue(7) }
                GradientStop { position: 8 / 15; color: root.padHue(8) }
                GradientStop { position: 9 / 15; color: root.padHue(9) }
                GradientStop { position: 10 / 15; color: root.padHue(10) }
                GradientStop { position: 11 / 15; color: root.padHue(11) }
                GradientStop { position: 12 / 15; color: root.padHue(12) }
                GradientStop { position: 13 / 15; color: root.padHue(13) }
                GradientStop { position: 14 / 15; color: root.padHue(14) }
                GradientStop { position: 15 / 15; color: root.padHue(15) }
            }
        }

        // --- the grid --------------------------------------------------------
        GridLayout {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: 4
            rows: 4
            columnSpacing: Skin.spacingS
            rowSpacing: Skin.spacingS

            Repeater {
                model: 16

                Item {
                    id: pad
                    required property int index
                    readonly property int padIndex: pad.index
                    readonly property real amount: {
                        const v = root.amounts[pad.padIndex]
                        return typeof v === "number" ? v : 0
                    }
                    readonly property bool bipolar: Mixer.fxPadBipolar(pad.padIndex)
                    readonly property bool on: Math.abs(pad.amount) > 0.02
                    readonly property bool isMapped: root.mapped[pad.padIndex] === true
                    readonly property bool waiting: root.waitingPad === pad.padIndex
                    readonly property color hue: root.padHue(pad.padIndex)
                    property real pulse: 0.35

                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    SequentialAnimation on pulse {
                        running: pad.waiting
                        loops: Animation.Infinite
                        NumberAnimation { from: 0.25; to: 0.85; duration: 480; easing.type: Easing.InOutSine }
                        NumberAnimation { from: 0.85; to: 0.25; duration: 480; easing.type: Easing.InOutSine }
                    }

                    // Ambient glow. Outside `face`'s clip on purpose, so it
                    // bleeds past the pad's own edge instead of being cut off
                    // by it - the cheap, shader-free way to a soft halo.
                    Rectangle {
                        anchors.fill: face
                        anchors.margins: -Px.px(5)
                        radius: face.radius + Px.px(5)
                        color: "transparent"
                        border.width: Px.px(5)
                        border.color: pad.hue
                        opacity: pad.waiting ? pad.pulse : (pad.on ? 0.32 : 0)
                        visible: opacity > 0.01
                        Behavior on opacity { NumberAnimation { duration: Skin.medium } }
                    }

                    Rectangle {
                        id: face
                        anchors.fill: parent
                        radius: Skin.radiusL
                        color: Skin.slotEmpty
                        clip: true
                        border.width: pad.waiting ? Px.px(2) : Px.px(1.5)
                        // Idle pads still carry a faint tint of their own hue
                        // rather than a neutral grey rim, so the grid reads
                        // as colourful even before anything is pressed.
                        border.color: pad.waiting ? Skin.solo
                                     : pad.on ? pad.hue
                                     : root.mapping ? Skin.focus
                                     : pad.activeFocus ? Skin.focus
                                     : Qt.rgba(pad.hue.r, pad.hue.g, pad.hue.b, 0.4)

                        // Unipolar fills from the bottom; bipolar fills from
                        // the middle so up and down can be read at a glance.
                        Rectangle {
                            visible: !pad.bipolar
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: Math.max(0, Math.min(1, pad.amount)) * parent.height
                            gradient: Gradient {
                                GradientStop { position: 0.0; color: Qt.lighter(pad.hue, 1.35) }
                                GradientStop { position: 1.0; color: Qt.darker(pad.hue, 1.1) }
                            }
                            opacity: 0.88
                        }
                        Rectangle {
                            visible: !pad.bipolar && pad.amount > 0.02
                            anchors.left: parent.left
                            anchors.right: parent.right
                            y: parent.height - Math.max(0, Math.min(1, pad.amount)) * parent.height
                            height: Px.px(2)
                            color: Qt.lighter(pad.hue, 1.6)
                        }

                        Rectangle {
                            visible: pad.bipolar
                            anchors.left: parent.left
                            anchors.right: parent.right
                            y: pad.amount >= 0
                               ? parent.height / 2 - Math.abs(pad.amount) * parent.height / 2
                               : parent.height / 2
                            height: Math.abs(pad.amount) * parent.height / 2
                            gradient: Gradient {
                                GradientStop { position: pad.amount >= 0 ? 0.0 : 1.0; color: Qt.lighter(pad.hue, 1.35) }
                                GradientStop { position: pad.amount >= 0 ? 1.0 : 0.0; color: Qt.darker(pad.hue, 1.1) }
                            }
                            opacity: 0.88
                        }
                        Rectangle {
                            visible: pad.bipolar && Math.abs(pad.amount) > 0.02
                            anchors.left: parent.left
                            anchors.right: parent.right
                            y: pad.amount >= 0
                               ? parent.height / 2 - Math.abs(pad.amount) * parent.height / 2
                               : parent.height / 2 + Math.abs(pad.amount) * parent.height / 2 - Px.px(2)
                            height: Px.px(2)
                            color: Qt.lighter(pad.hue, 1.6)
                        }

                        Rectangle {
                            visible: pad.bipolar
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            height: 1
                            color: Skin.border
                        }

                        // The label sits on its own dark chip rather than
                        // straight on the fill, so it stays legible whatever
                        // colour and however much of the pad is lit.
                        Rectangle {
                            anchors.top: parent.top
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.topMargin: Skin.spacingXS
                            radius: Skin.radiusS
                            color: Qt.rgba(0, 0, 0, 0.4)
                            width: label.implicitWidth + Skin.spacingS * 2
                            height: label.implicitHeight + Skin.spacingXS * 1.5

                            Text {
                                id: label
                                anchors.centerIn: parent
                                text: root.names[pad.padIndex]
                                color: Skin.text
                                font.pixelSize: Skin.fontXS
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                width: face.width - Skin.spacingL
                            }
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: Skin.spacingXS
                            visible: pad.on
                            text: pad.bipolar
                                  ? (pad.amount > 0 ? "+" : "") + Math.round(pad.amount * 100)
                                  : Math.round(pad.amount * 100)
                            color: Skin.text
                            font.pixelSize: Skin.fontS
                            font.bold: true
                            font.family: Skin.monoFamily
                            style: Text.Outline
                            styleColor: Qt.rgba(0, 0, 0, 0.55)
                        }

                        Rectangle {
                            visible: pad.isMapped
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: Skin.spacingXS
                            width: Px.px(8)
                            height: Px.px(8)
                            radius: width / 2
                            color: "transparent"
                            border.width: Px.px(2)
                            border.color: pad.waiting ? Skin.solo : pad.hue
                        }
                    }

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

        // --- transport strip: MAP / HOLD / status ---------------------------
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingXS

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Skin.border
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Skin.spacingS

                StripButton {
                    Layout.preferredWidth: Px.px(88)
                    Layout.preferredHeight: Px.px(36)
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
                    Layout.preferredWidth: Px.px(120)
                    Layout.preferredHeight: Px.px(36)
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
                        anchors.margins: Px.px(4)
                        width: Px.px(8)
                        height: Px.px(8)
                        radius: width / 2
                        color: "transparent"
                        border.width: Px.px(2)
                        border.color: root.waitingPad === 16 ? Skin.solo : Skin.focus
                    }
                }

                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: root.waitingPad >= 0
                      ? qsTr("Turn a knob on this strip's MIDI input… Esc cancels.")
                      : root.mapping
                        ? qsTr("Tap the pad you want, then turn a knob.")
                        : qsTr("Top is full. Pitch, Filter, Comb and Ring go both ways from the middle.")
                color: root.mapping || Mixer.learning ? Skin.solo : Skin.textDim
                font.pixelSize: Skin.fontXS
                wrapMode: Text.WordWrap
            }
        }
    }
}
