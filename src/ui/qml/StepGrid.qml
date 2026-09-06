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
//
// It reads the way the looper's and the drone's editors do. The window glows
// in the colour of what the sequencer is doing - red while it records, yellow
// under Fill, green while it runs - and a chip in the header says so in a
// word, next to the focused lane's own bar.beat and the name of the chip the
// notes actually reach. The grid sits in an inset panel with a row of step
// lamps under it that shows the whole lane, pages and all, and flares on the
// one. The four macros a performer rides - Density, Chaos, Probability,
// Ratchet - are bars on the right, Swing and Transpose smaller ones below
// them, and MAP binds any of them to a knob on this strip's MIDI input.
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

    // Everything MAP can bind. Read once on open and again when a learn
    // lands, so the rings on the controls are never a poll behind.
    readonly property var mappableIds: [
        root.idDivision, root.idLength, root.idGate, root.idTranspose,
        root.idSwing, root.idEuclid, root.idRecordArm, root.idDensity,
        root.idChaos, root.idRatchetAmount, root.idMasterProb,
        root.idPattern, root.idFill
    ]

    readonly property int laneCount: 8
    readonly property int maxSteps: 64
    readonly property int pageSteps: 16
    readonly property int patternCount: 16

    // Skyline's vertical window, same 48-semitone span as before. Moving
    // a pitch in Grid recenters this so that note sits in the middle.
    property int lowNote: 36
    property int highNote: 84
    readonly property int skylineSpan: 48

    readonly property var scaleNames: [
        qsTr("chrom"), qsTr("maj"), qsTr("min"), qsTr("dor"),
        qsTr("mix"), qsTr("p−"), qsTr("p+"), qsTr("blues")
    ]
    readonly property var rootNames: [
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    ]
    readonly property var divisionNames: ["1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T"]
    // Steps in one beat at each division, for the lamps and the clock.
    readonly property var divisionStepsPerBeat: [1, 2, 4, 8, 3, 6]

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
    // A pattern queued to take over at the bar, -1 for none.
    property int nextPattern: -1
    property bool fill: false
    property bool recording: false
    property int focusedLane: 0
    property int viewMode: 1  // grid first; skyline is the bassline view
    // Which page of 16 steps is on screen. QML-only - a restart always comes
    // back to page 0, and two editors open on the same insert can look at
    // different pages without fighting over one number in the blob.
    property int page: 0
    // How many steps are on in this pattern, across every lane. Zero is an
    // empty pattern, and the chip says so.
    property int hitCount: 0

    property int transpose: 0
    property real swing: 0
    property int scaleId: 0
    property int root: 0
    property int playhead: -1
    // The insert immediately below this sequencer: who the MIDI actually
    // hits. Empty name means nothing sits there. Pads are that chip's own
    // named keys, not General MIDI.
    property string targetName: ""
    property var targetPads: []
    // Pitches the grid can show, low to high: the chip's pads, the eight
    // lane notes, anything Skyline has painted, and (with no chip map) the
    // same C2–C6 span Skyline draws. Grid and Skyline share this list.
    property var rowPitches: []
    // View offset into rowPitches. QML-only — scrolling does not rewrite
    // lane notes; a kick pattern stays on the kick, and may leave the screen.
    property int padWindow: 0
    // Which pitch row is selected in Grid. A voice can own several pitches
    // (the pad plus Skyline locks); only one row is the selection, not all
    // of them. QML-only — the engine still focuses a lane.
    property int focusedPitch: -1
    readonly property int unlockedNote: 255
    // Set once, the first time this ever opens - after that the popup stays
    // wherever it was last dragged, the same as a real tool window would.
    property bool positioned: false

    // MAP: press it, tap a control, turn a knob. `waitingParam` is the one
    // tapped and not yet bound; `mapped` says which already have a knob.
    property bool mapping: false
    property int waitingParam: -1
    property var mapped: ({})
    // Lit for a moment when the focused lane comes round to its one, and
    // when Rec goes down or up: the panel's border and a wash over it flare
    // in the state's colour, and the glow under the window breathes with it.
    property real flash: 0
    property int lastHeadStep: -1

    readonly property int focusedLength: root.laneLength(root.focusedLane)
    readonly property bool padView: root.kitView
    readonly property int padWindowMax: Math.max(0, root.rowPitches.length - root.laneCount)
    readonly property int pageCount: Math.max(1, Math.ceil(
        (root.viewMode === 1 ? root.maxSteps : root.focusedLength) / root.pageSteps))
    readonly property int stepOffset: Math.min(root.page, root.pageCount - 1) * root.pageSteps

    // The transport is rolling through this sequencer.
    readonly property bool playing: root.playhead >= 0
    // Where the focused lane's own head is, or -1.
    readonly property int headStep: root.nativeHeadStep(root.focusedLane)
    readonly property int stepsPerBeat:
        root.divisionStepsPerBeat[root.laneDivision(root.focusedLane)] ?? 4
    // The focused lane's bar.beat, the way the transport shows the song's
    // and the looper shows the loop's. A lane in 7 reads 2.3 where the song
    // reads 4.1 - that is the polymeter, made visible.
    readonly property string laneClock: {
        if (root.headStep < 0) return ""
        const num = Math.max(1, Mixer.timeNumerator())
        const beat = Math.floor(root.headStep / Math.max(1, root.stepsPerBeat))
        return (Math.floor(beat / num) + 1) + "." + (beat % num + 1)
    }

    // What the sequencer is doing, in a word, and in a colour. The colour is
    // the one the glow, the chip, the panel's border and the lamps agree on.
    // Red is being written, yellow is Fill, green is the cycle going round.
    readonly property string stateLabel:
        root.recording ? qsTr("RECORDING")
        : root.fill ? qsTr("FILL")
        : root.hitCount === 0 ? qsTr("EMPTY")
        : root.playing ? qsTr("PLAYING")
        : qsTr("STOPPED")
    readonly property color stateHue:
        root.recording ? Skin.arm
        : root.fill ? Skin.solo
        : root.hitCount === 0 ? Skin.accent
        : root.playing ? Skin.meterLow
        : Skin.textDim
    readonly property bool stateBusy: root.recording || root.fill

    // The macros and the weather, each in a colour of its own, kept clear of
    // what colour already means in here: Rec's red, Fill's and the accent
    // pip's yellow, the hits' accent blue, the playhead's green.
    readonly property color hueDensity: Qt.hsla(0.47, 0.50, 0.58, 1.0)
    readonly property color hueChaos: Qt.hsla(0.83, 0.55, 0.66, 1.0)
    readonly property color hueProb: Qt.hsla(0.58, 0.50, 0.64, 1.0)
    readonly property color hueRatchet: Qt.hsla(0.08, 0.70, 0.62, 1.0)
    readonly property color hueSwing: Qt.hsla(0.72, 0.50, 0.66, 1.0)
    readonly property color hueTranspose: Qt.hsla(0.30, 0.45, 0.60, 1.0)

    // Capped to the overlay so it cannot paint off the mixer, but allowed to
    // use the room a normal 1200×700 window has.
    width: {
        const ov = Overlay.overlay
        const want = Px.px(900)
        if (!ov) return want
        return Math.min(want, Math.max(Px.px(560), ov.width - Px.px(24)))
    }
    height: {
        const ov = Overlay.overlay
        const want = Px.px(640)
        if (!ov) return want
        return Math.min(want, Math.max(Px.px(420), ov.height - Px.px(24)))
    }
    // A row that outgrows this width should look cramped, not spill buttons
    // out past the panel and over the mixer behind it.
    clip: true
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
        clip: true

        // The glow: light under a door, in the state's colour. While the
        // pattern runs it breathes on the one, so the bar can be felt with
        // the popup half seen across a dark stage; recording and Fill keep
        // a floor so they are never missed.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: parent.height * 0.4
            radius: parent.radius
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: root.stateHue }
            }
            opacity: root.stateBusy ? 0.18 + 0.10 * root.flash
                   : root.playing && root.hitCount > 0 ? 0.08 + 0.16 * root.flash
                   : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

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

    onClosed: {
        root.playhead = -1
        root.lastHeadStep = -1
        root.stopMapping()
    }

    // --- MIDI learn ----------------------------------------------------------------
    function isMapped(id) { return root.mapped[id] === true }

    function refreshMapped() {
        const next = {}
        for (const id of root.mappableIds)
            next[id] = Mixer.insertParamMapped(root.targetRow, root.targetSlot, id)
        root.mapped = next
    }

    function armParam(id, min, max) {
        Mixer.learnInsertParam(root.targetRow, root.targetSlot, id, min, max)
        root.waitingParam = id
    }

    function stopMapping() {
        Mixer.cancelLearn()
        root.mapping = false
        root.waitingParam = -1
    }

    Connections {
        target: Mixer
        function onLearnChanged() {
            if (!Mixer.learning) {
                root.waitingParam = -1
                root.refreshMapped()
            }
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: root.mapping || Mixer.learning
        onActivated: root.stopMapping()
    }

    // The flare: on in an instant, gone in a beat's worth of frames.
    function flare() { flareAnimation.restart() }
    NumberAnimation {
        id: flareAnimation
        target: root
        property: "flash"
        from: 1
        to: 0
        duration: 320
        easing.type: Easing.OutQuad
    }

    // A ring in the corner of anything bound to a knob - the same ring the
    // bars draw for themselves.
    component MapDot: Rectangle {
        required property int param
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Skin.spacingXS
        width: Px.px(8)
        height: Px.px(8)
        radius: width / 2
        z: 2
        visible: root.isMapped(param)
        color: "transparent"
        border.width: Px.px(2)
        border.color: root.waitingParam === param ? Skin.solo : Skin.focus
    }

    // The header's buttons: a little taller than the mixer's, the way the
    // looper's and the drone's are, so the row reads as a transport.
    component HeaderButton: StripButton {
        Layout.preferredHeight: Skin.buttonHeight + Px.px(6)
    }

    // A small label before a row of choices, in the bars' own lettering.
    component RowLabel: Text {
        color: Skin.textDim
        font.pixelSize: Skin.fontXS
        font.bold: true
        font.letterSpacing: Px.px(1)
    }

    // A row's worth of buttons: narrow, never wider than they need.
    component RowButton: StripButton {
        Layout.minimumWidth: 0
        Layout.preferredHeight: Skin.buttonHeight
    }


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

    // What that step actually plays: a locked Skyline pitch, or the lane's pad.
    function soundingNote(lane, step) {
        const locked = Math.round(root.cellNote(lane, step))
        if (locked === root.unlockedNote) return root.laneNote(lane)
        return locked
    }
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
        root.rebuildRowPitches()
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

    function sameFields(a, b) {
        if (a.length !== b.length) return false
        for (let i = 0; i < a.length; ++i) {
            if (a[i] !== b[i]) return false
        }
        return true
    }

    // pollOnly is for the 50ms playhead timer: it already pays for this
    // snapshot every tick, but the target-chain query and the row-pitch
    // scan don't need redoing unless what feeds them actually moved.
    function readAll(pollOnly) {
        const snap = Mixer.insertSequencerSnapshot(root.targetRow, root.targetSlot)
        const prevNote = root.note
        const prevLanes = root.lanes
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
        root.nextPattern = snap.nextPattern ?? -1
        root.fill = snap.fill === true || snap.fill === 1
        let hits = 0
        for (let i = 0; i < root.on.length; ++i)
            if ((root.on[i] ?? 0) !== 0) ++hits
        root.hitCount = hits
        root.recording = snap.recording === true || snap.recording === 1
        root.focusedLane = snap.focusedLane ?? 0
        root.viewMode = snap.view ?? 1
        root.transpose = snap.transpose ?? 0
        root.swing = snap.swing ?? 0
        root.scaleId = snap.scale ?? 0
        root.root = snap.root ?? 0
        if (pollOnly === true) {
            // The chain under the sequencer moves without this window being
            // told: the instrument below gets removed from the strip, or a
            // kit loads into it and names its pads. The header chip kept
            // saying "→ Odin" after Odin was gone. Asking each tick is one
            // short walk of the strip; assigning only on a change keeps the
            // header and the grid from being rebuilt fifty times a second.
            const targetMoved = root.readTarget()
            if (targetMoved || !root.sameFields(prevNote, root.note)
                    || !root.sameFields(prevLanes, root.lanes))
                root.rebuildRowPitches()
            return
        }
        root.readTarget()
        root.rebuildRowPitches()
    }

    // Who sits below, and whether that differs from what is on screen.
    function readTarget() {
        const t = Mixer.insertSequencerTarget(root.targetRow, root.targetSlot)
        const name = t.name || ""
        const pads = t.pads || []
        let same = name === root.targetName
                   && pads.length === root.targetPads.length
        for (let i = 0; same && i < pads.length; ++i)
            same = pads[i].note === root.targetPads[i].note
                   && pads[i].name === root.targetPads[i].name
        if (same) return false
        root.targetName = name
        root.targetPads = pads
        return true
    }

    function openFor(row, slot) {
        root.targetRow = row
        root.targetSlot = slot
        root.pluginName = Mixer.insertName(row, slot)
        root.padWindow = 0
        root.lastHeadStep = -1
        root.readAll()
        root.refreshMapped()
        root.revealFocusedVoice()
        root.page = 0
        if (!root.positioned) {
            root.x = Math.round((Overlay.overlay.width - root.width) / 2)
            root.y = Math.round((Overlay.overlay.height - root.height) / 2)
            root.positioned = true
        }
        root.clampPos()
        root.open()
    }

    function clampPos() {
        const ov = Overlay.overlay
        if (!ov) return
        root.x = Math.max(0, Math.min(root.x, ov.width - root.width))
        root.y = Math.max(0, Math.min(root.y, ov.height - root.height))
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

    // The next chip's own name for this key, or the note. Never General MIDI:
    // MIDI 39 is only a clap if that sampler said so.
    function padName(midi) {
        const n = Math.round(midi)
        const pads = root.targetPads
        for (let i = 0; i < pads.length; ++i) {
            if (pads[i].note === n) return pads[i].name
        }
        return root.noteName(n)
    }

    function setLanePitch(lane, note) {
        Mixer.setSequencerLane(
            root.targetRow, root.targetSlot, lane,
            Math.max(0, Math.min(127, Math.round(note))),
            root.laneLength(lane),
            root.laneDivision(lane),
            root.laneDirection(lane),
            root.laneChannel(lane),
            root.laneMuted(lane),
            root.laneGate(lane))
        root.readAll()
    }

    // Walk the chip's pad list when it has one, otherwise a semitone.
    function nextPadNote(current, dir) {
        const pads = root.targetPads
        if (!pads || pads.length === 0)
            return Math.max(0, Math.min(127, current + dir))
        if (dir > 0) {
            for (let i = 0; i < pads.length; ++i) {
                if (pads[i].note > current) return pads[i].note
            }
            return pads[pads.length - 1].note
        }
        for (let i = pads.length - 1; i >= 0; --i) {
            if (pads[i].note < current) return pads[i].note
        }
        return pads[0].note
    }

    function bumpLaneNote(lane, dir) {
        root.setLanePitch(lane, root.nextPadNote(root.laneNote(lane), dir))
    }

    function nearestPad(want) {
        const pads = root.targetPads
        if (!pads || pads.length === 0)
            return Math.max(0, Math.min(127, want))
        let best = pads[0].note
        let bestD = 999
        for (let i = 0; i < pads.length; ++i) {
            const d = Math.abs(pads[i].note - want)
            if (d < bestD) {
                bestD = d
                best = pads[i].note
            }
        }
        return best
    }

    function shiftOctave(lane, dir) {
        const want = root.laneNote(lane) + dir * 12
        root.setLanePitch(lane, root.nearestPad(want))
    }

    function shiftAllOctaves(dir) {
        for (let i = 0; i < root.laneCount; ++i) {
            Mixer.setSequencerLane(
                root.targetRow, root.targetSlot, i,
                root.nearestPad(root.laneNote(i) + dir * 12),
                root.laneLength(i),
                root.laneDivision(i),
                root.laneDirection(i),
                root.laneChannel(i),
                root.laneMuted(i),
                root.laneGate(i))
        }
        root.readAll()
    }

    function rowPitch(row) {
        const n = root.rowPitches[root.padWindow + row]
        return n === undefined ? -1 : n
    }

    function visiblePad(row) {
        const n = root.rowPitch(row)
        if (n < 0) return null
        return { note: n, name: root.padName(n) }
    }

    function laneForPad(midi) {
        const n = Math.round(midi)
        for (let i = 0; i < root.laneCount; ++i) {
            if (root.laneNote(i) === n) return i
        }
        return -1
    }

    function padInKit(midi) {
        const n = Math.round(midi)
        for (let i = 0; i < root.targetPads.length; ++i) {
            if (root.targetPads[i].note === n) return true
        }
        return false
    }

    function laneSoundingAt(midi, step) {
        const n = Math.round(midi)
        for (let l = 0; l < root.laneCount; ++l) {
            if (!root.cellOn(l, step)) continue
            if (root.soundingNote(l, step) === n) return l
        }
        return -1
    }

    function laneForPitch(midi) {
        const n = Math.round(midi)
        let lane = root.laneForPad(n)
        if (lane >= 0) return lane
        for (let l = 0; l < root.laneCount; ++l) {
            const len = root.laneLength(l)
            for (let s = 0; s < len; ++s) {
                if (root.cellOn(l, s) && root.soundingNote(l, s) === n)
                    return l
            }
        }
        return -1
    }

    function pitchOn(midi, step) {
        return root.laneSoundingAt(midi, step) >= 0
    }

    function laneForRow(row) {
        const n = root.rowPitch(row)
        if (n < 0) return -1
        return root.laneForPitch(n)
    }

    // Retuning a lane changes every unlocked hit already on it, so a lane
    // with a pattern of its own keeps it even when its current pitch isn't
    // a kit pad — only a lane with nothing unlocked programmed is "free".
    function laneHasUnlockedHits(lane) {
        for (let s = 0; s < root.maxSteps; ++s) {
            if (root.cellOn(lane, s) && Math.round(root.cellNote(lane, s)) === root.unlockedNote)
                return true
        }
        return false
    }

    // A kit pad with no owner gets a free voice. A Skyline pitch that the
    // chip does not name (or any pitch when there is no chip) stays on the
    // focused lane as a locked step — never retunes the whole pad.
    function claimLaneForPad(midi) {
        let lane = root.laneForPad(midi)
        if (lane >= 0) return lane
        if (!root.padInKit(midi))
            return root.focusedLane
        for (let i = 0; i < root.laneCount; ++i) {
            if (root.laneMuted(i) && !root.padInKit(root.laneNote(i)) &&
                !root.laneHasUnlockedHits(i)) {
                root.setLanePitch(i, midi)
                return i
            }
        }
        for (let i = 0; i < root.laneCount; ++i) {
            if (!root.padInKit(root.laneNote(i)) && !root.laneHasUnlockedHits(i)) {
                root.setLanePitch(i, midi)
                return i
            }
        }
        // Every lane is already carrying a pattern of its own: same fallback
        // as an out-of-kit pitch, land it on the focused lane as a locked
        // step rather than retuning — and silently moving — that lane's
        // whole pattern out from under whatever it was already playing.
        return root.focusedLane
    }

    function rebuildRowPitches() {
        const seen = {}
        const list = []
        const add = n => {
            n = Math.round(n)
            if (n < 0 || n > 127 || n === root.unlockedNote) return
            if (seen[n]) return
            seen[n] = true
            list.push(n)
        }
        for (let i = 0; i < root.targetPads.length; ++i)
            add(root.targetPads[i].note)
        for (let l = 0; l < root.laneCount; ++l)
            add(root.laneNote(l))
        if (root.note.length > 0) {
            for (let l = 0; l < root.laneCount; ++l) {
                for (let s = 0; s < root.maxSteps; ++s) {
                    const locked = Math.round(root.cellNote(l, s))
                    if (locked !== root.unlockedNote)
                        add(locked)
                    if (root.cellOn(l, s))
                        add(root.soundingNote(l, s))
                }
            }
        }
        // No chip map: the grid is the same span Skyline can paint.
        if (root.targetPads.length === 0) {
            for (let n = root.lowNote; n <= root.highNote; ++n)
                add(n)
        }
        list.sort((a, b) => a - b)
        const prev = root.rowPitches
        let same = prev.length === list.length
        if (same) {
            for (let i = 0; i < list.length; ++i) {
                if (prev[i] !== list[i]) { same = false; break }
            }
        }
        if (!same)
            root.rowPitches = list
        const maxWindow = Math.max(0, list.length - root.laneCount)
        if (root.padWindow > maxWindow)
            root.padWindow = maxWindow
    }

    function revealPitch(midi) {
        const n = Math.round(midi)
        const list = root.rowPitches
        let idx = -1
        for (let i = 0; i < list.length; ++i) {
            if (list[i] === n) { idx = i; break }
        }
        if (idx < 0) return
        if (idx < root.padWindow)
            root.padWindow = idx
        else if (idx >= root.padWindow + root.laneCount)
            root.padWindow = Math.max(0, idx - root.laneCount + 1)
    }

    function centerSkylineOn(midi) {
        const n = Math.max(0, Math.min(127, Math.round(midi)))
        const span = root.skylineSpan
        let lo = n - Math.floor(span / 2)
        let hi = lo + span
        if (lo < 0) {
            hi -= lo
            lo = 0
        }
        if (hi > 127) {
            lo -= hi - 127
            hi = 127
        }
        root.lowNote = Math.max(0, lo)
        root.highNote = Math.min(127, Math.max(root.lowNote + 1, hi))
    }

    function enterSkyline() {
        const pitch = root.focusedPitch >= 0
            ? root.focusedPitch : root.laneNote(root.focusedLane)
        let lane = root.laneForPitch(pitch)
        if (lane < 0) lane = root.focusedLane
        root.focusedPitch = pitch
        root.setParam(root.idView, 0)
        root.setParam(root.idFocusedLane, lane)
        root.centerSkylineOn(pitch)
        root.page = Math.floor(Math.max(0, root.editStep) / root.pageSteps)
        root.readAll()
    }

    function revealFocusedVoice() {
        const pad = root.laneNote(root.focusedLane)
        root.revealPitch(pad)
        if (root.cellOn(root.focusedLane, root.editStep)) {
            const painted = root.soundingNote(root.focusedLane, root.editStep)
            root.revealPitch(painted)
            root.focusedPitch = painted
        } else if (root.focusedPitch < 0) {
            root.focusedPitch = pad
        }
    }

    function rowIsSelected(pitch) {
        if (pitch < 0) return false
        if (root.focusedPitch >= 0) return pitch === root.focusedPitch
        return pitch === root.laneNote(root.focusedLane)
    }

    function focusRow(row) {
        const n = root.rowPitch(row)
        if (n < 0) return
        let lane = root.laneForPitch(n)
        if (lane < 0) lane = root.claimLaneForPad(n)
        root.focusedPitch = n
        root.setParam(root.idFocusedLane, lane)
        root.readAll()
    }

    function toggleRowStep(row, step) {
        const midi = root.rowPitch(row)
        if (midi < 0) return
        root.focusedPitch = midi
        const existing = root.laneSoundingAt(midi, step)
        if (existing >= 0) {
            for (let l = 0; l < root.laneCount; ++l) {
                if (root.cellOn(l, step) && root.soundingNote(l, step) === midi)
                    root.setOn(l, step, false)
            }
            root.rebuildRowPitches()
            return
        }
        const lane = root.claimLaneForPad(midi)
        const store = root.laneNote(lane) === midi ? root.unlockedNote : midi
        root.armNote(lane, step, store)
    }

    // Put this window of rows onto the eight lanes. Explicit — scrolling
    // the view does not do this; the groove stays on the pads that own it.
    // start indexes rowPitches (what the grid is showing), same as rowPitch().
    function pullPadWindow(start) {
        const rows = root.rowPitches
        if (!rows || rows.length === 0) return
        const maxW = Math.max(0, rows.length - root.laneCount)
        const from = Math.max(0, Math.min(start, maxW))
        let firstNote = -1
        for (let i = 0; i < root.laneCount; ++i) {
            const note = rows[from + i]
            if (note === undefined) break
            if (firstNote < 0) firstNote = note
            Mixer.setSequencerLane(
                root.targetRow, root.targetSlot, i, note,
                root.laneLength(i),
                root.laneDivision(i),
                root.laneDirection(i),
                root.laneChannel(i),
                root.laneMuted(i),
                root.laneGate(i))
        }
        root.readAll()
        if (firstNote >= 0)
            root.revealPitch(firstNote)
    }

    function scrollPadWindow(start) {
        root.padWindow = Math.max(0, Math.min(start, root.padWindowMax))
    }

    // The window moves. Hits stay on their pitch — a Skyline C5 is still
    // C5 after you scroll, it just may leave the eight visible rows.
    function nudgeVertical(dir) {
        root.scrollPadWindow(root.padWindow + dir)
    }

    readonly property bool kitView: root.viewMode === 1

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
            // timer already paid for that elsewhere. pollOnly skips redoing
            // the target-chain query and row-pitch scan when nothing moved.
            root.readAll(true)
            // The focused lane came round to its one: that was the bar.
            const head = root.headStep
            if (head === 0 && root.lastHeadStep !== 0 && root.lastHeadStep >= 0) root.flare()
            root.lastHeadStep = head
        }
    }

    Connections {
        target: Overlay.overlay
        function onWidthChanged() { root.clampPos() }
        function onHeightChanged() { root.clampPos() }
    }

    contentItem: ColumnLayout {
        spacing: Skin.spacing
        clip: true

        // --- header ------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingS

            Text {
                text: qsTr("SEQUENCER")
                color: Skin.text
                font.pixelSize: Skin.fontL
                font.bold: true
                font.letterSpacing: Px.px(2)
            }

            Item { Layout.preferredWidth: Skin.spacing }

            // The state, on a chip: a dot in the state's colour, the word for
            // it, the focused lane's own bar.beat, which pattern is playing
            // and which is queued, and the name of the chip the notes reach
            // - or a warning that nothing does.
            Rectangle {
                id: stageChip
                Layout.preferredWidth: Px.px(340)
                Layout.preferredHeight: Skin.buttonHeight + Px.px(6)
                radius: Skin.radius
                color: Skin.slotEmpty
                border.width: 1
                border.color: Qt.rgba(root.stateHue.r, root.stateHue.g, root.stateHue.b, 0.6)
                property real pulse: 1

                SequentialAnimation on pulse {
                    running: root.stateBusy
                    loops: Animation.Infinite
                    NumberAnimation { from: 1; to: 0.35; duration: 420; easing.type: Easing.InOutSine }
                    NumberAnimation { from: 0.35; to: 1; duration: 420; easing.type: Easing.InOutSine }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Skin.spacing
                    anchors.rightMargin: Skin.spacing
                    spacing: Skin.spacingS

                    Rectangle {
                        Layout.preferredWidth: Px.px(10)
                        Layout.preferredHeight: Px.px(10)
                        radius: width / 2
                        color: root.stateHue
                        opacity: root.stateBusy ? stageChip.pulse
                               : root.playing ? 1 : 0.5
                    }

                    Text {
                        text: root.stateLabel
                        color: root.stateHue
                        font.pixelSize: Skin.fontL
                        font.bold: true
                        font.letterSpacing: Px.px(1)
                    }

                    Text {
                        visible: root.laneClock.length > 0
                        text: root.laneClock
                        color: Skin.text
                        font.pixelSize: Skin.fontL
                        font.bold: true
                        font.family: Skin.monoFamily
                    }

                    Text {
                        text: root.nextPattern >= 0 && root.nextPattern !== root.pattern
                              ? qsTr("P%1 → P%2").arg(root.pattern + 1).arg(root.nextPattern + 1)
                              : qsTr("P%1").arg(root.pattern + 1)
                        color: root.nextPattern >= 0 && root.nextPattern !== root.pattern
                               ? Skin.solo : Skin.textDim
                        font.pixelSize: Skin.fontS
                        font.bold: true
                        font.family: Skin.monoFamily
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.targetName.length > 0
                              ? "→ " + root.targetName
                              : qsTr("→ nothing below")
                        color: root.targetName.length > 0 ? Skin.textDim : Skin.solo
                        font.pixelSize: Skin.fontS
                        elide: Text.ElideRight
                    }
                }

                HoverHandler { id: stageHover }
                Tip {
                    text: qsTr("What the sequencer is doing, where the focused lane is as bar.beat, which pattern plays and which is queued, and the instrument the notes go to - the next insert down this strip.")
                    visible: stageHover.hovered
                }
            }

            Item { Layout.preferredWidth: Skin.spacing }

            HeaderButton {
                Layout.preferredWidth: Px.px(64)
                label: qsTr("REC")
                active: root.recording
                activeColor: Skin.arm
                tip: root.mapping
                     ? qsTr("Tap to bind Rec to the next control.")
                     : qsTr("Play your MIDI input and it lands on the nearest step, with its velocity and how long you held it, on the focused lane. The pattern itself goes quiet while this is on, so only what you play sounds.")
                onClicked: {
                    if (root.mapping) { root.armParam(root.idRecordArm, 0, 1); return }
                    root.setParam(root.idRecordArm, root.recording ? 0 : 1)
                    root.readAll()
                    root.flare()
                }
                MapDot { param: root.idRecordArm }
            }
            HeaderButton {
                Layout.preferredWidth: Px.px(64)
                label: qsTr("FILL")
                active: root.fill
                activeColor: Skin.solo
                tip: root.mapping
                     ? qsTr("Tap to bind Fill to the next control.")
                     : qsTr("Hold for the fill. Steps with the Fill condition only fire while this is on; NotFill does the opposite.")
                onClicked: {
                    if (root.mapping) { root.armParam(root.idFill, 0, 1); return }
                    root.setParam(root.idFill, root.fill ? 0 : 1)
                    root.readAll()
                }
                MapDot { param: root.idFill }
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

            HeaderButton {
                label: qsTr("MAP")
                active: root.mapping || Mixer.learning
                activeColor: Skin.solo
                tip: qsTr("Bind a control to a knob or pad on this strip's MIDI input. Press MAP, tap Rec, Fill, a macro, a lane setting… then turn the control. MAP stays on so the next one can follow.")
                onClicked: {
                    if (root.mapping || Mixer.learning) root.stopMapping()
                    else {
                        root.mapping = true
                        root.waitingParam = -1
                    }
                }
            }
        }

        // --- body -------------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Skin.spacingL

            // --- the grid, and the rows that shape the lane under it ----------------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Skin.spacingS

                Rectangle {
                    id: panel
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: Skin.radiusL
                    color: Qt.rgba(0, 0, 0, 0.25)
                    border.width: 1
                    border.color: Skin.line

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Skin.spacing
                        spacing: Skin.spacingS

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
                            Layout.preferredWidth: Px.px(96)
                            Layout.minimumWidth: Px.px(80)
                            Layout.fillHeight: true
                            spacing: Px.px(2)

                            StripButton {
                                visible: root.kitView
                                Layout.fillWidth: true
                                Layout.preferredHeight: Px.px(16)
                                Layout.maximumHeight: Px.px(16)
                                flat: true
                                label: "▲"
                                enabled: root.padWindow < root.padWindowMax
                                tip: qsTr("Show higher pitches. Hits stay where Skyline put them — a C5 does not become a C6.")
                                onClicked: root.nudgeVertical(1)
                            }

                            Repeater {
                                model: root.laneCount

                                Rectangle {
                                    id: laneRow
                                    required property int index

                                    readonly property var pad: root.visiblePad(laneRow.index)
                                    // Skyline is eight voices in order. Grid is a pitch
                                    // window — mute/focus must follow the pad on that row,
                                    // not lane-index-as-row, or Lane 1 mutes the kick.
                                    readonly property int lane: root.kitView
                                        ? root.laneForRow(laneRow.index) : laneRow.index
                                    readonly property bool assigned: laneRow.lane >= 0
                                    readonly property bool focused: root.kitView
                                        ? (laneRow.pad !== null
                                           && root.rowIsSelected(laneRow.pad.note))
                                        : root.focusedLane === laneRow.index
                                    readonly property bool muted:
                                        laneRow.assigned && root.laneMuted(laneRow.lane)

                                    visible: !root.kitView || laneRow.pad !== null
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    radius: Skin.radiusS
                                    color: laneRow.focused ? Skin.slotHover : Skin.slot
                                    border.width: laneRow.focused ? 1 : 0
                                    border.color: Skin.accent
                                    opacity: !root.kitView || laneRow.assigned ? 1 : 0.55

                                    TapHandler {
                                        onTapped: {
                                            if (root.kitView)
                                                root.focusRow(laneRow.index)
                                            else {
                                                root.setParam(root.idFocusedLane, laneRow.index)
                                                root.readAll()
                                            }
                                        }
                                    }
                                    WheelHandler {
                                        enabled: root.kitView && laneRow.assigned
                                        onWheel: event => {
                                            const dir = event.angleDelta.y > 0 ? 1 : -1
                                            if (event.modifiers & Qt.ShiftModifier)
                                                root.shiftOctave(laneRow.lane, dir)
                                            else
                                                root.bumpLaneNote(laneRow.lane, dir)
                                            event.accepted = true
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
                                            enabled: laneRow.assigned
                                            active: laneRow.muted
                                            activeColor: Skin.mute
                                            tip: qsTr("Mute lane %1. Its head keeps walking - only the sound stops.")
                                                .arg((laneRow.assigned ? laneRow.lane : laneRow.index) + 1)
                                            onClicked: {
                                                if (!laneRow.assigned) return
                                                Mixer.setSequencerLane(
                                                    root.targetRow, root.targetSlot, laneRow.lane,
                                                    root.laneNote(laneRow.lane),
                                                    root.laneLength(laneRow.lane),
                                                    root.laneDivision(laneRow.lane),
                                                    root.laneDirection(laneRow.lane),
                                                    root.laneChannel(laneRow.lane),
                                                    !laneRow.muted,
                                                    root.laneGate(laneRow.lane))
                                                root.readAll()
                                            }
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            Text {
                                                text: {
                                                    if (!root.kitView)
                                                        return qsTr("Lane %1").arg(laneRow.index + 1)
                                                    if (laneRow.pad)
                                                        return laneRow.pad.name
                                                    if (!laneRow.assigned)
                                                        return qsTr("empty")
                                                    const name = root.padName(root.laneNote(laneRow.lane))
                                                    const n = root.laneLength(laneRow.lane)
                                                    return n === 16 ? name : name + " " + n
                                                }
                                                color: laneRow.focused ? Skin.text : Skin.textDim
                                                font.pixelSize: Skin.fontXS
                                                font.bold: root.kitView
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            Text {
                                                visible: !root.kitView
                                                text: root.noteName(root.laneNote(laneRow.assigned
                                                    ? laneRow.lane : laneRow.index))
                                                color: laneRow.focused ? Skin.text : Skin.textDim
                                                font.pixelSize: Skin.fontXS
                                                font.bold: true
                                            }
                                        }

                                        ColumnLayout {
                                            visible: !root.padView
                                            spacing: 0
                                            StripButton {
                                                Layout.preferredWidth: Px.px(16)
                                                Layout.preferredHeight: Px.px(12)
                                                flat: true
                                                label: "+"
                                                tip: qsTr("This lane's own pitch, up a semitone. What an unlocked step plays.")
                                                onClicked: root.bumpLaneNote(laneRow.assigned
                                                    ? laneRow.lane : laneRow.index, 1)
                                            }
                                            StripButton {
                                                Layout.preferredWidth: Px.px(16)
                                                Layout.preferredHeight: Px.px(12)
                                                flat: true
                                                label: "−"
                                                tip: qsTr("This lane's own pitch, down a semitone.")
                                                onClicked: root.bumpLaneNote(laneRow.assigned
                                                    ? laneRow.lane : laneRow.index, -1)
                                            }
                                        }
                                    }
                                }
                            }

                            StripButton {
                                visible: root.kitView
                                Layout.fillWidth: true
                                Layout.preferredHeight: Px.px(16)
                                Layout.maximumHeight: Px.px(16)
                                flat: true
                                label: "▼"
                                enabled: root.padWindow > 0
                                tip: qsTr("Show lower pitches. Hits stay where Skyline put them.")
                                onClicked: root.nudgeVertical(-1)
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
                                        readonly property real note: root.soundingNote(column.lane, column.step)
                                        readonly property real velocity: root.cellVel(column.lane, column.step)
                                        readonly property real chance: root.cellChance(column.lane, column.step)
                                        readonly property bool accented: root.cellAccent(column.lane, column.step)
                                        readonly property bool tied: root.cellTie(column.lane, column.step)
                                        readonly property bool selectedStep: column.step === root.editStep
                                        readonly property bool selectedPitch: root.focusedPitch >= 0
                                            && Math.round(column.note) === root.focusedPitch

                                        width: (skylineColumns.width - (root.pageSteps - 1) * Px.px(2))
                                               / root.pageSteps
                                        height: skylineColumns.height
                                        opacity: column.inPattern ? 1.0 : 0.3

                                        Rectangle {
                                            anchors.fill: parent
                                            color: column.index % 4 === 0 ? Skin.strip : "transparent"
                                            radius: Skin.radiusS
                                            border.width: column.selectedStep ? 1 : 0
                                            border.color: Skin.accent
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

                                        // Same gold pip as the grid. Only this corner
                                        // eats the click — a strip across the top stole
                                        // taps meant to arm or mute the step.
                                        Item {
                                            id: accentTick
                                            anchors.top: parent.top
                                            anchors.right: parent.right
                                            width: Px.px(16)
                                            height: Px.px(16)

                                            Rectangle {
                                                visible: column.accented
                                                anchors.top: parent.top
                                                anchors.right: parent.right
                                                anchors.margins: Px.px(3)
                                                width: Px.px(5)
                                                height: Px.px(5)
                                                radius: Px.px(2.5)
                                                color: Skin.solo
                                            }

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
                                            anchors.top: parent.top
                                            anchors.topMargin: Px.px(4)
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
                                                border.width: column.selectedPitch && column.on ? 1 : 0
                                                border.color: Skin.text

                                                Behavior on color {
                                                    ColorAnimation { duration: Skin.fast }
                                                }
                                                Behavior on height {
                                                    NumberAnimation { duration: Skin.fast }
                                                }
                                            }
                                        }

                                        Text {
                                            anchors.horizontalCenter: pitchFill.horizontalCenter
                                            anchors.top: pitchFill.top
                                            anchors.topMargin: Px.px(2)
                                            visible: column.on && column.width > Px.px(22)
                                                     && pitchFill.height > Px.px(16)
                                            text: root.padName(root.heardNote(column.note))
                                            color: Skin.text
                                            font.pixelSize: Skin.fontXS
                                        }

                                        // Tie sits on the note itself. Anchoring it to `bar`
                                        // put a green tick in empty air whenever the pitch
                                        // was low, which read as a second playhead.
                                        Rectangle {
                                            visible: column.on && column.step < root.focusedLength - 1
                                            anchors.right: pitchFill.right
                                            anchors.verticalCenter: pitchFill.verticalCenter
                                            width: Px.px(5)
                                            height: Math.min(Px.px(10), pitchFill.height)
                                            radius: 1
                                            color: column.tied ? Skin.meterLow : Skin.border

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
                                            onTapped: eventPoint => {
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
                                        // whether or not it already was. A click with a
                                        // couple of jitter pixels is not a drag — that
                                        // used to paint the step back on and eat the tap
                                        // that would have turned it off.
                                        DragHandler {
                                            id: pitchDrag
                                            target: null
                                            xAxis.enabled: true
                                            yAxis.enabled: true

                                            property int paintNote: 60
                                            property bool painting: false

                                            function movedEnough() {
                                                const dx = pitchDrag.centroid.position.x
                                                         - pitchDrag.centroid.pressPosition.x
                                                const dy = pitchDrag.centroid.position.y
                                                         - pitchDrag.centroid.pressPosition.y
                                                return Math.sqrt(dx * dx + dy * dy) >= Px.px(8)
                                            }

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

                                            onActiveChanged: {
                                                if (!pitchDrag.active)
                                                    pitchDrag.painting = false
                                            }
                                            onCentroidChanged: {
                                                if (!pitchDrag.active) return
                                                if (!pitchDrag.painting && !pitchDrag.movedEnough())
                                                    return
                                                pitchDrag.painting = true
                                                pitchDrag.updatePaint()
                                            }
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

                                        readonly property int pitch: root.rowPitch(gridRow.index)
                                        readonly property var pad: root.visiblePad(gridRow.index)
                                        readonly property int lane: gridRow.pitch < 0
                                            ? -1 : root.laneForPitch(gridRow.pitch)
                                        readonly property bool assigned: gridRow.lane >= 0

                                        visible: gridRow.pitch >= 0
                                        width: gridRows.width
                                        height: (gridRows.height - (root.laneCount - 1) * Px.px(2))
                                                / root.laneCount
                                        spacing: Px.px(2)
                                        opacity: gridRow.assigned || !root.padView ? 1 : 0.55

                                        Repeater {
                                            model: root.pageSteps

                                            Rectangle {
                                                id: cell
                                                required property int index

                                                readonly property int step: root.stepOffset + cell.index
                                                readonly property int lane: {
                                                    if (gridRow.pitch < 0) return -1
                                                    const sounding = root.laneSoundingAt(
                                                        gridRow.pitch, cell.step)
                                                    return sounding >= 0 ? sounding : gridRow.lane
                                                }
                                                readonly property bool on:
                                                    gridRow.pitch >= 0
                                                    && root.pitchOn(gridRow.pitch, cell.step)
                                                readonly property bool inPattern: {
                                                    const voice = cell.lane >= 0
                                                        ? cell.lane : root.focusedLane
                                                    return cell.step < root.laneLength(voice)
                                                }
                                                readonly property bool atPlayhead: {
                                                    if (cell.lane < 0) return false
                                                    return root.nativeHeadStep(cell.lane) === cell.step
                                                }
                                                // The focused lane's step, drawn on every row so a
                                                // polymeter offset is a second light, not a mystery.
                                                readonly property bool atKitBeat:
                                                    root.playhead >= 0 &&
                                                    root.nativeHeadStep(root.focusedLane) === cell.step
                                                readonly property bool extraHere:
                                                    cell.lane >= 0
                                                    && root.extraHeadOnStep(cell.lane, cell.step)
                                                readonly property real velocity: cell.lane >= 0
                                                    ? root.cellVel(cell.lane, cell.step) : 100
                                                readonly property bool accented: cell.lane >= 0
                                                    && root.cellAccent(cell.lane, cell.step)

                                                width: (gridRow.width - (root.pageSteps - 1) * Px.px(2))
                                                       / root.pageSteps
                                                height: gridRow.height
                                                radius: Skin.radiusS
                                                opacity: cell.inPattern ? 1.0 : 0.3
                                                color: cell.on
                                                    ? Skin.accent
                                                    : (cell.index % 4 === 0 ? Skin.strip : Skin.slot)
                                                border.width: root.rowIsSelected(gridRow.pitch) ? 1 : 0
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
                                                    color: Skin.accent
                                                    opacity: parent.atKitBeat && !parent.atPlayhead ? 0.12 : 0
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
                                                    onTapped: root.toggleRowStep(gridRow.index, cell.step)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                        // Step lamps along the bottom: one per step of the
                        // focused lane - the whole lane, pages and all - the
                        // downbeats tallest, the beats taller, lit up to where
                        // the head is. A metronome you can see from across the
                        // room, and the one place a 64-step lane is seen
                        // entire. The page on screen is the bright stretch;
                        // tap the lamps anywhere to turn to that page.
                        Item {
                            id: lamps
                            Layout.fillWidth: true
                            Layout.preferredHeight: Px.px(12)
                            readonly property int steps: Math.max(1, Math.min(root.maxSteps, root.focusedLength))
                            readonly property int gapPx: lamps.steps > 32 ? 1 : Px.px(2)
                            readonly property real lampWidth: Math.max(1,
                                (lamps.width - (lamps.steps - 1) * lamps.gapPx) / lamps.steps)
                            readonly property int perBar: Math.max(1,
                                root.stepsPerBeat * Math.max(1, Mixer.timeNumerator()))
                            readonly property color hue: root.recording ? Skin.arm : Skin.meterLow

                            Repeater {
                                model: lamps.steps
                                Rectangle {
                                    required property int index
                                    readonly property bool downbeat: index % lamps.perBar === 0
                                    readonly property bool beat: index % Math.max(1, root.stepsPerBeat) === 0
                                    readonly property bool on: index === root.headStep
                                    readonly property bool past: root.headStep >= 0 && index < root.headStep
                                    readonly property bool onPage: index >= root.stepOffset
                                                                   && index < root.stepOffset + root.pageSteps
                                    x: index * (lamps.lampWidth + lamps.gapPx)
                                    y: parent.height - height
                                    width: lamps.lampWidth
                                    height: downbeat ? Px.px(12) : beat ? Px.px(8) : Px.px(5)
                                    radius: Px.px(1.5)
                                    color: on ? Qt.lighter(lamps.hue, 1.3)
                                         : past ? lamps.hue
                                         : downbeat ? Skin.border : Skin.slot
                                    border.width: downbeat && !on && !past ? 1 : 0
                                    border.color: Qt.rgba(lamps.hue.r, lamps.hue.g, lamps.hue.b, 0.5)
                                    opacity: (on ? 1 : past ? 0.6 : 0.9) * (onPage ? 1 : 0.4)
                                }
                            }

                            HoverHandler {
                                id: lampHover
                                cursorShape: root.pageCount > 1 ? Qt.PointingHandCursor : Qt.ArrowCursor
                            }
                            Tip {
                                text: qsTr("The focused lane's steps, all of them, lit up to the head. Tall lamps are bars, medium ones beats. The bright stretch is the page on screen - tap anywhere here to turn to that page.")
                                visible: lampHover.hovered
                            }
                            TapHandler {
                                onTapped: eventPoint => {
                                    const step = Math.floor(eventPoint.position.x
                                        / Math.max(1, lamps.lampWidth + lamps.gapPx))
                                    root.page = Math.max(0, Math.min(root.pageCount - 1,
                                        Math.floor(step / root.pageSteps)))
                                }
                            }
                        }
                    }

                    // The flare on the one and on a Rec press.
                    Rectangle {
                        anchors.fill: parent
                        radius: parent.radius
                        color: root.stateHue
                        opacity: root.flash * 0.10
                        visible: opacity > 0.005
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: parent.radius
                        color: "transparent"
                        border.width: Px.px(2)
                        border.color: root.stateHue
                        opacity: root.flash
                        visible: opacity > 0.005
                    }
                }

                // --- pattern ---------------------------------------------------------
                // Sixteen patterns. A click switches now; a right-click queues
                // one for the top of the bar and rings it yellow until then.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Skin.spacingXS

                    Item {
                        Layout.preferredWidth: patternLabel.implicitWidth + Px.px(10)
                        Layout.preferredHeight: Skin.buttonHeight
                        RowLabel {
                            id: patternLabel
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("PATTERN")
                        }
                        MapDot { param: root.idPattern }
                    }

                    Repeater {
                        model: root.patternCount
                        RowButton {
                            id: patternButton
                            required property int index
                            readonly property bool queued: root.nextPattern === patternButton.index
                                                           && root.pattern !== patternButton.index
                            Layout.fillWidth: true
                            Layout.preferredWidth: Px.px(30)
                            label: String(index + 1)
                            flat: !patternButton.active && !patternButton.queued
                            active: root.pattern === index
                            tip: root.mapping
                                 ? qsTr("Tap to bind the pattern number to the next control.")
                                 : qsTr("Pattern %1. Click switches now - notes already in the air keep their gate. Right-click queues it for the top of the bar.").arg(index + 1)
                            onClicked: {
                                if (root.mapping) { root.armParam(root.idPattern, 0, root.patternCount - 1); return }
                                root.setParam(root.idPattern, index)
                                root.readAll()
                            }
                            // Accepting only the right button here consumes
                            // it before the button sees it - the same trick
                            // the insert slots use for their menu.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                onClicked: {
                                    root.setParam(root.idNextPattern,
                                                  patternButton.queued ? -1 : patternButton.index)
                                    root.readAll()
                                }
                            }
                            Rectangle {
                                visible: patternButton.queued
                                anchors.fill: parent
                                radius: Skin.radius
                                color: "transparent"
                                border.width: Px.px(2)
                                border.color: Skin.solo
                            }
                        }
                    }
                }

                // --- view and edit ---------------------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Skin.spacingXS

                    RowButton {
                        Layout.preferredWidth: Px.px(64)
                        label: qsTr("Skyline")
                        active: root.viewMode === 0
                        tip: qsTr("One lane, pitch as a bar. The instrument for a melody or a bassline.")
                        onClicked: root.enterSkyline()
                    }
                    RowButton {
                        Layout.preferredWidth: Px.px(48)
                        label: qsTr("Grid")
                        active: root.viewMode === 1
                        tip: qsTr("Kit view: eight drum pads. Hits, Euclid, Fill and mute are the beat tools — scale and Notes hide.")
                        onClicked: {
                            root.setParam(root.idView, 1)
                            root.page = 0
                            root.readAll()
                            root.revealFocusedVoice()
                        }
                    }

                    Item { Layout.preferredWidth: Skin.spacingS }

                    RowButton {
                        Layout.preferredWidth: Px.px(28)
                        label: "←"
                        tip: qsTr("Nudge the focused lane's pattern one step earlier.")
                        onClicked: root.fire(root.idNudgeLeft)
                    }
                    RowButton {
                        Layout.preferredWidth: Px.px(28)
                        label: "→"
                        tip: qsTr("Nudge the focused lane's pattern one step later.")
                        onClicked: root.fire(root.idNudgeRight)
                    }
                    RowButton {
                        Layout.preferredWidth: Px.px(40)
                        label: qsTr("Hits")
                        tip: root.kitView
                            ? qsTr("Randomize which pads fire. The drum notes stay.")
                            : qsTr("Randomize which steps are on. Notes stay.")
                        onClicked: root.fire(root.idRandomHits)
                    }
                    RowButton {
                        visible: !root.kitView
                        Layout.preferredWidth: Px.px(44)
                        label: qsTr("Notes")
                        tip: qsTr("Randomize pitches, snapped to the scale.")
                        onClicked: root.fire(root.idRandomNotes)
                    }
                    RowButton {
                        Layout.preferredWidth: Px.px(48)
                        label: qsTr("Mutate")
                        tip: root.kitView
                            ? qsTr("Flip a few hits on this pattern. Pad pitches stay.")
                            : qsTr("Nudge the current pattern: a few hits flip, locked pitches walk a scale degree.")
                        onClicked: root.fire(root.idMutate)
                    }
                    RowButton {
                        Layout.preferredWidth: Px.px(44)
                        label: qsTr("Clear")
                        danger: true
                        tip: qsTr("Turn every step off. The pitches stay.")
                        onClicked: root.fire(root.idClearHits)
                    }

                    Item { Layout.fillWidth: true }

                    RowButton {
                        visible: root.kitView
                        Layout.preferredWidth: Px.px(32)
                        label: "−12"
                        tip: qsTr("Focused pad, down an octave. Wheel a row to walk it; Shift+wheel is an octave. Skyline still reaches any pitch on a step.")
                        onClicked: root.shiftOctave(root.focusedLane, -1)
                    }
                    RowButton {
                        visible: root.kitView
                        Layout.preferredWidth: Px.px(32)
                        label: "+12"
                        tip: qsTr("Focused pad, up an octave.")
                        onClicked: root.shiftOctave(root.focusedLane, 1)
                    }
                    RowButton {
                        visible: root.kitView && root.targetPads.length > 0
                        Layout.preferredWidth: Px.px(40)
                        label: qsTr("pads")
                        tip: qsTr("Assign this window of %1 to the eight lanes. Scrolling the grid does not do that — the kick pattern stays on the kick.").arg(root.targetName)
                        onClicked: root.pullPadWindow(root.padWindow)
                    }

                    Item { visible: root.pageCount > 1; Layout.preferredWidth: Skin.spacingS }

                    // Pages of sixteen, when a lane or the grid runs past
                    // that. The lamps under the grid turn pages too.
                    RowButton {
                        visible: root.pageCount > 1
                        Layout.preferredWidth: Px.px(28)
                        label: "‹"
                        enabled: root.page > 0
                        tip: qsTr("Previous page of sixteen steps.")
                        onClicked: root.page = Math.max(0, root.page - 1)
                    }
                    Text {
                        visible: root.pageCount > 1
                        text: (Math.min(root.page, root.pageCount - 1) + 1) + "/" + root.pageCount
                        color: Skin.textDim
                        font.pixelSize: Skin.fontS
                        font.family: Skin.monoFamily
                    }
                    RowButton {
                        visible: root.pageCount > 1
                        Layout.preferredWidth: Px.px(28)
                        label: "›"
                        enabled: root.page < root.pageCount - 1
                        tip: qsTr("Next page of sixteen steps.")
                        onClicked: root.page = Math.min(root.pageCount - 1, root.page + 1)
                    }
                }

                // --- run and scale: the melody tools, skyline only ---------------------
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Skin.spacingXS
                    visible: !root.kitView

                    RowLabel { text: qsTr("RUN") }
                    Item { Layout.preferredWidth: Skin.spacingXS }

                    component DirButton: RowButton {
                        required property int forValue
                        Layout.preferredWidth: Px.px(28)
                        active: root.laneDirection(root.focusedLane) === forValue
                        onClicked: root.setParam(root.idDirection, forValue)
                    }
                    DirButton { forValue: 0; label: "→"; tip: qsTr("Forward.") }
                    DirButton { forValue: 1; label: "←"; tip: qsTr("Reverse.") }
                    DirButton { forValue: 2; label: "↔"; tip: qsTr("Pendulum.") }
                    DirButton { forValue: 3; label: qsTr("?"); tip: qsTr("A new step each time.") }

                    Item { Layout.preferredWidth: Skin.spacingS }
                    RowLabel { text: qsTr("SCALE") }
                    Item { Layout.preferredWidth: Skin.spacingXS }

                    Repeater {
                        model: root.scaleNames
                        RowButton {
                            required property int index
                            required property string modelData
                            Layout.fillWidth: true
                            Layout.preferredWidth: Px.px(36)
                            label: modelData
                            active: root.scaleId === index
                            tip: qsTr("Notes snap to this scale.")
                            onClicked: root.setParam(root.idScale, index)
                        }
                    }
                    RowButton {
                        Layout.preferredWidth: Px.px(32)
                        label: root.rootNames[root.root] || "C"
                        tip: qsTr("Root of the scale. Click to walk it.")
                        onClicked: root.setParam(root.idRoot, (root.root + 1) % 12)
                    }
                }

                // --- the focused lane ---------------------------------------------------
                // What one lane is: how fine its steps are, how many before it
                // loops, how long each note holds, which channel it speaks on,
                // and Euclid to spread hits across it. Lanes can differ - that
                // is polymeter.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Skin.spacingXS

                    Item {
                        Layout.preferredWidth: laneLabel.implicitWidth + Px.px(10)
                        Layout.preferredHeight: Skin.buttonHeight
                        RowLabel {
                            id: laneLabel
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("LANE %1").arg(root.focusedLane + 1)
                        }
                        MapDot { param: root.idDivision }
                    }

                    component DivisionButton: RowButton {
                        required property int forValue
                        Layout.preferredWidth: Px.px(36)
                        label: root.divisionNames[forValue]
                        active: root.laneDivision(root.focusedLane) === forValue
                        tip: root.mapping
                             ? qsTr("Tap to bind the division to the next control.")
                             : qsTr("One step is a %1 note on this lane.").arg(root.divisionNames[forValue])
                        onClicked: {
                            if (root.mapping) { root.armParam(root.idDivision, 0, 5); return }
                            root.setParam(root.idDivision, forValue)
                        }
                    }
                    DivisionButton { forValue: 0 }
                    DivisionButton { forValue: 1 }
                    DivisionButton { forValue: 2 }
                    DivisionButton { forValue: 3 }
                    DivisionButton { forValue: 4 }
                    DivisionButton { forValue: 5 }

                    Item { Layout.preferredWidth: Skin.spacingXS }

                    // A ValueTrack with the map ring in its corner.
                    component LaneTrack: Item {
                        id: laneTrack
                        property alias label: track.label
                        property alias valueText: track.valueText
                        property alias value: track.value
                        property alias step: track.step
                        property alias fineStep: track.fineStep
                        property alias tip: track.tip
                        required property int param
                        property real armMin: 0
                        property real armMax: 1
                        signal moved(real value)
                        Layout.fillWidth: true
                        Layout.preferredHeight: Skin.buttonHeight
                        ValueTrack {
                            id: track
                            anchors.fill: parent
                            pickOnly: root.mapping
                            onMoved: v => laneTrack.moved(v)
                            onPicked: root.armParam(laneTrack.param, laneTrack.armMin, laneTrack.armMax)
                        }
                        MapDot { param: laneTrack.param }
                    }

                    LaneTrack {
                        Layout.preferredWidth: Px.px(84)
                        param: root.idLength
                        armMax: root.maxSteps
                        label: qsTr("steps")
                        valueText: root.focusedLength
                        value: (root.focusedLength - 1) / (root.maxSteps - 1)
                        step: 1 / (root.maxSteps - 1)
                        fineStep: 1 / (root.maxSteps - 1)
                        tip: qsTr("How many of this lane's steps play before it loops. Lanes can differ - that is polymeter.")
                        onMoved: v => root.setParam(root.idLength, Math.round(1 + v * (root.maxSteps - 1)))
                    }
                    LaneTrack {
                        Layout.preferredWidth: Px.px(76)
                        param: root.idGate
                        armMin: 0.05
                        label: qsTr("gate")
                        valueText: Math.round(root.laneGate(root.focusedLane) * 100) + "%"
                        value: (root.laneGate(root.focusedLane) - 0.05) / 0.95
                        tip: qsTr("How long each note holds, as a share of its step. Full is legato into the next.")
                        onMoved: v => root.setParam(root.idGate, 0.05 + v * 0.95)
                    }
                    LaneTrack {
                        Layout.preferredWidth: Px.px(60)
                        param: root.idChannel
                        armMax: 15
                        label: qsTr("ch")
                        valueText: root.laneChannel(root.focusedLane) + 1
                        value: root.laneChannel(root.focusedLane) / 15
                        step: 1 / 15
                        fineStep: 1 / 15
                        tip: qsTr("MIDI channel this lane sends on.")
                        onMoved: v => root.setParam(root.idChannel, Math.round(v * 15))
                    }
                    LaneTrack {
                        Layout.preferredWidth: Px.px(76)
                        param: root.idEuclid
                        armMax: root.maxSteps
                        label: qsTr("euclid")
                        valueText: root.laneEuclid(root.focusedLane)
                        value: root.laneEuclid(root.focusedLane) / root.maxSteps
                        step: 1 / root.maxSteps
                        fineStep: 1 / root.maxSteps
                        tip: qsTr("Spread this many hits evenly across the focused lane — a beat, not a scale.")
                        onMoved: v => {
                            root.setParam(root.idEuclid, Math.round(v * root.maxSteps))
                            root.readAll()
                        }
                    }
                }

                // --- the selected step ---------------------------------------------------
                // What one hit does beyond being on: how many times it fires
                // inside its step, and when it is allowed to.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Skin.spacingXS
                    visible: root.cellOn(root.focusedLane, root.editStep)

                    RowLabel {
                        Layout.preferredWidth: Px.px(52)
                        text: qsTr("STEP %1").arg(root.editStep + 1)
                    }
                    ValueTrack {
                        Layout.preferredWidth: Px.px(84)
                        Layout.minimumWidth: 0
                        Layout.preferredHeight: Skin.buttonHeight
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
                        RowButton {
                            required property int index
                            required property string modelData
                            Layout.fillWidth: true
                            Layout.preferredWidth: Px.px(36)
                            flat: !active
                            label: modelData
                            active: root.cellCond(root.focusedLane, root.editStep) === index
                            tip: [qsTr("Fires every time."),
                                  qsTr("Only while Fill is held."),
                                  qsTr("Only while Fill is not held."),
                                  qsTr("Only if the step before it fired."),
                                  qsTr("Only if the step before it did not fire."),
                                  qsTr("Only if the neighbouring lane fired this step."),
                                  qsTr("Every second time round.")][index]
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
            }

            // --- the macros and the weather --------------------------------------------
            // A layout fills by default; this one is told not to, or it takes
            // the grid's room.
            ColumnLayout {
                Layout.fillWidth: false
                Layout.preferredWidth: Px.px(230)
                Layout.maximumWidth: Px.px(230)
                Layout.fillHeight: true
                spacing: Skin.spacing

                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: Skin.spacing

                    ParamBar {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        big: true
                        bipolar: true
                        label: qsTr("DENSITY")
                        hue: root.hueDensity
                        value: (root.macros[0] ?? 1) / 2
                        readout: "×" + (root.macros[0] ?? 1).toFixed(2)
                        mapped: root.isMapped(root.idDensity)
                        waiting: root.waitingParam === root.idDensity
                        mapping: root.mapping
                        tip: qsTr("How much of the pattern plays. The middle is what you wrote; below it hits drop out, above it off steps can fire as quiet ghost notes. Rides from the middle, so ×1 reads as nothing added.")
                        onEdited: v => {
                            root.macros = root.setField(root.macros, 0, v * 2)
                            root.setParam(root.idDensity, v * 2)
                        }
                        onArmed: root.armParam(root.idDensity, 0, 2)
                    }
                    ParamBar {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        big: true
                        label: qsTr("CHAOS")
                        hue: root.hueChaos
                        value: root.macros[1] ?? 0
                        readout: Math.round((root.macros[1] ?? 0) * 100)
                        mapped: root.isMapped(root.idChaos)
                        waiting: root.waitingParam === root.idChaos
                        mapping: root.mapping
                        tip: qsTr("Jitters the timing of every hit and can flip one. Nothing at the bottom; at the top a pattern that never plays the same way twice. The head itself keeps time.")
                        onEdited: v => {
                            root.macros = root.setField(root.macros, 1, v)
                            root.setParam(root.idChaos, v)
                        }
                        onArmed: root.armParam(root.idChaos, 0, 1)
                    }
                }

                // Told not to fill: a nested layout does by default, and this
                // one then took half the column from the two big bars.
                GridLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: false
                    Layout.preferredHeight: Px.px(170)
                    columns: 2
                    columnSpacing: Skin.spacingS
                    rowSpacing: Skin.spacingS

                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("PROB"); hue: root.hueProb
                        value: root.macros[3] ?? 1
                        readout: Math.round((root.macros[3] ?? 1) * 100)
                        mapped: root.isMapped(root.idMasterProb)
                        waiting: root.waitingParam === root.idMasterProb
                        mapping: root.mapping
                        tip: qsTr("A ceiling on every step's own chance. Full leaves each step to its own; pull it down and the whole pattern thins.")
                        onEdited: v => {
                            root.macros = root.setField(root.macros, 3, v)
                            root.setParam(root.idMasterProb, v)
                        }
                        onArmed: root.armParam(root.idMasterProb, 0, 1)
                    }
                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("RATCHET"); hue: root.hueRatchet
                        value: root.macros[2] ?? 0
                        readout: Math.round((root.macros[2] ?? 0) * 100)
                        mapped: root.isMapped(root.idRatchetAmount)
                        waiting: root.waitingParam === root.idRatchetAmount
                        mapping: root.mapping
                        tip: qsTr("How hard per-step ratchets fire. Zero leaves one hit however many a step asks for; full gives every step its count.")
                        onEdited: v => {
                            root.macros = root.setField(root.macros, 2, v)
                            root.setParam(root.idRatchetAmount, v)
                        }
                        onArmed: root.armParam(root.idRatchetAmount, 0, 1)
                    }
                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("SWING"); hue: root.hueSwing
                        value: root.swing
                        readout: Math.round(root.swing * 100)
                        mapped: root.isMapped(root.idSwing)
                        waiting: root.waitingParam === root.idSwing
                        mapping: root.mapping
                        tip: qsTr("Off-beats lean late. Nothing at the bottom is straight; the top is a hard shuffle. Global, over every lane.")
                        onEdited: v => {
                            root.swing = v
                            root.setParam(root.idSwing, v)
                        }
                        onArmed: root.armParam(root.idSwing, 0, 1)
                    }
                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("TRANSPOSE"); hue: root.hueTranspose
                        bipolar: true
                        value: (root.transpose + 24) / 48
                        readout: (root.transpose > 0 ? "+" : root.transpose < 0 ? "−" : "")
                                 + Math.abs(Math.round(root.transpose))
                        step: 1 / 48
                        fineStep: 1 / 48
                        mapped: root.isMapped(root.idTranspose)
                        waiting: root.waitingParam === root.idTranspose
                        mapping: root.mapping
                        tip: qsTr("Every lane, up or down, two octaves either way in semitones. On a kit that moves every pad - handy for a second bank of samples, a trap otherwise. Wheel moves a semitone.")
                        onEdited: v => {
                            root.transpose = Math.round(v * 48 - 24)
                            root.setParam(root.idTranspose, root.transpose)
                        }
                        onArmed: root.armParam(root.idTranspose, -24, 24)
                    }
                }
            }
        }

        // --- status line -------------------------------------------------------------
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: {
                if (root.waitingParam >= 0)
                    return qsTr("Turn a knob on this strip's MIDI input… Esc cancels.")
                if (root.mapping)
                    return qsTr("Tap the control you want, then turn a knob.")
                if (root.targetName.length === 0)
                    return qsTr("Nothing sits below this sequencer on the strip. Put an instrument in the next slot down and the notes reach it.")
                if (root.recording)
                    return qsTr("Recording onto lane %1. Play your MIDI input; each note lands on the nearest step. The pattern is quiet until Rec goes off.").arg(root.focusedLane + 1)
                if (root.nextPattern >= 0 && root.nextPattern !== root.pattern)
                    return qsTr("Pattern %1 takes over at the top of the bar. Right-click it again to cancel.").arg(root.nextPattern + 1)
                if (root.kitView) {
                    const pads = root.rowPitches.length > root.laneCount
                        ? qsTr("Pads %1–%2 of %3 from %4; ▲▼ scroll them. ")
                              .arg(root.padWindow + 1)
                              .arg(Math.min(root.padWindow + root.laneCount, root.rowPitches.length))
                              .arg(root.rowPitches.length)
                              .arg(root.targetName)
                        : ""
                    return pads + qsTr("Tap a pad on a step to hit it there. Wheel a row to retune its pad, Shift+wheel for an octave. Page %1 of %2 - the lamps turn pages too.")
                        .arg(Math.min(root.page, root.pageCount - 1) + 1).arg(root.pageCount)
                }
                return qsTr("Tap a column where the note should sit; drag sideways to paint a run. The strips under each note are chance and velocity; the corner pip is accent, the notch on the note a tie.")
            }
            color: root.mapping || Mixer.learning ? Skin.solo
                 : root.targetName.length === 0 ? Skin.solo
                 : root.recording ? Skin.arm : Skin.textDim
            font.pixelSize: Skin.fontXS
            wrapMode: Text.WordWrap
        }
    }
}
