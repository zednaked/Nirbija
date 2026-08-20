pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// The step sequencer's own editor. The generic one renders it as a wall of
// sliders, which proves the plugin works and is no way to play it - and an
// 8x64 grid with p-locks does not fit that interface at all.
//
// Two views on the same eight lanes: skyline is the 303 - one lane, pitch as
// a bar, drag to pitch it, snapped to the scale. Grid is the 808 - all eight
// lanes as pads, on/off, velocity in the opacity. A lane rail on the left
// picks focus and mute in either view. Sixty-four steps per lane page in
// sixteens; sixteen patterns, edited here but only pattern 0 audible until
// the bank lands.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property string pluginName: ""

    readonly property int idDivision: 0
    readonly property int idLength: 1
    readonly property int idGate: 2
    readonly property int idTranspose: 3
    readonly property int idChannel: 4
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
    readonly property int idRecordArm: 15
    readonly property int idDensity: 192
    readonly property int idChaos: 193
    readonly property int idRatchetAmount: 194
    readonly property int idMasterProb: 195
    readonly property int idPattern: 196
    readonly property int idNextPattern: 197
    readonly property int idFill: 198
    readonly property int idFocusedLane: 199
    readonly property int idView: 200
    readonly property int idMutate: 201

    readonly property int laneCount: 8
    readonly property int maxSteps: 64
    readonly property int pageSteps: 16
    readonly property int patternCount: 16

    readonly property int lowNote: 36
    readonly property int highNote: 84

    readonly property var scaleNames: [
        qsTr("chrom"), qsTr("maj"), qsTr("min"), qsTr("dor"),
        qsTr("mix"), qsTr("p−"), qsTr("p+"), qsTr("blues")
    ]
    readonly property var rootNames: [
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    ]

    // --- current-pattern planes, lane-major: index = lane * 64 + step --------
    property var on: []
    property var accent: []
    property var tie: []
    property var note: []
    property var vel: []
    property var chance: []
    property var ratchet: []
    property var cond: []
    property var condArg: []
    property var micro: []
    property int editStep: 0

    // --- per-lane state, 8 * (note, length, mute, channel, division,
    //     direction, euclid) -----------------------------------------------
    property var lanes: []
    property var gates: []
    // Packed light per head: bit 16 set for an extra head, lane in bits 8-15,
    // step in bits 0-7. First eight entries are the native heads, in lane order.
    property var heads: []

    property var macros: [1, 0, 0, 1]
    property int pattern: 0
    property bool fill: false
    property bool recording: false
    property int focusedLane: 0
    property int viewMode: 0  // 0 skyline, 1 grid
    // Which page of 16 steps is on screen. QML-only - a restart always comes
    // back to page 0, and two editors open on the same insert can look at
    // different pages without fighting over one number in the blob.
    property int page: 0

    property int transpose: 0
    property real swing: 0
    property int scaleId: 0
    property int root: 0
    property int playhead: -1
    // Set once, the first time this ever opens - after that the popup stays
    // wherever it was last dragged, the same as a real tool window would.
    property bool positioned: false

    readonly property int focusedLength: root.laneLength(root.focusedLane)
    readonly property int pageCount: Math.max(1, Math.ceil(
        (root.viewMode === 1 ? root.maxSteps : root.focusedLength) / root.pageSteps))
    readonly property int stepOffset: Math.min(root.page, root.pageCount - 1) * root.pageSteps

    width: Px.px(760)
    height: Px.px(680)
    // Not modal: the mixer behind it stays live, so a fader or the transport
    // is still reachable with this open - the whole point of it being a tool
    // window rather than a dialog. Dragging the empty background moves it;
    // see the DragHandler below.
    modal: false
    padding: Skin.spacingL
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        color: Skin.popup
        border.width: 1
        border.color: Skin.border
        radius: Skin.radiusL

        HoverHandler {}
        TapHandler {}
        // Empty chrome is not a handler on its own, so without this a press
        // on the padding falls through onto whatever strip sits underneath -
        // and, now that the popup can sit anywhere, doubles as how it moves.
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

    // --- lane accessors --------------------------------------------------------
    function laneNote(lane) { return root.lanes[lane * 7 + 0] ?? 60 }
    function laneLength(lane) { return root.lanes[lane * 7 + 1] ?? 16 }
    function laneMuted(lane) { return (root.lanes[lane * 7 + 2] ?? 0) !== 0 }
    function laneChannel(lane) { return root.lanes[lane * 7 + 3] ?? 0 }
    function laneDivision(lane) { return root.lanes[lane * 7 + 4] ?? 2 }
    function laneDirection(lane) { return root.lanes[lane * 7 + 5] ?? 0 }
    function laneEuclid(lane) { return root.lanes[lane * 7 + 6] ?? 0 }
    function laneGate(lane) { return root.gates[lane] ?? 0.5 }

    // The step a native head is on right now, or -1. Extra heads are not lit
    // here - four more moving dots on a kit view is noise before Fugue exists.
    function nativeHeadStep(lane) {
        if (root.playhead < 0) return -1
        const packed = root.heads[lane]
        if (packed === undefined || packed < 0) return -1
        return packed & 0xff
    }

    function extraHeadOnStep(lane, step) {
        if (root.playhead < 0) return false
        for (let i = 8; i < root.heads.length; ++i) {
            const packed = root.heads[i]
            if (packed === undefined || packed < 0) continue
            if ((packed & 0x10000) === 0) continue
            if (((packed >> 8) & 0xff) === lane && (packed & 0xff) === step)
                return true
        }
        return false
    }

    // --- cell accessors, current pattern ----------------------------------------
    function cellIndex(lane, step) { return lane * root.maxSteps + step }
    function cellOn(lane, step) { return (root.on[root.cellIndex(lane, step)] ?? 0) !== 0 }
    function cellAccent(lane, step) {
        return (root.accent[root.cellIndex(lane, step)] ?? 0) !== 0
    }
    function cellTie(lane, step) { return (root.tie[root.cellIndex(lane, step)] ?? 0) !== 0 }
    function cellNote(lane, step) { return root.note[root.cellIndex(lane, step)] ?? 60 }
    function cellVel(lane, step) { return root.vel[root.cellIndex(lane, step)] ?? 100 }
    function cellChance(lane, step) { return root.chance[root.cellIndex(lane, step)] ?? 1 }
    function cellRatchet(lane, step) { return root.ratchet[root.cellIndex(lane, step)] ?? 1 }
    function cellCond(lane, step) { return root.cond[root.cellIndex(lane, step)] ?? 0 }
    function cellMicro(lane, step) { return root.micro[root.cellIndex(lane, step)] ?? 0 }

    function sendTrig(lane, step) {
        const i = root.cellIndex(lane, step)
        Mixer.setSequencerTrig(
            root.targetRow, root.targetSlot, root.pattern, lane, step,
            root.micro[i] ?? 0, Math.round(root.ratchet[i] ?? 1),
            Math.round(root.cond[i] ?? 0), Math.round(root.condArg[i] ?? 0))
    }

    function setField(list, index, value) {
        const copy = list.slice()
        copy[index] = value
        return copy
    }

    // Sends the whole cell - the Mixer has no partial setter, so any single
    // field change reads the rest back out of the arrays already in memory.
    function sendCell(lane, step) {
        root.editStep = step
        const i = root.cellIndex(lane, step)
        Mixer.setSequencerCell(
            root.targetRow, root.targetSlot, root.pattern, lane, step,
            Math.round(root.note[i] ?? 60), Math.round(root.vel[i] ?? 100),
            (root.on[i] ?? 0) !== 0, root.chance[i] ?? 1,
            (root.accent[i] ?? 0) !== 0, (root.tie[i] ?? 0) !== 0)
    }

    function armNote(lane, step, noteValue) {
        const i = root.cellIndex(lane, step)
        root.note = root.setField(root.note, i, noteValue)
        root.on = root.setField(root.on, i, 1)
        root.sendCell(lane, step)
    }

    function setOn(lane, step, value) {
        const i = root.cellIndex(lane, step)
        root.on = root.setField(root.on, i, value ? 1 : 0)
        root.sendCell(lane, step)
    }

    function setVelocity(lane, step, value) {
        const i = root.cellIndex(lane, step)
        root.vel = root.setField(root.vel, i, value)
        root.sendCell(lane, step)
    }

    function setChance(lane, step, value) {
        const i = root.cellIndex(lane, step)
        root.chance = root.setField(root.chance, i, value)
        root.sendCell(lane, step)
    }

    function setAccent(lane, step, value) {
        const i = root.cellIndex(lane, step)
        root.accent = root.setField(root.accent, i, value)
        root.sendCell(lane, step)
    }

    function setTie(lane, step, value) {
        const i = root.cellIndex(lane, step)
        root.tie = root.setField(root.tie, i, value)
        root.sendCell(lane, step)
    }

    // Holding the first step and pulling sideways stamps every step the
    // pointer crosses with that same note, on the way to painting a
    // baseline in one pass instead of one drag per step.
    function paintNotes(lane, fromStep, toStep, value) {
        const lo = Math.min(fromStep, toStep)
        const hi = Math.max(fromStep, toStep)
        for (let i = lo; i <= hi; ++i) root.armNote(lane, i, value)
    }

    function readAll() {
        const snap = Mixer.insertSequencerSnapshot(root.targetRow, root.targetSlot)
        root.on = snap.on || []
        root.accent = snap.accent || []
        root.tie = snap.tie || []
        root.note = snap.note || []
        root.vel = snap.vel || []
        root.chance = snap.chance || []
        root.ratchet = snap.ratchet || []
        root.cond = snap.cond || []
        root.condArg = snap.condArg || []
        root.micro = snap.micro || []
        root.lanes = snap.lanes || []
        root.gates = snap.gates || []
        root.heads = snap.heads || []
        root.macros = snap.macros || [1, 0, 0, 1]
        root.pattern = snap.pattern ?? 0
        root.fill = snap.fill === true || snap.fill === 1
        root.recording = snap.recording === true || snap.recording === 1
        root.focusedLane = snap.focusedLane ?? 0
        root.viewMode = snap.view ?? 0
        root.transpose = snap.transpose ?? 0
        root.swing = snap.swing ?? 0
        root.scaleId = snap.scale ?? 0
        root.root = snap.root ?? 0
    }

    function openFor(row, slot) {
        root.targetRow = row
        root.targetSlot = slot
        root.pluginName = Mixer.insertName(row, slot)
        root.readAll()
        root.page = 0
        if (!root.positioned) {
            root.x = Math.round((Overlay.overlay.width - root.width) / 2)
            root.y = Math.round((Overlay.overlay.height - root.height) / 2)
            root.positioned = true
        }
        root.open()
    }

    function setParam(id, value) {
        Mixer.setInsertParameter(root.targetRow, root.targetSlot, id, value)
    }

    function fire(id) {
        root.setParam(id, 1)
        root.readAll()
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

    // A click's y within a column, straight to the note under it - no more
    // clicking, holding, and dragging up by however far the old note
    // happened to be from wherever the press landed.
    function pitchFromY(y, span) {
        const fraction = 1 - Math.max(0, Math.min(span, y)) / Math.max(1, span)
        const midi = root.lowNote + fraction * (root.highNote - root.lowNote)
        return root.snapNote(Math.round(
            Math.max(root.lowNote, Math.min(root.highNote, midi))))
    }

    Timer {
        running: root.visible
        interval: 50
        repeat: true
        onTriggered: {
            root.playhead = Mixer.insertPlayhead(root.targetRow, root.targetSlot)
            // The full snapshot, not just the playhead: the grid needs all
            // eight heads lit, and Record needs the pattern it is painting.
            // insertSequencerSnapshot is already the size of an insertParameters
            // call on a plugin with a hundred-odd parameters - the same 50ms
            // timer already paid for that elsewhere.
            root.readAll()
        }
    }

    contentItem: ColumnLayout {
        spacing: Skin.spacingS

        Text {
            text: root.pluginName
            color: Skin.text
            font.pixelSize: Skin.fontL
            font.bold: true
        }

        // --- performance row: always visible, works from across the room -------
        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingXS

            StripButton {
                Layout.preferredWidth: Px.px(52)
                label: qsTr("Rec")
                active: root.recording
                activeColor: Skin.mute
                tip: qsTr("Play your MIDI input and it lands on the nearest step, with its velocity and how long you held it, on the focused lane. The pattern itself goes quiet while this is on, so only what you play sounds.")
                onClicked: {
                    root.setParam(root.idRecordArm, root.recording ? 0 : 1)
                    root.readAll()
                }
            }
            StripButton {
                Layout.preferredWidth: Px.px(52)
                label: qsTr("Fill")
                active: root.fill
                activeColor: Skin.solo
                tip: qsTr("Fill. Steps with the Fill condition only fire while this is on; NotFill does the opposite.")
                onClicked: {
                    root.setParam(root.idFill, root.fill ? 0 : 1)
                    root.readAll()
                }
            }

            Item { Layout.preferredWidth: Skin.spacingS }

            Repeater {
                model: root.patternCount
                StripButton {
                    required property int index
                    Layout.preferredWidth: Px.px(22)
                    Layout.preferredHeight: Px.px(22)
                    label: String(index + 1)
                    flat: true
                    active: root.pattern === index
                    tip: qsTr("Pattern %1. Instant switch — notes already in the air keep their gate.").arg(index + 1)
                    onClicked: {
                        root.setParam(root.idPattern, index)
                        root.readAll()
                    }
                }
            }

            Item { Layout.fillWidth: true }

            ValueTrack {
                Layout.preferredWidth: Px.px(76)
                Layout.preferredHeight: Px.px(22)
                label: qsTr("dens")
                valueText: (root.macros[0] ?? 1).toFixed(2)
                value: (root.macros[0] ?? 1) / 2
                tip: qsTr("Density. Above the middle, off steps can fire as quiet ghost notes.")
                onMoved: v => root.setParam(root.idDensity, v * 2)
            }
            ValueTrack {
                Layout.preferredWidth: Px.px(76)
                Layout.preferredHeight: Px.px(22)
                label: qsTr("chaos")
                valueText: (root.macros[1] ?? 0).toFixed(2)
                value: root.macros[1] ?? 0
                tip: qsTr("Chaos. No audible effect yet - lands with microtiming.")
                onMoved: v => root.setParam(root.idChaos, v)
            }
            ValueTrack {
                Layout.preferredWidth: Px.px(76)
                Layout.preferredHeight: Px.px(22)
                label: qsTr("rtch")
                valueText: (root.macros[2] ?? 0).toFixed(2)
                value: root.macros[2] ?? 0
                tip: qsTr("Ratchet amount. No audible effect yet - lands with the ratchet scheduler.")
                onMoved: v => root.setParam(root.idRatchetAmount, v)
            }
            ValueTrack {
                Layout.preferredWidth: Px.px(76)
                Layout.preferredHeight: Px.px(22)
                label: qsTr("prob")
                valueText: (root.macros[3] ?? 1).toFixed(2)
                value: root.macros[3] ?? 1
                tip: qsTr("Master probability. A ceiling on every step's own chance.")
                onMoved: v => root.setParam(root.idMasterProb, v)
            }
        }

        // --- edit row: view, nudge, generators, direction/scale --------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingXS

            StripButton {
                Layout.preferredWidth: Px.px(58)
                label: qsTr("Skyline")
                active: root.viewMode === 0
                tip: qsTr("One lane, pitch as a bar. The instrument for a melody or a bassline.")
                onClicked: { root.setParam(root.idView, 0); root.page = 0; root.readAll() }
            }
            StripButton {
                Layout.preferredWidth: Px.px(48)
                label: qsTr("Grid")
                active: root.viewMode === 1
                tip: qsTr("All eight lanes as pads. The instrument for a kit.")
                onClicked: { root.setParam(root.idView, 1); root.page = 0; root.readAll() }
            }

            Item { Layout.preferredWidth: Skin.spacingS }

            StripButton {
                Layout.preferredWidth: Px.px(36)
                label: "←"
                tip: qsTr("Nudge the focused lane's pattern one step earlier.")
                onClicked: root.fire(root.idNudgeLeft)
            }
            StripButton {
                Layout.preferredWidth: Px.px(36)
                label: "→"
                tip: qsTr("Nudge the focused lane's pattern one step later.")
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
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Mutate")
                tip: qsTr("Nudge the current pattern: a few hits flip, locked pitches walk a scale degree.")
                onClicked: root.fire(root.idMutate)
            }

            Item { Layout.fillWidth: true }

            component DirButton: StripButton {
                required property int forValue
                Layout.preferredWidth: Px.px(32)
                active: root.laneDirection(root.focusedLane) === forValue
                onClicked: root.setParam(root.idDirection, forValue)
            }
            DirButton { forValue: 0; label: "→"; tip: qsTr("Forward.") }
            DirButton { forValue: 1; label: "←"; tip: qsTr("Reverse.") }
            DirButton { forValue: 2; label: "↔"; tip: qsTr("Pendulum.") }
            DirButton { forValue: 3; label: qsTr("?"); tip: qsTr("A new step each time.") }

            Item { Layout.preferredWidth: Skin.spacingS }

            Repeater {
                model: root.scaleNames
                StripButton {
                    required property int index
                    required property string modelData
                    Layout.preferredWidth: Px.px(36)
                    label: modelData
                    active: root.scaleId === index
                    tip: qsTr("Notes snap to this scale.")
                    onClicked: root.setParam(root.idScale, index)
                }
            }
            StripButton {
                Layout.preferredWidth: Px.px(32)
                label: root.rootNames[root.root] || "C"
                tip: qsTr("Root of the scale. Click to walk it.")
                onClicked: root.setParam(root.idRoot, (root.root + 1) % 12)
            }
        }

        // --- lane rail + skyline/grid --------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Skin.spacingXS

            // Eight lanes, always on screen in either view: mute, focus, and a
            // pair of pitch steppers for the pad's own note (unlocked cells play
            // this; locked ones carry their own pitch regardless).
            ColumnLayout {
                // A nested Layout defaults Layout.fillWidth to true, unlike a
                // plain Item - without this it competes for width with the
                // skyline/grid pane next to it instead of staying a rail.
                Layout.fillWidth: false
                Layout.preferredWidth: Px.px(112)
                Layout.fillHeight: true
                spacing: Px.px(2)

                Repeater {
                    model: root.laneCount

                    Rectangle {
                        id: laneRow
                        required property int index

                        readonly property bool focused: root.focusedLane === laneRow.index
                        readonly property bool muted: root.laneMuted(laneRow.index)

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: Skin.radiusS
                        color: laneRow.focused ? Skin.slotHover : Skin.slot
                        border.width: laneRow.focused ? 1 : 0
                        border.color: Skin.accent

                        TapHandler {
                            onTapped: {
                                root.setParam(root.idFocusedLane, laneRow.index)
                                root.readAll()
                            }
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: Px.px(3)
                            spacing: Px.px(2)

                            StripButton {
                                Layout.preferredWidth: Px.px(20)
                                Layout.preferredHeight: Px.px(20)
                                flat: true
                                label: "M"
                                active: laneRow.muted
                                activeColor: Skin.mute
                                tip: qsTr("Mute lane %1. Its head keeps walking - only the sound stops.")
                                    .arg(laneRow.index + 1)
                                onClicked: {
                                    Mixer.setSequencerLane(
                                        root.targetRow, root.targetSlot, laneRow.index,
                                        root.laneNote(laneRow.index),
                                        root.laneLength(laneRow.index),
                                        root.laneDivision(laneRow.index),
                                        root.laneDirection(laneRow.index),
                                        root.laneChannel(laneRow.index),
                                        !laneRow.muted,
                                        root.laneGate(laneRow.index))
                                    root.readAll()
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Text {
                                    text: qsTr("Lane %1").arg(laneRow.index + 1)
                                    color: laneRow.focused ? Skin.text : Skin.textDim
                                    font.pixelSize: Skin.fontXS
                                }
                                Text {
                                    text: root.noteName(root.laneNote(laneRow.index))
                                    color: laneRow.focused ? Skin.text : Skin.textDim
                                    font.pixelSize: Skin.fontXS
                                    font.bold: true
                                }
                            }

                            ColumnLayout {
                                spacing: 0
                                StripButton {
                                    Layout.preferredWidth: Px.px(16)
                                    Layout.preferredHeight: Px.px(12)
                                    flat: true
                                    label: "+"
                                    tip: qsTr("This lane's own pitch, up a semitone. What an unlocked step plays.")
                                    onClicked: {
                                        Mixer.setSequencerLane(
                                            root.targetRow, root.targetSlot, laneRow.index,
                                            Math.min(127, root.laneNote(laneRow.index) + 1),
                                            root.laneLength(laneRow.index),
                                            root.laneDivision(laneRow.index),
                                            root.laneDirection(laneRow.index),
                                            root.laneChannel(laneRow.index),
                                            laneRow.muted, root.laneGate(laneRow.index))
                                        root.readAll()
                                    }
                                }
                                StripButton {
                                    Layout.preferredWidth: Px.px(16)
                                    Layout.preferredHeight: Px.px(12)
                                    flat: true
                                    label: "−"
                                    tip: qsTr("This lane's own pitch, down a semitone.")
                                    onClicked: {
                                        Mixer.setSequencerLane(
                                            root.targetRow, root.targetSlot, laneRow.index,
                                            Math.max(0, root.laneNote(laneRow.index) - 1),
                                            root.laneLength(laneRow.index),
                                            root.laneDivision(laneRow.index),
                                            root.laneDirection(laneRow.index),
                                            root.laneChannel(laneRow.index),
                                            laneRow.muted, root.laneGate(laneRow.index))
                                        root.readAll()
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // --- skyline: one lane, pitch as a bar ----------------------------------
            Rectangle {
                visible: root.viewMode === 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Skin.slotEmpty
                radius: Skin.radius
                border.width: 1
                border.color: Skin.border
                clip: true

                Row {
                    id: skylineColumns
                    anchors.fill: parent
                    anchors.margins: Skin.spacingXS
                    spacing: Px.px(2)

                    Repeater {
                        model: root.pageSteps

                        Item {
                            id: column
                            required property int index

                            readonly property int lane: root.focusedLane
                            readonly property int step: root.stepOffset + column.index
                            readonly property bool on: root.cellOn(column.lane, column.step)
                            readonly property bool inPattern: column.step < root.focusedLength
                            readonly property bool atPlayhead:
                                root.playhead >= 0 && root.playhead === column.step
                            readonly property bool extraHere:
                                root.extraHeadOnStep(column.lane, column.step)
                            readonly property real note: root.cellNote(column.lane, column.step)
                            readonly property real velocity: root.cellVel(column.lane, column.step)
                            readonly property real chance: root.cellChance(column.lane, column.step)
                            readonly property bool accented: root.cellAccent(column.lane, column.step)
                            readonly property bool tied: root.cellTie(column.lane, column.step)

                            width: (skylineColumns.width - (root.pageSteps - 1) * Px.px(2))
                                   / root.pageSteps
                            height: skylineColumns.height
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
                            Rectangle {
                                anchors.fill: parent
                                radius: Skin.radiusS
                                color: Skin.solo
                                opacity: column.extraHere ? 0.18 : 0
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
                                    onTapped: root.setAccent(column.lane, column.step,
                                                             !column.accented)
                                }
                            }

                            Rectangle {
                                id: bar
                                readonly property real fraction:
                                    Math.min(1, Math.max(0,
                                        (root.heardNote(column.note) - root.lowNote) /
                                        (root.highNote - root.lowNote)))

                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.margins: Px.px(3)
                                anchors.top: accentTick.bottom
                                anchors.topMargin: Px.px(2)
                                anchors.bottom: chanceStrip.top
                                anchors.bottomMargin: Px.px(2)
                                radius: Skin.radiusS
                                color: Skin.slot
                                opacity: 0.35

                                Rectangle {
                                    id: pitchFill
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    height: Math.max(Px.px(4), bar.fraction * parent.height)
                                    radius: Skin.radiusS
                                    color: column.on ? Skin.accent : Skin.slotHover
                                    opacity: column.on
                                             ? 0.55 + 0.45 * (column.velocity / 127)
                                             : 0.5

                                    Behavior on color {
                                        ColorAnimation { duration: Skin.fast }
                                    }
                                    Behavior on height {
                                        NumberAnimation { duration: Skin.fast }
                                    }
                                }
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.bottom: pitchFill.top
                                anchors.bottomMargin: Px.px(2)
                                visible: column.on && column.width > Px.px(22)
                                text: root.noteName(root.heardNote(column.note))
                                color: Skin.text
                                font.pixelSize: Skin.fontXS
                            }

                            // Tie: the notch on the trailing edge. On, this step
                            // holds into the next instead of retriggering.
                            Rectangle {
                                visible: column.step < root.focusedLength - 1
                                anchors.right: parent.right
                                anchors.verticalCenter: bar.verticalCenter
                                width: Px.px(5)
                                height: Px.px(10)
                                radius: 1
                                color: column.tied ? Skin.meterLow : Skin.border
                                opacity: column.on ? 1 : 0.35

                                TapHandler {
                                    onTapped: root.setTie(column.lane, column.step,
                                                          !column.tied)
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
                                        root.setChance(column.lane, column.step, value)
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
                                        root.setVelocity(column.lane, column.step, value)
                                    }
                                    onPressed: mouse => positionChanged(mouse)
                                }
                            }

                            // A plain click: off becomes on, placed exactly where
                            // the click landed. On becomes off, same as always -
                            // a real drag never reaches this, so painting a run
                            // of steps below never fights with it.
                            TapHandler {
                                onSingleTapped: eventPoint => {
                                    if (column.on) {
                                        root.setOn(column.lane, column.step, false)
                                    } else {
                                        root.armNote(column.lane, column.step,
                                            root.pitchFromY(eventPoint.position.y,
                                                            column.height))
                                    }
                                }
                            }

                            // A real drag: the note tracks the pointer's height
                            // directly while it stays over this step, and the
                            // moment it crosses into a neighbour that step - and
                            // every one between here and there - gets stamped
                            // with whatever note this one last held, armed
                            // whether or not it already was.
                            DragHandler {
                                id: pitchDrag
                                target: null
                                xAxis.enabled: true
                                yAxis.enabled: true

                                property int paintNote: 60

                                function updatePaint() {
                                    const cellStep = column.width + Px.px(2)
                                    const globalX = column.x + pitchDrag.centroid.position.x
                                    const targetIndex = Math.max(0, Math.min(
                                        root.pageSteps - 1, Math.floor(globalX / cellStep)))
                                    if (targetIndex === column.index)
                                        pitchDrag.paintNote = root.pitchFromY(
                                            pitchDrag.centroid.position.y, column.height)
                                    root.paintNotes(column.lane, column.step,
                                        root.stepOffset + targetIndex, pitchDrag.paintNote)
                                }

                                onActiveChanged: if (pitchDrag.active) pitchDrag.updatePaint()
                                onCentroidChanged: if (pitchDrag.active) pitchDrag.updatePaint()
                            }
                        }
                    }
                }
            }

            // --- grid: eight lanes as pads -------------------------------------------
            Rectangle {
                visible: root.viewMode === 1
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Skin.slotEmpty
                radius: Skin.radius
                border.width: 1
                border.color: Skin.border
                clip: true

                Column {
                    id: gridRows
                    anchors.fill: parent
                    anchors.margins: Skin.spacingXS
                    spacing: Px.px(2)

                    Repeater {
                        model: root.laneCount

                        Row {
                            id: gridRow
                            required property int index

                            width: gridRows.width
                            height: (gridRows.height - (root.laneCount - 1) * Px.px(2))
                                    / root.laneCount
                            spacing: Px.px(2)

                            Repeater {
                                model: root.pageSteps

                                Rectangle {
                                    id: cell
                                    required property int index

                                    readonly property int lane: gridRow.index
                                    readonly property int step: root.stepOffset + cell.index
                                    readonly property bool on: root.cellOn(cell.lane, cell.step)
                                    readonly property bool inPattern:
                                        cell.step < root.laneLength(cell.lane)
                                    readonly property bool atPlayhead:
                                        root.nativeHeadStep(cell.lane) === cell.step
                                    readonly property bool extraHere:
                                        root.extraHeadOnStep(cell.lane, cell.step)
                                    readonly property real velocity: root.cellVel(cell.lane, cell.step)
                                    readonly property bool accented: root.cellAccent(cell.lane, cell.step)

                                    width: (gridRow.width - (root.pageSteps - 1) * Px.px(2))
                                           / root.pageSteps
                                    height: gridRow.height
                                    radius: Skin.radiusS
                                    opacity: cell.inPattern ? 1.0 : 0.3
                                    color: cell.on
                                        ? Skin.accent
                                        : (cell.index % 4 === 0 ? Skin.strip : Skin.slot)
                                    border.width: cell.lane === root.focusedLane ? 1 : 0
                                    border.color: Skin.accent

                                    Rectangle {
                                        anchors.fill: parent
                                        radius: Skin.radiusS
                                        color: Skin.text
                                        opacity: parent.on
                                            ? 0.15 + 0.35 * (parent.velocity / 127) : 0
                                    }

                                    Rectangle {
                                        anchors.fill: parent
                                        radius: Skin.radiusS
                                        color: Skin.solo
                                        opacity: parent.atPlayhead ? 0.35 : (parent.extraHere ? 0.2 : 0)
                                        Behavior on opacity {
                                            NumberAnimation { duration: Skin.fast }
                                        }
                                    }

                                    Rectangle {
                                        visible: cell.accented
                                        anchors.top: parent.top
                                        anchors.right: parent.right
                                        anchors.margins: Px.px(2)
                                        width: Px.px(5)
                                        height: Px.px(5)
                                        radius: Px.px(2.5)
                                        color: Skin.solo
                                    }

                                    TapHandler {
                                        onTapped: root.setOn(cell.lane, cell.step, !cell.on)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // --- page nav, when a lane or the grid runs past sixteen -------------------
        RowLayout {
            Layout.fillWidth: true
            visible: root.pageCount > 1
            spacing: Skin.spacingXS

            StripButton {
                Layout.preferredWidth: Px.px(28)
                label: "‹"
                enabled: root.page > 0
                onClicked: root.page = Math.max(0, root.page - 1)
            }
            Text {
                text: qsTr("page %1/%2 · steps %3–%4").arg(root.page + 1).arg(root.pageCount)
                    .arg(root.stepOffset + 1).arg(root.stepOffset + root.pageSteps)
                color: Skin.textDim
                font.pixelSize: Skin.fontXS
            }
            StripButton {
                Layout.preferredWidth: Px.px(28)
                label: "›"
                enabled: root.page < root.pageCount - 1
                onClicked: root.page = Math.min(root.pageCount - 1, root.page + 1)
            }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingXS
            visible: root.cellOn(root.focusedLane, root.editStep)

            Text {
                text: qsTr("step %1").arg(root.editStep + 1)
                color: Skin.textDim
                font.pixelSize: Skin.fontXS
            }
            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(18)
                label: qsTr("ratchet")
                valueText: Math.max(1, Math.round(root.cellRatchet(root.focusedLane, root.editStep)))
                value: (Math.max(1, root.cellRatchet(root.focusedLane, root.editStep)) - 1) / 7
                step: 1 / 7
                fineStep: 1 / 7
                tip: qsTr("Hits inside this step. Needs the Ratchet macro above zero.")
                onMoved: v => {
                    const i = root.cellIndex(root.focusedLane, root.editStep)
                    root.ratchet = root.setField(root.ratchet, i, 1 + Math.round(v * 7))
                    root.sendTrig(root.focusedLane, root.editStep)
                }
            }
            Repeater {
                model: [qsTr("always"), qsTr("fill"), qsTr("!fill"), qsTr("pre"),
                        qsTr("!pre"), qsTr("nei"), qsTr("1:2")]
                StripButton {
                    required property int index
                    required property string modelData
                    Layout.preferredWidth: Px.px(40)
                    Layout.preferredHeight: Px.px(18)
                    flat: true
                    label: modelData
                    active: root.cellCond(root.focusedLane, root.editStep) === index
                    onClicked: {
                        const i = root.cellIndex(root.focusedLane, root.editStep)
                        root.cond = root.setField(root.cond, i, index)
                        root.condArg = root.setField(root.condArg, i,
                            index === 6 ? ((1 << 4) | 2) : 0)
                        root.sendTrig(root.focusedLane, root.editStep)
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            text: root.viewMode === 0
                ? qsTr("Tap a step to arm it right where you clicked · drag sideways to paint that note across the steps you cross · gold tick is accent · green notch is a tie · yellow strip is chance")
                : qsTr("Tap a pad to arm it at the lane's own note · brightness is velocity · gold dot is accent · tap a lane on the left to focus it, M to mute")
            color: Skin.textDim
            font.pixelSize: Skin.fontXS
            wrapMode: Text.WordWrap
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: Skin.spacing
            rowSpacing: Skin.spacingXS

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(22)
                label: qsTr("steps")
                valueText: root.focusedLength
                value: (root.focusedLength - 1) / (root.maxSteps - 1)
                step: 1 / (root.maxSteps - 1)
                fineStep: 1 / (root.maxSteps - 1)
                tip: qsTr("How many of this lane's steps play before it loops. Lanes can differ - that is polymeter.")
                onMoved: v => root.setParam(root.idLength, Math.round(1 + v * (root.maxSteps - 1)))
            }

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(22)
                label: qsTr("division")
                valueText: ["1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T"][
                    root.laneDivision(root.focusedLane)]
                value: root.laneDivision(root.focusedLane) / 5
                step: 1 / 5
                fineStep: 1 / 5
                onMoved: v => root.setParam(root.idDivision, Math.round(v * 5))
            }

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(22)
                label: qsTr("channel")
                valueText: root.laneChannel(root.focusedLane) + 1
                value: root.laneChannel(root.focusedLane) / 15
                step: 1 / 15
                fineStep: 1 / 15
                tip: qsTr("MIDI channel this lane sends on.")
                onMoved: v => root.setParam(root.idChannel, Math.round(v * 15))
            }

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(22)
                label: qsTr("swing")
                valueText: Math.round(root.swing * 100) + "%"
                value: root.swing
                tip: qsTr("Off-beats lean late. 0 is straight. Global.")
                onMoved: v => root.setParam(root.idSwing, v)
            }

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(22)
                label: qsTr("gate")
                valueText: Math.round(root.laneGate(root.focusedLane) * 100) + "%"
                value: (root.laneGate(root.focusedLane) - 0.05) / 0.95
                onMoved: v => root.setParam(root.idGate, 0.05 + v * 0.95)
            }

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(22)
                label: qsTr("transpose")
                valueText: (root.transpose > 0 ? "+" : "") + Math.round(root.transpose)
                value: (root.transpose + 24) / 48
                step: 1 / 48
                fineStep: 1 / 48
                tip: qsTr("Global, on top of every lane.")
                onMoved: v => root.setParam(root.idTranspose, Math.round(v * 48 - 24))
            }

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(22)
                label: qsTr("euclid")
                valueText: root.laneEuclid(root.focusedLane)
                value: root.laneEuclid(root.focusedLane) / root.maxSteps
                step: 1 / root.maxSteps
                fineStep: 1 / root.maxSteps
                tip: qsTr("Spread this many hits evenly across the focused lane.")
                onMoved: v => {
                    root.setParam(root.idEuclid, Math.round(v * root.maxSteps))
                    root.readAll()
                }
            }
        }
    }
}
