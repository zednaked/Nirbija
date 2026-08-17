pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// The step sequencer's own editor. The generic one renders it as a wall of
// sliders, which proves the plugin works and is no way to play it.
//
// One column per step: the bar's height is the note, tapping it turns the
// step on or off, a drag pitches it (snapped to the scale), the strip at
// the foot is how hard it hits, the thinner one above that is chance.
// Accent is the tick at the top; the notch on the right edge is a tie.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property string pluginName: ""

    readonly property int idDivision: 0
    readonly property int idLength: 1
    readonly property int idGate: 2
    readonly property int idTranspose: 3
    readonly property int idSwing: 5
    readonly property int idDirection: 6
    readonly property int idScale: 7
    readonly property int idRoot: 8
    readonly property int idNudgeLeft: 9
    readonly property int idNudgeRight: 10
    readonly property int idRandomHits: 11
    readonly property int idRandomNotes: 12
    readonly property int idClearHits: 13
    readonly property int idEuclid: 14
    readonly property int idStepNote: 16
    readonly property int idStepVelocity: 48
    readonly property int idStepActive: 80
    readonly property int idStepProb: 112
    readonly property int idStepAccent: 144
    readonly property int idStepTie: 176
    readonly property int stepCount: 16

    readonly property int lowNote: 36
    readonly property int highNote: 84

    readonly property var scaleNames: [
        qsTr("chrom"), qsTr("maj"), qsTr("min"), qsTr("dor"),
        qsTr("mix"), qsTr("p−"), qsTr("p+"), qsTr("blues")
    ]
    readonly property var rootNames: [
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    ]

    property var notes: []
    property var velocities: []
    property var actives: []
    property var chances: []
    property var accents: []
    property var ties: []
    property int division: 2
    property int length: 16
    property real gate: 0.5
    property int transpose: 0
    property real swing: 0
    property int direction: 0
    property int scaleId: 0
    property int root: 0
    property int euclid: 0
    property int playhead: -1

    width: Px.px(640)
    height: Px.px(540)
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
        WheelHandler {
            acceptedModifiers: Qt.NoModifier
            onWheel: event => event.accepted = true
        }
        WheelHandler {
            acceptedModifiers: Qt.ShiftModifier
            onWheel: event => event.accepted = true
        }
    }

    background: Rectangle {
        color: Skin.popup
        border.width: 1
        border.color: Skin.border
        radius: Skin.radiusL

        HoverHandler {}
        TapHandler {}
        DragHandler {
            target: null
            grabPermissions: PointerHandler.TakeOverForbidden
        }
        WheelHandler {
            acceptedModifiers: Qt.NoModifier
            onWheel: event => event.accepted = true
        }
        WheelHandler {
            acceptedModifiers: Qt.ShiftModifier
            onWheel: event => event.accepted = true
        }
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Skin.fast }
    }

    onClosed: playhead = -1

    function readAll() {
        const values = Mixer.insertParameters(root.targetRow, root.targetSlot)
        const byId = {}
        for (const entry of values) byId[entry.id] = entry.value

        const notes = []
        const velocities = []
        const actives = []
        const chances = []
        const accents = []
        const ties = []
        for (let i = 0; i < root.stepCount; ++i) {
            notes.push(byId[root.idStepNote + i])
            velocities.push(byId[root.idStepVelocity + i])
            actives.push(byId[root.idStepActive + i] >= 0.5)
            chances.push(byId[root.idStepProb + i] ?? 1)
            accents.push(byId[root.idStepAccent + i] >= 0.5)
            ties.push(byId[root.idStepTie + i] >= 0.5)
        }
        root.notes = notes
        root.velocities = velocities
        root.actives = actives
        root.chances = chances
        root.accents = accents
        root.ties = ties
        root.division = byId[root.idDivision]
        root.length = byId[root.idLength]
        root.gate = byId[root.idGate]
        root.transpose = byId[root.idTranspose]
        root.swing = byId[root.idSwing] ?? 0
        root.direction = byId[root.idDirection] ?? 0
        root.scaleId = byId[root.idScale] ?? 0
        root.root = byId[root.idRoot] ?? 0
        root.euclid = byId[root.idEuclid] ?? 0
    }

    function openFor(row, slot) {
        root.targetRow = row
        root.targetSlot = slot
        root.pluginName = Mixer.insertName(row, slot)
        root.readAll()
        root.open()
    }

    function setParam(id, value) {
        Mixer.setInsertParameter(root.targetRow, root.targetSlot, id, value)
    }

    function fire(id) {
        root.setParam(id, 1)
        root.readAll()
    }

    function setStep(list, index, value) {
        const copy = list.slice()
        copy[index] = value
        return copy
    }

    function noteName(midi) {
        const names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
        const n = Math.round(midi)
        return names[((n % 12) + 12) % 12] + (Math.floor(n / 12) - 1)
    }

    // Same degrees the engine uses, so a drag lands on what will actually play.
    function snapNote(midi) {
        const tables = [
            null,
            [0, 2, 4, 5, 7, 9, 11],
            [0, 2, 3, 5, 7, 8, 10],
            [0, 2, 3, 5, 7, 9, 10],
            [0, 2, 4, 5, 7, 9, 10],
            [0, 3, 5, 7, 10],
            [0, 2, 4, 7, 9],
            [0, 3, 5, 6, 7, 10]
        ]
        const deg = tables[root.scaleId]
        if (!deg) return Math.round(midi)
        const rootPc = root.root
        let best = Math.round(midi)
        let bestD = 128
        const want = Math.round(midi)
        for (let n = want - 6; n <= want + 6; ++n) {
            if (n < 0 || n > 127) continue
            const pc = ((n - rootPc) % 12 + 12) % 12
            if (deg.indexOf(pc) < 0) continue
            const d = Math.abs(n - want)
            if (d < bestD) {
                bestD = d
                best = n
            }
        }
        return best
    }

    function heardNote(midi) {
        return root.snapNote(midi + root.transpose)
    }

    Timer {
        running: root.visible
        interval: 50
        repeat: true
        onTriggered: root.playhead = Mixer.insertPlayhead(root.targetRow,
                                                          root.targetSlot)
    }

    contentItem: ColumnLayout {
        spacing: Skin.spacingS

        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingS

            Text {
                text: root.pluginName
                color: Skin.text
                font.pixelSize: Skin.fontL
                font.bold: true
            }

            Text {
                text: root.direction === 1 ? qsTr("reverse")
                    : root.direction === 2 ? qsTr("pendulum")
                    : root.direction === 3 ? qsTr("random")
                    : qsTr("forward")
                color: Skin.textDim
                font.pixelSize: Skin.fontS
            }

            Item { Layout.fillWidth: true }

            StripButton {
                Layout.preferredWidth: Px.px(36)
                label: "←"
                tip: qsTr("Nudge the pattern one step earlier.")
                onClicked: root.fire(root.idNudgeLeft)
            }
            StripButton {
                Layout.preferredWidth: Px.px(36)
                label: "→"
                tip: qsTr("Nudge the pattern one step later.")
                onClicked: root.fire(root.idNudgeRight)
            }
            StripButton {
                Layout.preferredWidth: Px.px(52)
                label: qsTr("Hits")
                tip: qsTr("Randomize which steps are on. Notes stay.")
                onClicked: root.fire(root.idRandomHits)
            }
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Notes")
                tip: qsTr("Randomize pitches, snapped to the scale.")
                onClicked: root.fire(root.idRandomNotes)
            }
            StripButton {
                Layout.preferredWidth: Px.px(52)
                label: qsTr("Clear")
                danger: true
                tip: qsTr("Turn every step off. The pitches stay.")
                onClicked: root.fire(root.idClearHits)
            }
        }

        // --- the grid ---------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Skin.slotEmpty
            radius: Skin.radius
            border.width: 1
            border.color: Skin.border
            clip: true

            Row {
                id: columns
                anchors.fill: parent
                anchors.margins: Skin.spacingXS
                spacing: Px.px(2)

                Repeater {
                    model: root.stepCount

                    Item {
                        id: column
                        required property int index

                        readonly property bool on: root.actives[column.index] === true
                        readonly property bool inPattern: column.index < root.length
                        readonly property bool atPlayhead: root.playhead === column.index
                        readonly property real note: root.notes[column.index] || 60
                        readonly property real velocity: root.velocities[column.index] || 100
                        readonly property real chance: root.chances[column.index] ?? 1
                        readonly property bool accented: root.accents[column.index] === true
                        readonly property bool tied: root.ties[column.index] === true

                        width: (columns.width - (root.stepCount - 1) * Px.px(2))
                               / root.stepCount
                        height: columns.height
                        opacity: column.inPattern ? 1.0 : 0.3

                        Rectangle {
                            anchors.fill: parent
                            color: column.index % 4 === 0 ? Skin.strip : "transparent"
                            radius: Skin.radiusS
                        }

                        Rectangle {
                            anchors.fill: parent
                            radius: Skin.radiusS
                            color: Skin.accent
                            opacity: column.atPlayhead ? 0.22 : 0
                            Behavior on opacity {
                                NumberAnimation { duration: Skin.fast }
                            }
                        }

                        // Accent: a tap up here, not on the bar, so pitching
                        // never turns it on by accident.
                        Rectangle {
                            id: accentTick
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: Px.px(3)
                            height: Px.px(8)
                            radius: Skin.radiusS
                            color: column.accented ? Skin.solo : Skin.slot
                            opacity: column.on ? 1 : 0.45

                            TapHandler {
                                onTapped: {
                                    const now = !column.accented
                                    root.accents = root.setStep(root.accents,
                                                                column.index, now)
                                    root.setParam(root.idStepAccent + column.index,
                                                  now ? 1 : 0)
                                }
                            }
                        }

                        Rectangle {
                            id: bar
                            readonly property real fraction:
                                (root.heardNote(column.note) - root.lowNote) /
                                (root.highNote - root.lowNote)

                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.margins: Px.px(3)
                            anchors.top: accentTick.bottom
                            anchors.topMargin: Px.px(2)
                            anchors.bottom: chanceStrip.top
                            anchors.bottomMargin: Px.px(2)
                            radius: Skin.radiusS
                            color: column.on ? Skin.accent : Skin.slot
                            opacity: column.on
                                     ? 0.45 + 0.55 * (column.velocity / 127)
                                     : 0.45

                            // The fill is the pitch, sitting on the floor of
                            // the cell so a melody is still a skyline.
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: Math.max(Px.px(4),
                                                 Math.min(1, Math.max(0, bar.fraction))
                                                 * parent.height)
                                radius: Skin.radiusS
                                color: column.on ? Skin.accent : Skin.slotHover
                            }

                            Behavior on color {
                                ColorAnimation { duration: Skin.fast }
                            }
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: bar.bottom
                            anchors.bottomMargin: Px.px(2)
                            visible: column.on && column.width > Px.px(22)
                            text: root.noteName(root.heardNote(column.note))
                            color: Skin.onAccent
                            font.pixelSize: Skin.fontXS
                        }

                        // Tie: the notch on the trailing edge. On, this step
                        // holds into the next instead of retriggering.
                        Rectangle {
                            visible: column.index < root.length - 1
                            anchors.right: parent.right
                            anchors.verticalCenter: bar.verticalCenter
                            width: Px.px(5)
                            height: Px.px(10)
                            radius: 1
                            color: column.tied ? Skin.meterLow : Skin.border
                            opacity: column.on ? 1 : 0.35

                            TapHandler {
                                onTapped: {
                                    const now = !column.tied
                                    root.ties = root.setStep(root.ties,
                                                             column.index, now)
                                    root.setParam(root.idStepTie + column.index,
                                                  now ? 1 : 0)
                                }
                            }
                        }

                        Rectangle {
                            id: chanceStrip
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: velocityStrip.top
                            anchors.bottomMargin: Px.px(2)
                            anchors.margins: Px.px(3)
                            height: Px.px(6)
                            radius: Skin.radiusS
                            color: Skin.slot

                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: parent.width * column.chance
                                radius: Skin.radiusS
                                color: Skin.solo
                                opacity: column.on ? 0.8 : 0.25
                            }

                            MouseArea {
                                anchors.fill: parent
                                onPositionChanged: mouse => {
                                    const value = Math.max(0, Math.min(1,
                                        mouse.x / Math.max(1, width)))
                                    root.chances = root.setStep(
                                        root.chances, column.index, value)
                                    root.setParam(root.idStepProb + column.index,
                                                  value)
                                }
                                onPressed: mouse => positionChanged(mouse)
                            }
                        }

                        Rectangle {
                            id: velocityStrip
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: Px.px(3)
                            height: Px.px(10)
                            radius: Skin.radiusS
                            color: Skin.slot

                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: parent.width * (column.velocity / 127)
                                radius: Skin.radiusS
                                color: Skin.accent
                                opacity: column.on ? 0.7 : 0.25
                            }

                            MouseArea {
                                anchors.fill: parent
                                onPositionChanged: mouse => {
                                    const value = Math.round(
                                        Math.max(1, Math.min(127,
                                            mouse.x / width * 127)))
                                    root.velocities = root.setStep(
                                        root.velocities, column.index, value)
                                    root.setParam(root.idStepVelocity + column.index,
                                                  value)
                                }
                                onPressed: mouse => positionChanged(mouse)
                            }
                        }

                        TapHandler {
                            onSingleTapped: {
                                const now = !column.on
                                root.actives = root.setStep(root.actives,
                                                            column.index, now)
                                root.setParam(root.idStepActive + column.index,
                                              now ? 1 : 0)
                            }
                        }

                        DragHandler {
                            id: pitchDrag
                            target: null
                            yAxis.enabled: true
                            xAxis.enabled: false

                            property real startNote: 60

                            onActiveChanged: {
                                if (active) pitchDrag.startNote = column.note
                            }
                            onTranslationChanged: {
                                if (!pitchDrag.active) return
                                const span = root.highNote - root.lowNote
                                const moved = -pitchDrag.translation.y
                                              / column.height * span
                                const value = root.snapNote(Math.max(
                                    root.lowNote,
                                    Math.min(root.highNote,
                                             pitchDrag.startNote + moved)))
                                root.notes = root.setStep(root.notes,
                                                          column.index, value)
                                root.setParam(root.idStepNote + column.index, value)
                            }
                        }
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Tap a step to arm it · drag to pitch · gold tick is accent · green notch is a tie · yellow strip is chance")
            color: Skin.textDim
            font.pixelSize: Skin.fontXS
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingXS

            component DirButton: StripButton {
                required property int forValue
                Layout.preferredWidth: Px.px(40)
                active: root.direction === forValue
                onClicked: {
                    root.direction = forValue
                    root.setParam(root.idDirection, forValue)
                }
            }
            DirButton { forValue: 0; label: "→"; tip: qsTr("Forward.") }
            DirButton { forValue: 1; label: "←"; tip: qsTr("Reverse.") }
            DirButton { forValue: 2; label: "↔"; tip: qsTr("Pendulum.") }
            DirButton { forValue: 3; label: qsTr("?"); tip: qsTr("A new step each time.") }

            Item { Layout.preferredWidth: Skin.spacing }

            Repeater {
                model: root.scaleNames
                StripButton {
                    required property int index
                    required property string modelData
                    Layout.preferredWidth: Px.px(40)
                    label: modelData
                    active: root.scaleId === index
                    tip: qsTr("Notes snap to this scale.")
                    onClicked: {
                        root.scaleId = index
                        root.setParam(root.idScale, index)
                    }
                }
            }

            StripButton {
                Layout.preferredWidth: Px.px(36)
                label: root.rootNames[root.root] || "C"
                tip: qsTr("Root of the scale. Click to walk it.")
                onClicked: {
                    root.root = (root.root + 1) % 12
                    root.setParam(root.idRoot, root.root)
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 3
            columnSpacing: Skin.spacing
            rowSpacing: Skin.spacingXS

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(22)
                label: qsTr("steps")
                valueText: Math.round(root.length)
                value: (root.length - 1) / 15
                step: 1 / 15
                fineStep: 1 / 15
                onMoved: v => {
                    root.length = Math.round(1 + v * 15)
                    root.setParam(root.idLength, root.length)
                }
            }

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(22)
                label: qsTr("division")
                valueText: ["1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T"][root.division]
                value: root.division / 5
                step: 1 / 5
                fineStep: 1 / 5
                onMoved: v => {
                    root.division = Math.round(v * 5)
                    root.setParam(root.idDivision, root.division)
                }
            }

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(22)
                label: qsTr("swing")
                valueText: Math.round(root.swing * 100) + "%"
                value: root.swing
                tip: qsTr("Off-beats lean late. 0 is straight.")
                onMoved: v => {
                    root.swing = v
                    root.setParam(root.idSwing, v)
                }
            }

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(22)
                label: qsTr("gate")
                valueText: Math.round(root.gate * 100) + "%"
                value: (root.gate - 0.05) / 0.95
                onMoved: v => {
                    root.gate = 0.05 + v * 0.95
                    root.setParam(root.idGate, root.gate)
                }
            }

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(22)
                label: qsTr("transpose")
                valueText: (root.transpose > 0 ? "+" : "") + Math.round(root.transpose)
                value: (root.transpose + 24) / 48
                step: 1 / 48
                fineStep: 1 / 48
                onMoved: v => {
                    root.transpose = Math.round(v * 48 - 24)
                    root.setParam(root.idTranspose, root.transpose)
                }
            }

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(22)
                label: qsTr("euclid")
                valueText: Math.round(root.euclid)
                value: root.euclid / 16
                step: 1 / 16
                fineStep: 1 / 16
                tip: qsTr("Spread this many hits evenly across the pattern.")
                onMoved: v => {
                    root.euclid = Math.round(v * 16)
                    root.setParam(root.idEuclid, root.euclid)
                    root.readAll()
                }
            }
        }
    }
}
