pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// A tape and two rides. The tape is the loop, drawn wide across the window:
// trim it by dragging its edges, fade it by dragging the small marks just
// inside them, and watch the head go round. The rides on the right are what
// a looper performer actually holds during a take - how much of the stack
// survives each pass, and how loud the stack sits under the live signal -
// with pitch, speed and tone as the smaller weather below them. Record,
// play, clear, quantise and gain are still numbers underneath -
// LooperInstance keeps them reachable by id for MIDI-learn - but nothing
// here asks to be read as one.
//
// The window glows in the colour of what the looper is doing: red while it
// writes, green while it plays, yellow while it counts in, as bright as the
// loop is loud. Half seen across a dark stage, that is the whole state.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1

    // Mirrors of the engine's own state, refreshed on open and on a timer
    // while the popup is up. Dragging a handle updates its half of this
    // immediately, ahead of the next refresh, so the handle never visibly
    // snaps back while the finger is still on it.
    property var peaks: []
    // Which overdub pass most recently touched each bucket of `peaks`,
    // same indexing - see Mixer.looperLayers() / layerColor() below.
    property var layers: []
    property real trimStart: 0.0
    property real trimEnd: 1.0
    property real fadeIn: 0.0
    property real fadeOut: 0.0
    property real playPosition: -1.0
    // The loop's own level, already mapped onto the fader's travel like
    // every other meter in this app - see Mixer.gainToFader().
    property real loopLevel: 0.0
    property real loopLevelHold: 0.0
    property real quantize: 2
    property real gain: 1.0
    property real pitch: 0
    property real tone: 1
    property real loopBeats: 0
    // Rec is down. The head may not be on the tape yet: see `writing`.
    property bool recording: false
    // The head is actually writing. Between a press and the grid these two
    // disagree, and that gap is what the sign over the tape counts down.
    property bool writing: false
    // Beats until a pending press lands, -1 with no grid to wait for.
    property real beatsToBoundary: -1
    // The last head position seen, to catch the wrap - the loop's "one".
    property real lastPosition: -1
    // Lit for a moment on the one, and when the head punches in or out:
    // the tape's border and a wash over it flare in the state's colour.
    property real flash: 0
    property bool playing: true
    // Audio on the tape, including a first pass still being written. A silent
    // take still counts: peak-scanning treated digital zero as empty.
    property bool hasAudio: false
    // A closed loop, as opposed to the first record pass.
    property bool hasLoop: false
    property bool canUndo: false
    property bool canRedo: false
    property bool canMultiply: false
    property bool reverse: false
    property bool once: false
    property bool replace: false
    property real speed: 1
    property real feedback: 1
    property bool countIn: false
    property int countBeats: 0
    // Length reading Sync: which other Looper this one follows, and the
    // full list to pick one from - [{row, slot, label}], refreshed with the
    // rest of the structural state.
    property int syncTargetRow: -1
    property int syncTargetSlot: -1
    property var syncCandidates: []
    property bool mapping: false
    property int waitingParam: -1
    property var mapped: ({})
    // Set once, the first time this ever opens - after that the popup stays
    // wherever it was last dragged, the same as a real tool window would.
    property bool positioned: false

    // Parameter ids, mirrored from LooperInstance.
    readonly property int pRecord: 0
    readonly property int pPlay: 1
    readonly property int pClear: 2
    readonly property int pQuantize: 3
    readonly property int pGain: 4
    readonly property int pPitch: 5
    readonly property int pTone: 6
    readonly property int pReverse: 7
    readonly property int pFeedback: 8
    readonly property int pReplace: 9
    readonly property int pOnce: 10
    readonly property int pSpeed: 11
    readonly property int pCountIn: 12

    // Rec pressed, head not on the tape yet: waiting for the grid.
    readonly property bool armed: root.recording && !root.writing && root.countBeats <= 0
    // Rec released, head still on the tape until the grid.
    readonly property bool punchingOut: !root.recording && root.writing
    // Beats to go until a pending press lands, whole, for the sign.
    readonly property int boundaryIn: root.beatsToBoundary > 0
                                      ? Math.max(1, Math.ceil(root.beatsToBoundary - 0.02)) : 0
    // Where the head is in the loop, in beats from its top.
    readonly property real loopBeatPos: root.hasLoop && root.playPosition >= 0
                                        ? Math.max(0, Math.min(root.loopBeats, root.playPosition * root.loopBeats))
                                        : -1
    // Beats until the head comes round to the top again - the one.
    readonly property int oneIn: root.loopBeatPos >= 0
                                 ? Math.max(1, Math.ceil(root.loopBeats - root.loopBeatPos - 0.02)) : 0
    // The loop's own bar.beat, the way the transport shows the song's.
    readonly property string loopClock: {
        if (root.loopBeatPos < 0) return ""
        const num = Math.max(1, Mixer.timeNumerator())
        const beat = Math.floor(root.loopBeatPos + 0.02)
        return (Math.floor(beat / num) + 1) + "." + (beat % num + 1)
    }

    // What the looper is doing, in a word, and in a colour. The colour is
    // the one the glow, the chip, the tape's border and the playhead all
    // agree on, so the state is never read from a label alone. Yellow is
    // always "about to": counting, armed, or on the way out.
    readonly property string stageLabel:
        root.countBeats > 0 ? qsTr("COUNT")
        : root.armed ? qsTr("ARMED")
        : root.punchingOut ? (root.hasLoop ? qsTr("ENDING") : qsTr("CLOSING"))
        : root.writing
            ? (root.hasLoop
                   ? (root.replace ? qsTr("REPLACING") : qsTr("OVERDUBBING"))
                   : qsTr("RECORDING"))
            : !root.hasAudio ? qsTr("EMPTY")
            : !root.playing ? qsTr("STOPPED")
            : root.reverse && root.once ? qsTr("REVERSE ONCE")
            : root.reverse ? qsTr("REVERSE")
            : root.once ? qsTr("ONCE")
            : qsTr("PLAYING")
    readonly property color stateHue:
        root.countBeats > 0 || root.armed || root.punchingOut ? Skin.solo
        : root.writing ? Skin.arm
        : root.hasAudio && root.playing ? Skin.meterLow
        : root.hasAudio ? Skin.textDim
        : Skin.accent

    // The rides and the weather, each in a colour of its own, kept clear of
    // what colour already means in here: Rec's red, Count's yellow, and the
    // base take's accent blue, which Feedback shares because it is the base
    // take's own survival.
    readonly property color hueFeedback: Skin.accent
    readonly property color hueGain: Qt.hsla(0.40, 0.50, 0.58, 1.0)
    readonly property color huePitch: Qt.hsla(0.76, 0.55, 0.68, 1.0)
    readonly property color hueSpeed: Qt.hsla(0.08, 0.70, 0.62, 1.0)
    readonly property color hueTone: Qt.hsla(0.50, 0.50, 0.58, 1.0)

    width: Px.px(900)
    height: Px.px(600)
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

        // The glow: light under a door, in the state's colour, as bright as
        // the loop is loud. Recording keeps a floor so a silent take still
        // shows the tape is rolling; counting in breathes with the click.
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
            opacity: root.countBeats > 0 || root.armed || root.punchingOut ? 0.22
                   : root.writing ? Math.max(0.14, Math.min(0.32, root.loopLevel * 0.5))
                   : root.hasAudio && root.playing ? Math.min(0.32, root.loopLevel * 0.5)
                   : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        // Empty chrome is not a handler on its own, so without this a press
        // on the padding falls through onto the strip behind - and, now that
        // the popup can sit anywhere, doubles as how it moves.
        HoverHandler {}
        TapHandler {}
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

    function openFor(row, slot) {
        root.targetRow = row
        root.targetSlot = slot
        root.refreshAll()
        if (!root.positioned) {
            // Clamped: opened before the window has its size, the centre of
            // a zero-sized overlay is off the top-left corner.
            root.x = Math.max(0, Math.round((Overlay.overlay.width - root.width) / 2))
            root.y = Math.max(0, Math.round((Overlay.overlay.height - root.height) / 2))
            root.positioned = true
        }
        root.open()
    }

    onOpened: {
        positionTimer.start()
        waveformTimer.start()
    }
    onClosed: {
        positionTimer.stop()
        waveformTimer.stop()
        root.stopMapping()
    }

    function isMapped(id) {
        return root.mapped[id] === true
    }

    function refreshMapped() {
        const next = {}
        for (let id = 0; id <= 12; ++id)
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

    function setParam(id, value) {
        Mixer.setInsertParameter(root.targetRow, root.targetSlot, id, value)
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

    // Parented to the overlay, not to this popup's content: openAt positions
    // in overlay coordinates, and a popup declared inside another one would
    // otherwise add the outer popup's offset again.
    SlotMenu {
        id: syncMenu
        parent: Overlay.overlay
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
    // drone's are, so the row reads as a transport and not as a strip.
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

    function refreshAll() {
        root.refreshWaveform()
        root.refreshPosition()
        root.refreshTransport()
    }

    function refreshTransport() {
        root.recording = Mixer.looperRecording(root.targetRow, root.targetSlot)
        root.playing = Mixer.looperPlaying(root.targetRow, root.targetSlot)
        root.hasAudio = Mixer.looperHasAudio(root.targetRow, root.targetSlot)
        root.hasLoop = Mixer.looperLoopClosed(root.targetRow, root.targetSlot)
        root.canUndo = Mixer.looperCanUndo(root.targetRow, root.targetSlot)
        root.canRedo = Mixer.looperCanRedo(root.targetRow, root.targetSlot)
        root.canMultiply = Mixer.looperCanMultiply(root.targetRow, root.targetSlot)
        root.loopBeats = Mixer.looperBeats(root.targetRow, root.targetSlot)
        root.countIn = Mixer.looperCountIn(root.targetRow, root.targetSlot)
        root.syncTargetRow = Mixer.looperSyncTargetRow(root.targetRow, root.targetSlot)
        root.syncTargetSlot = Mixer.looperSyncTargetSlot(root.targetRow, root.targetSlot)
        root.syncCandidates = Mixer.looperSyncCandidates(root.targetRow, root.targetSlot)
        root.refreshMapped()
        for (const p of Mixer.insertParameters(root.targetRow, root.targetSlot)) {
            if (p.id === root.pQuantize) root.quantize = p.value
            else if (p.id === root.pGain) root.gain = p.value
            else if (p.id === root.pPitch) root.pitch = p.value
            else if (p.id === root.pTone) root.tone = p.value
            else if (p.id === root.pReverse) root.reverse = p.value >= 0.5
            else if (p.id === root.pFeedback) root.feedback = p.value
            else if (p.id === root.pReplace) root.replace = p.value >= 0.5
            else if (p.id === root.pOnce) root.once = p.value >= 0.5
            else if (p.id === root.pSpeed) root.speed = p.value
            else if (p.id === root.pCountIn) root.countIn = p.value >= 0.5
        }
    }

    function refreshWaveform() {
        root.peaks = Mixer.looperWaveform(root.targetRow, root.targetSlot, 160)
        root.layers = Mixer.looperLayers(root.targetRow, root.targetSlot, 160)
        root.trimStart = Mixer.looperTrimStart(root.targetRow, root.targetSlot)
        root.trimEnd = Mixer.looperTrimEnd(root.targetRow, root.targetSlot)
        root.fadeIn = Mixer.looperFadeIn(root.targetRow, root.targetSlot)
        root.fadeOut = Mixer.looperFadeOut(root.targetRow, root.targetSlot)
    }

    // Layer 0, the base take, keeps the same accent every other one-pass
    // loop has always drawn in - only a second pass and up gets a colour of
    // its own, stepped by a golden-angle-ish hue so nearby layers stay easy
    // to tell apart without clashing with what colour means everywhere else
    // in this popup (Rec's red, Count's yellow).
    function layerColor(layer) {
        if (layer <= 0) return Skin.accent
        const hue = (0.58 + layer * 0.13) % 1.0
        return Qt.hsla(hue, 0.6, 0.62, 1.0)
    }

    // How many passes are on the tape, from the highest layer any bucket
    // was last touched by. Drawn as a row of dots over the tape.
    readonly property int layerCount: {
        let top = -1
        for (const l of root.layers) if (l > top) top = l
        return top + 1
    }

    // The loop's length in words: bars when it is a whole number of them,
    // beats otherwise, nothing when there is no loop yet.
    function lengthLabel(beats) {
        const num = Mixer.timeNumerator()
        const bars = num > 0 ? beats / num : 0
        if (bars >= 0.95 && Math.abs(bars - Math.round(bars)) < 0.08)
            return qsTr("%1 bars").arg(Math.round(bars))
        return beats > 0 ? qsTr("%1 beats").arg(beats.toFixed(1)) : ""
    }

    // The label for whichever Looper Sync is currently pointed at, from the
    // same list the picker opens - so it always names something that is
    // actually still choosable, not a row/slot that got removed underneath.
    function syncTargetLabel() {
        for (const c of root.syncCandidates)
            if (c.row === root.syncTargetRow && c.slot === root.syncTargetSlot)
                return c.label
        return ""
    }

    function openSyncMenu(item) {
        const entries = root.syncCandidates.map(c => ({
            label: c.label,
            action: () => {
                Mixer.setLooperSyncTarget(root.targetRow, root.targetSlot,
                                          c.row, c.slot)
                root.syncTargetRow = c.row
                root.syncTargetSlot = c.slot
            }
        }))
        if (entries.length === 0)
            entries.push({
                label: qsTr("No other Looper in this session yet"),
                enabled: false,
                action: () => {}
            })
        syncMenu.openAt(item, entries, qsTr("Sync Length to"))
    }

    function refreshPosition() {
        const position = Mixer.looperPosition(root.targetRow, root.targetSlot)
        // The head came round: that was the one.
        if (root.hasLoop && position >= 0 && root.lastPosition >= 0
                && (root.reverse ? position > root.lastPosition + 0.5
                                 : position < root.lastPosition - 0.5))
            root.flare()
        root.lastPosition = position
        root.playPosition = position
        root.countBeats = Mixer.looperCountBeats(root.targetRow, root.targetSlot)
        const writing = Mixer.looperWriting(root.targetRow, root.targetSlot)
        // The head punched in or out: the moment the player was waiting for.
        if (writing !== root.writing) root.flare()
        root.writing = writing
        root.beatsToBoundary = Mixer.looperBeatsToBoundary(root.targetRow, root.targetSlot)
        const level = Mixer.gainToFader(Mixer.looperLevel(root.targetRow, root.targetSlot))
        root.loopLevel = level
        // Instant rise, steady fall - the same ballistic every other meter
        // in this app uses, just done here instead of in C++ since this is
        // the only place that wants it at this rate.
        root.loopLevelHold = Math.max(level, root.loopLevelHold - 0.012)
    }

    // The playhead moves every block; the waveform only changes while
    // recording or right after a clear. Polling it at the same rate would
    // mean rescanning up to a minute of audio fifteen times a second for a
    // line that is not moving.
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

    Timer {
        id: positionTimer
        interval: 40
        repeat: true
        onTriggered: root.refreshPosition()
    }
    Timer {
        id: waveformTimer
        interval: 400
        repeat: true
        onTriggered: {
            root.refreshWaveform()
            root.refreshTransport()
        }
    }

    contentItem: ColumnLayout {
        spacing: Skin.spacing

        // --- header ------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingS

            Text {
                text: qsTr("LOOPER")
                color: Skin.text
                font.pixelSize: Skin.fontL
                font.bold: true
                font.letterSpacing: Px.px(2)
            }

            Item { Layout.preferredWidth: Skin.spacing }

            // The state, on a chip: a dot in the state's colour, the word
            // for it, the loop's length, and the loop's own level - separate
            // from the channel meter beside it in the mixer, which is the
            // loop plus whatever is passing through live and does not say
            // whether the stack itself is getting hot.
            Rectangle {
                id: stageChip
                Layout.preferredWidth: Px.px(300)
                Layout.preferredHeight: Skin.buttonHeight + Px.px(6)
                radius: Skin.radius
                color: Skin.slotEmpty
                border.width: 1
                border.color: Qt.rgba(root.stateHue.r, root.stateHue.g, root.stateHue.b, 0.6)
                property real pulse: 1

                SequentialAnimation on pulse {
                    running: root.writing || root.countBeats > 0 || root.armed || root.punchingOut
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
                        opacity: root.writing || root.countBeats > 0 || root.armed || root.punchingOut
                                 ? stageChip.pulse
                               : root.hasAudio && root.playing ? 1 : 0.5
                    }

                    Text {
                        text: root.countBeats > 0 ? root.countBeats : root.stageLabel
                        color: root.stateHue
                        font.pixelSize: root.countBeats > 0 ? Skin.fontXL : Skin.fontL
                        font.bold: true
                        font.letterSpacing: Px.px(1)
                    }

                    // Where the head is in the loop, bar.beat, so a player
                    // reading the song's clock in the top bar reads the
                    // loop's the same way.
                    Text {
                        visible: root.loopClock.length > 0
                        text: root.loopClock
                        color: Skin.text
                        font.pixelSize: Skin.fontL
                        font.bold: true
                        font.family: Skin.monoFamily
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: root.hasLoop ? root.lengthLabel(root.loopBeats)
                            : wave.defining && wave.targetBeats > 0
                                ? qsTr("→ %1").arg(root.lengthLabel(wave.targetBeats))
                            : wave.defining ? qsTr("free") : ""
                        color: Skin.textDim
                        font.pixelSize: Skin.fontS
                        font.family: Skin.monoFamily
                        elide: Text.ElideRight
                    }

                    Meter {
                        visible: root.hasAudio
                        vertical: false
                        showHold: true
                        Layout.preferredWidth: Px.px(64)
                        Layout.alignment: Qt.AlignVCenter
                        position: root.loopLevel
                        hold: root.loopLevelHold
                    }
                }

                HoverHandler { id: stageHover }
                Tip {
                    text: qsTr("What the tape is doing, where the head is in the loop as bar.beat, how long the loop is, and how hot the loop itself runs - apart from whatever is passing through live.")
                    visible: stageHover.hovered
                }
            }

            Item { Layout.preferredWidth: Skin.spacing }

            HeaderButton {
                label: qsTr("COUNT")
                active: root.countIn
                activeColor: Skin.solo
                tip: root.mapping
                     ? qsTr("Tap to bind Count to the next control.")
                     : qsTr("Latch: Rec waits one bar of clicks. Off, Rec starts at once. Turn it off during the count to cancel.")
                onClicked: {
                    if (root.mapping) { root.armParam(root.pCountIn, 0, 1); return }
                    root.countIn = !root.countIn
                    Mixer.setLooperCountIn(root.targetRow, root.targetSlot, root.countIn)
                    if (!root.countIn) {
                        root.countBeats = 0
                        root.recording = Mixer.looperRecording(
                            root.targetRow, root.targetSlot)
                    }
                }
                MapDot { param: root.pCountIn }
            }
            HeaderButton {
                Layout.preferredWidth: Px.px(64)
                label: qsTr("REC")
                activeColor: Skin.arm
                active: root.recording
                tip: root.mapping
                     ? qsTr("Tap to bind Rec to the next control.")
                     : qsTr("Start recording, at the next cycle if quantised. The take closes at Length and keeps looping; press again to stop recording.")
                onClicked: {
                    if (root.mapping) { root.armParam(root.pRecord, 0, 1); return }
                    root.recording = !root.recording
                    Mixer.setLooperRecord(root.targetRow, root.targetSlot, root.recording)
                    root.refreshTransport()
                    root.refreshPosition()
                }
                MapDot { param: root.pRecord }
            }
            HeaderButton {
                Layout.preferredWidth: Px.px(64)
                label: qsTr("PLAY")
                activeColor: Skin.meterLow
                active: root.playing
                tip: root.mapping
                     ? qsTr("Tap to bind Play to the next control.")
                     : qsTr("Play the recorded loop; press again to mute it without losing it.")
                onClicked: {
                    if (root.mapping) { root.armParam(root.pPlay, 0, 1); return }
                    root.playing = !root.playing
                    Mixer.setLooperPlay(root.targetRow, root.targetSlot, root.playing)
                }
                MapDot { param: root.pPlay }
            }
            HeaderButton {
                label: qsTr("CLEAR")
                danger: true
                tip: root.mapping
                     ? qsTr("Tap to bind Clear to the next control.")
                     : qsTr("Throw the loop away and drop Rec.")
                onClicked: {
                    if (root.mapping) { root.armParam(root.pClear, 0, 1); return }
                    Mixer.clearLooper(root.targetRow, root.targetSlot)
                    root.recording = false
                    root.hasAudio = false
                    root.hasLoop = false
                    root.refreshAll()
                }
                MapDot { param: root.pClear }
            }

            Item { Layout.preferredWidth: Skin.spacingS }

            HeaderButton {
                label: qsTr("UNDO")
                enabled: root.canUndo
                tip: qsTr("Peel the last thing you played - the phrase between silences - not the whole Rec pass. Press again from Redo to put it back.")
                onClicked: {
                    Mixer.undoLooper(root.targetRow, root.targetSlot)
                    root.refreshAll()
                }
            }
            HeaderButton {
                label: qsTr("REDO")
                enabled: root.canRedo
                tip: qsTr("Put back what Undo just peeled.")
                onClicked: {
                    Mixer.redoLooper(root.targetRow, root.targetSlot)
                    root.refreshAll()
                }
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
                tip: qsTr("Bind a looper control to a knob or pad. Press MAP, tap Rec, Play, a bar… then turn the control. MAP stays on so the next one can follow.")
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

            // --- the tape, and the rows that shape the next take --------------------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Skin.spacing

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: Skin.radiusL
                    color: Qt.rgba(0, 0, 0, 0.25)
                    border.width: 1
                    border.color: Skin.line

                    // The waveform: trim by dragging its edges, fade by
                    // dragging the small marks just inside them.
                    Item {
                        id: wave
                        anchors.fill: parent
                        anchors.margins: Skin.spacing

                        // A miss on a handle used to fall through the graph
                        // onto the strip behind it. Eat the gesture here; the
                        // handles take over when they actually get the press.
                        HoverHandler { cursorShape: Qt.ArrowCursor }
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

                        readonly property int trimStartX: root.trimStart * wave.width
                        readonly property int trimEndX: root.trimEnd * wave.width
                        // True during the first, still-open pass: audio is
                        // on the tape but there is no closed loop to trim yet.
                        readonly property bool defining: root.hasAudio && !root.hasLoop
                        // How many beats the first pass will close at,
                        // mirroring LooperInstance::unit_beats() - known
                        // from Length even before the loop exists, so the
                        // grid can be drawn ahead of time.
                        readonly property real targetBeats: {
                            if (root.quantize === 6)  // Sync: whatever the target last measured
                                return root.syncTargetRow >= 0
                                    ? Mixer.looperBeats(root.syncTargetRow, root.syncTargetSlot)
                                    : 0
                            if (root.quantize <= 0) return 0
                            if (root.quantize === 1) return 1
                            const kBars = [1, 2, 4, 8]
                            return Mixer.timeNumerator() * kBars[Math.round(root.quantize) - 2]
                        }
                        // The beat count the bar grid should draw against:
                        // the closed loop's own length once it has one,
                        // otherwise the length it is heading for.
                        readonly property real gridBeats: root.hasLoop ? root.loopBeats
                                                           : (wave.defining ? wave.targetBeats : 0)
                        readonly property int gridBars: {
                            const num = Mixer.timeNumerator()
                            return num > 0 ? Math.round(wave.gridBeats / num) : 0
                        }
                        // Fades are capped at half the trim window in the
                        // engine, so a fade fraction of 1.0 walks its handle
                        // exactly to the midpoint.
                        readonly property real halfWindow: (trimEndX - trimStartX) / 2
                        readonly property int fadeInX: trimStartX + root.fadeIn * wave.halfWindow
                        readonly property int fadeOutX: trimEndX - root.fadeOut * wave.halfWindow
                        readonly property color headHue: root.writing ? Skin.arm : Skin.meterLow

                        Rectangle {
                            id: tape
                            anchors.fill: parent
                            radius: Skin.radius
                            color: Skin.slotEmpty
                            border.width: 1
                            border.color: Qt.rgba(root.stateHue.r, root.stateHue.g,
                                                  root.stateHue.b, 0.45)
                            clip: true

                            // The rest line, faint, so an empty tape is still
                            // a tape.
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                height: 1
                                color: Qt.rgba(Skin.accent.r, Skin.accent.g, Skin.accent.b, 0.12)
                            }

                            // Empty: say so, and say what to do about it.
                            Column {
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.verticalCenterOffset: -Px.px(18)
                                visible: !root.hasAudio && root.countBeats <= 0 && !root.armed
                                spacing: Skin.spacingS
                                width: parent.width - 2 * Skin.spacingL
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: qsTr("NOTHING ON THE TAPE")
                                    color: Skin.disabled
                                    font.pixelSize: Skin.fontS
                                    font.bold: true
                                    font.letterSpacing: Px.px(2)
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: qsTr("Press REC, play something, press REC again to close the loop.")
                                    color: Skin.textDim
                                    font.pixelSize: Skin.fontXS
                                    horizontalAlignment: Text.AlignHCenter
                                    width: parent.width
                                    wrapMode: Text.WordWrap
                                }
                            }

                            // The first pass, as it is written: a wash of
                            // Rec's colour behind everything already on the
                            // tape, so the head is seen filling the room.
                            Rectangle {
                                visible: wave.defining && root.writing && root.playPosition >= 0
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: Math.max(0, Math.min(1, root.playPosition)) * parent.width
                                color: Qt.rgba(Skin.arm.r, Skin.arm.g, Skin.arm.b, 0.08)
                            }

                            // The waveform itself, drawn from the middle out
                            // - the usual shape, and one Rectangle per bucket
                            // is what every other bar in this app is already
                            // built from.
                            Row {
                                id: bars
                                visible: root.hasAudio
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                height: parent.height - 2 * Skin.spacingL

                                Repeater {
                                    model: root.peaks

                                    Rectangle {
                                        id: bar
                                        required property real modelData
                                        required property int index
                                        readonly property real peak: Math.min(1, modelData)
                                        // Which overdub pass most recently
                                        // touched this stretch - 0 for the
                                        // base take. Falls back to 0 if the
                                        // layer scan has not landed yet, so
                                        // a bar never flashes an unrelated
                                        // colour for a frame. Named layerId,
                                        // not layer: Item already has a
                                        // FINAL `layer` grouped property.
                                        readonly property int layerId:
                                            bar.index < root.layers.length ? root.layers[bar.index] : 0

                                        width: Math.max(1, bars.width / Math.max(1, root.peaks.length))
                                        height: Math.max(Px.px(2), peak * bars.height)
                                        anchors.verticalCenter: bars.verticalCenter
                                        color: root.layerColor(bar.layerId)
                                        opacity: 0.85
                                    }
                                }
                            }

                            // Dims what trimming leaves out of the loop.
                            Rectangle {
                                visible: root.hasLoop
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: wave.trimStartX
                                color: Skin.background
                                opacity: 0.72
                            }
                            Rectangle {
                                visible: root.hasLoop
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: parent.width - wave.trimEndX
                                color: Skin.background
                                opacity: 0.72
                            }

                            // The fade ramps, as a gradient rather than a
                            // number: opaque where the loop is silent, clear
                            // where it is at full volume.
                            Rectangle {
                                visible: root.hasLoop && root.fadeIn > 0
                                anchors.left: parent.left
                                anchors.leftMargin: wave.trimStartX
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: wave.fadeInX - wave.trimStartX
                                gradient: Gradient {
                                    orientation: Gradient.Horizontal
                                    GradientStop { position: 0.0; color: Skin.background }
                                    GradientStop { position: 1.0; color: "transparent" }
                                }
                                opacity: 0.6
                            }
                            Rectangle {
                                visible: root.hasLoop && root.fadeOut > 0
                                anchors.right: parent.right
                                anchors.rightMargin: parent.width - wave.trimEndX
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: wave.trimEndX - wave.fadeOutX
                                gradient: Gradient {
                                    orientation: Gradient.Horizontal
                                    GradientStop { position: 0.0; color: "transparent" }
                                    GradientStop { position: 1.0; color: Skin.background }
                                }
                                opacity: 0.6
                            }

                            // Beat ticks along the top and bottom edges, and
                            // bar lines the full height, from the length the
                            // close snap settled on - so a 4-bar take reads
                            // as four rooms rather than a long smear. While
                            // the first pass is still open these are drawn
                            // from Length's own grid instead: the room to
                            // work with, laid out before there is anything
                            // closed to measure.
                            Repeater {
                                model: (root.hasLoop || wave.defining)
                                       && wave.gridBeats > 1 && wave.gridBeats <= 64
                                       ? Math.round(wave.gridBeats) - 1 : 0
                                Item {
                                    required property int index
                                    x: (index + 1) / Math.round(wave.gridBeats) * parent.width
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: 1
                                    opacity: wave.defining ? 0.45 : 0.85
                                    Rectangle {
                                        anchors.top: parent.top
                                        width: 1
                                        height: Px.px(6)
                                        color: Skin.border
                                    }
                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        width: 1
                                        height: Px.px(6)
                                        color: Skin.border
                                    }
                                }
                            }
                            Repeater {
                                model: (root.hasLoop || wave.defining) && wave.gridBars > 1
                                       ? wave.gridBars - 1 : 0
                                Rectangle {
                                    required property int index
                                    x: (index + 1) / wave.gridBars * parent.width
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: 1
                                    color: Skin.border
                                    opacity: wave.defining ? 0.45 : 0.85
                                }
                            }

                            // The playhead, with a little light around it:
                            // green going round, red while it writes. -1
                            // while nothing is playing.
                            Item {
                                visible: root.hasAudio && root.playPosition >= 0
                                x: root.playPosition * parent.width - width / 2
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: Px.px(14)
                                Rectangle {
                                    anchors.fill: parent
                                    gradient: Gradient {
                                        orientation: Gradient.Horizontal
                                        GradientStop { position: 0.0; color: "transparent" }
                                        GradientStop { position: 0.5; color: Qt.rgba(wave.headHue.r, wave.headHue.g, wave.headHue.b, 0.35) }
                                        GradientStop { position: 1.0; color: "transparent" }
                                    }
                                }
                                Rectangle {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: Px.px(2)
                                    color: wave.headHue
                                }
                            }

                            // One dot per pass on the tape, in the pass's
                            // colour: the legend for the waveform's colours,
                            // and a count of how deep the stack is.
                            Row {
                                anchors.top: parent.top
                                anchors.left: parent.left
                                anchors.topMargin: Skin.spacingS
                                anchors.leftMargin: Px.px(14)
                                spacing: Skin.spacingXS
                                visible: root.hasLoop && root.layerCount > 1
                                Repeater {
                                    model: Math.min(12, root.layerCount)
                                    Rectangle {
                                        required property int index
                                        width: Px.px(8)
                                        height: Px.px(8)
                                        radius: width / 2
                                        color: root.layerColor(index)
                                        opacity: 0.9
                                    }
                                }
                            }

                            // The sign: one big number and a word, for the
                            // player rather than the engineer. It counts
                            // beats down to every moment that matters -
                            // the count-in, a press landing on the grid,
                            // the loop closing, an overdub ending, the head
                            // coming round to the one - in yellow for what
                            // is about to happen, red for what is being
                            // written, green for the cycle going round.
                            readonly property bool signBig:
                                root.countBeats > 0 || root.armed || root.punchingOut
                                || (wave.defining && root.writing && wave.targetBeats > 0)
                            readonly property string signWord:
                                root.countBeats > 0 ? qsTr("COUNT-IN")
                                : root.armed ? (root.hasLoop ? qsTr("OVERDUB IN") : qsTr("REC IN"))
                                : root.punchingOut ? (root.hasLoop ? qsTr("OVERDUB ENDS IN") : qsTr("LOOP CLOSES IN"))
                                : wave.defining && root.writing && wave.targetBeats > 0 ? qsTr("LOOP CLOSES IN")
                                : root.hasLoop && root.playing && root.loopBeatPos >= 0 ? qsTr("ONE IN")
                                : ""
                            readonly property int signCount:
                                root.countBeats > 0 ? root.countBeats
                                : root.armed || root.punchingOut
                                    ? (root.boundaryIn > 0 ? root.boundaryIn : 1)
                                : wave.defining && root.writing && wave.targetBeats > 0
                                    ? Math.max(1, Math.ceil(wave.targetBeats * (1 - Math.max(0, Math.min(1, root.playPosition))) - 0.02))
                                : root.oneIn
                            readonly property color signHue:
                                root.countBeats > 0 || root.armed || root.punchingOut ? Skin.solo
                                : root.writing ? Skin.arm
                                : Skin.meterLow

                            Column {
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.verticalCenterOffset: tape.signBig ? 0 : -parent.height * 0.28
                                visible: tape.signWord.length > 0
                                spacing: 0
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: tape.signCount
                                    color: tape.signHue
                                    font.pixelSize: tape.signBig ? Skin.fontXL * 4 : Skin.fontXL * 2
                                    font.bold: true
                                    font.family: Skin.monoFamily
                                    style: Text.Outline
                                    styleColor: Qt.rgba(0, 0, 0, 0.6)
                                    opacity: tape.signBig ? 0.55 + 0.45 * stageChip.pulse : 0.8
                                    Behavior on font.pixelSize { NumberAnimation { duration: Skin.medium } }
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: tape.signWord
                                    color: tape.signHue
                                    font.pixelSize: tape.signBig ? Skin.fontS : Skin.fontXS
                                    font.bold: true
                                    font.letterSpacing: Px.px(2)
                                    style: Text.Outline
                                    styleColor: Qt.rgba(0, 0, 0, 0.6)
                                }
                            }

                            // Beat lamps along the bottom edge: one per beat
                            // of the loop, the downbeats taller, lit up to
                            // where the head is. A metronome you can see
                            // from across the room. While the first pass is
                            // open they show the room Length gives it.
                            Row {
                                id: lamps
                                y: parent.height - height - Skin.spacingS
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: Skin.spacingS
                                anchors.rightMargin: Skin.spacingS
                                height: Px.px(10)
                                spacing: Skin.spacingXS
                                readonly property int beats: Math.round(wave.gridBeats)
                                readonly property int lit: root.playPosition >= 0 && lamps.beats > 0
                                                           ? Math.floor(Math.max(0, Math.min(0.999, root.playPosition)) * lamps.beats) : -1
                                visible: lamps.beats > 1 && lamps.beats <= 64
                                         && (root.hasLoop || wave.defining)
                                Repeater {
                                    model: lamps.visible ? lamps.beats : 0
                                    Rectangle {
                                        required property int index
                                        readonly property bool downbeat: index % Math.max(1, Mixer.timeNumerator()) === 0
                                        readonly property bool on: index === lamps.lit
                                        readonly property bool past: index < lamps.lit
                                        y: parent.height - height
                                        width: Math.max(Px.px(2),
                                            (lamps.width - (lamps.beats - 1) * lamps.spacing) / lamps.beats)
                                        height: downbeat ? Px.px(10) : Px.px(6)
                                        radius: Px.px(2)
                                        color: on ? Qt.lighter(wave.headHue, 1.3)
                                             : past ? wave.headHue
                                             : downbeat ? Skin.border : Skin.line
                                        border.width: downbeat && !on && !past ? 1 : 0
                                        border.color: Qt.rgba(wave.headHue.r, wave.headHue.g, wave.headHue.b, 0.5)
                                        opacity: on ? 1 : past ? 0.6 : 0.9
                                    }
                                }
                            }

                            // The flare on the one and on a punch.
                            Rectangle {
                                anchors.fill: parent
                                radius: parent.radius
                                color: root.stateHue
                                opacity: root.flash * 0.18
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

                        // Trim handles: full-height, dragged to wherever the
                        // loop should start and stop, with a tab at the top
                        // that is easier to find than a hairline.
                        component TrimHandle: Item {
                            id: handle
                            required property bool isStart
                            property int atX: 0
                            readonly property bool lit: dragHover.hovered || drag.active

                            x: handle.atX - width / 2
                            width: Px.px(14)
                            height: wave.height

                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: Px.px(4)
                                radius: Skin.radiusS
                                color: handle.lit ? Skin.focus : Skin.accent
                            }
                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.top: parent.top
                                width: parent.width
                                height: Px.px(10)
                                radius: Skin.radiusS
                                color: handle.lit ? Skin.focus : Skin.accent
                            }
                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: Px.px(10)
                                radius: Skin.radiusS
                                color: handle.lit ? Skin.focus : Skin.accent
                            }

                            HoverHandler { id: dragHover; cursorShape: Qt.SizeHorCursor }
                            Tip {
                                text: handle.isStart
                                      ? qsTr("Where the loop starts. Drag it in to trim the head.")
                                      : qsTr("Where the loop ends. Drag it in to trim the tail.")
                                visible: dragHover.hovered && !drag.active
                            }

                            DragHandler {
                                id: drag
                                target: null
                                // Both axes, or a slightly vertical drag is a
                                // better match for the fader sitting under
                                // this popup and steals the grab mid-trim.
                                xAxis.enabled: true
                                yAxis.enabled: true
                                grabPermissions: PointerHandler.CanTakeOverFromAnything
                                                 | PointerHandler.ApprovesTakeOverByNothing
                                onCentroidChanged: if (drag.active) {
                                    const fraction = Math.max(0, Math.min(1,
                                        (handle.x + handle.width / 2 + drag.centroid.position.x
                                         - drag.centroid.pressPosition.x) / wave.width))
                                    if (handle.isStart)
                                        root.trimStart = Math.min(fraction, root.trimEnd - 0.02)
                                    else
                                        root.trimEnd = Math.max(fraction, root.trimStart + 0.02)
                                    Mixer.setLooperTrim(root.targetRow, root.targetSlot,
                                                        root.trimStart, root.trimEnd)
                                }
                            }
                        }

                        TrimHandle {
                            isStart: true
                            atX: wave.trimStartX
                            visible: root.hasLoop
                        }
                        TrimHandle {
                            isStart: false
                            atX: wave.trimEndX
                            visible: root.hasLoop
                        }

                        // Fade handles: small marks that only move between
                        // their own trim edge and the window's midpoint.
                        component FadeHandle: Item {
                            id: fadeHandle
                            required property bool isIn
                            property int atX: 0
                            readonly property bool lit: fadeHover.hovered || fadeDrag.active

                            x: fadeHandle.atX - width / 2
                            anchors.verticalCenter: wave.verticalCenter
                            width: Px.px(22)
                            height: Px.px(22)

                            Rectangle {
                                anchors.centerIn: parent
                                width: Px.px(12)
                                height: Px.px(12)
                                radius: width / 2
                                color: fadeHandle.lit ? Skin.focus : Skin.solo
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: parent.width * 1.8
                                    height: width
                                    radius: width / 2
                                    color: "transparent"
                                    border.width: Px.px(2)
                                    border.color: fadeHandle.lit ? Skin.focus : Skin.solo
                                    opacity: 0.45
                                }
                            }

                            HoverHandler { id: fadeHover; cursorShape: Qt.SizeHorCursor }
                            Tip {
                                text: fadeHandle.isIn
                                      ? qsTr("Fade in. Drag it right and the loop swells from silence each time round.")
                                      : qsTr("Fade out. Drag it left and the loop sinks to silence before the end.")
                                visible: fadeHover.hovered && !fadeDrag.active
                            }

                            DragHandler {
                                id: fadeDrag
                                target: null
                                xAxis.enabled: true
                                yAxis.enabled: true
                                grabPermissions: PointerHandler.CanTakeOverFromAnything
                                                 | PointerHandler.ApprovesTakeOverByNothing
                                onCentroidChanged: if (fadeDrag.active) {
                                    const half = Math.max(1, wave.halfWindow)
                                    const x = fadeHandle.x + fadeHandle.width / 2
                                              + fadeDrag.centroid.position.x
                                              - fadeDrag.centroid.pressPosition.x
                                    if (fadeHandle.isIn) {
                                        root.fadeIn = Math.max(0, Math.min(1,
                                            (x - wave.trimStartX) / half))
                                    } else {
                                        root.fadeOut = Math.max(0, Math.min(1,
                                            (wave.trimEndX - x) / half))
                                    }
                                    Mixer.setLooperFades(root.targetRow, root.targetSlot,
                                                         root.fadeIn, root.fadeOut)
                                }
                            }
                        }

                        FadeHandle {
                            isIn: true
                            atX: wave.fadeInX
                            visible: root.hasLoop
                        }
                        FadeHandle {
                            isIn: false
                            atX: wave.fadeOutX
                            visible: root.hasLoop
                        }
                    }
                }

                // How long the first take is. Rec left down closes on this
                // grid and stays in overdub, so the phrase loops instead of
                // growing a tail of silence that buries it. Punching out
                // early still snaps to the same grid, the way it always did.
                // Reading Sync, the picker for which Looper to follow sits
                // on the same row.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Skin.spacingXS

                    // A plain Item, not one told to fill: a filling child
                    // makes its row fill too, and the row then eats the tape.
                    Item {
                        Layout.preferredWidth: lengthLabel.implicitWidth + Px.px(10)
                        Layout.preferredHeight: Skin.buttonHeight
                        RowLabel {
                            id: lengthLabel
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("LENGTH")
                        }
                        MapDot { param: root.pQuantize }
                    }

                    component QuantizeButton: StripButton {
                        required property real forValue
                        Layout.preferredWidth: Px.px(44)
                        active: root.quantize === forValue
                        onClicked: {
                            if (root.mapping) { root.armParam(root.pQuantize, 0, 6); return }
                            root.quantize = forValue
                            root.setParam(root.pQuantize, forValue)
                        }
                    }

                    QuantizeButton { forValue: 0; label: qsTr("free") }
                    QuantizeButton { forValue: 1; label: qsTr("beat") }
                    QuantizeButton { forValue: 2; label: qsTr("1") }
                    QuantizeButton { forValue: 3; label: qsTr("2") }
                    QuantizeButton { forValue: 4; label: qsTr("4") }
                    QuantizeButton { forValue: 5; label: qsTr("8") }
                    QuantizeButton {
                        forValue: 6
                        label: qsTr("sync")
                        tip: qsTr("Follow another Looper's own length instead of a fixed bar count - pick which one beside it.")
                    }

                    Text {
                        text: qsTr("bars")
                        color: Skin.textDim
                        font.pixelSize: Skin.fontS
                        visible: root.quantize >= 2 && root.quantize <= 5
                    }

                    // Only meaningful once Length reads Sync: which other
                    // Looper to chase, and what it currently measures - a
                    // picker rather than a number, since the whole point is
                    // that this length is not this instance's own to set.
                    RowLabel {
                        visible: root.quantize === 6
                        text: qsTr("TO")
                    }
                    StripButton {
                        id: syncPickButton
                        visible: root.quantize === 6
                        Layout.preferredWidth: Px.px(140)
                        label: root.syncTargetRow >= 0 ? root.syncTargetLabel()
                                                        : qsTr("choose…")
                        tip: qsTr("Pick which other channel's Looper to follow. Its own Length (or free-running length once closed) becomes this one's.")
                        onClicked: root.openSyncMenu(syncPickButton)
                    }
                    Text {
                        visible: root.quantize === 6
                        text: {
                            if (root.syncTargetRow < 0) return qsTr("no target chosen")
                            if (root.syncTargetLabel().length === 0)
                                return qsTr("that Looper is gone - pick another")
                            const beats = Mixer.looperBeats(root.syncTargetRow, root.syncTargetSlot)
                            if (beats <= 0) return qsTr("waiting for it to close a loop…")
                            return root.lengthLabel(beats)
                        }
                        color: Skin.textDim
                        font.pixelSize: Skin.fontXS
                        font.family: Skin.monoFamily
                    }

                    Item { Layout.fillWidth: true }
                }

                // How the tape plays, and how fast it runs.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Skin.spacingXS

                    RowLabel { text: qsTr("TAPE") }
                    Item { Layout.preferredWidth: Skin.spacingXS }

                    StripButton {
                        Layout.preferredWidth: Px.px(56)
                        label: qsTr("REV")
                        active: root.reverse
                        activeColor: Skin.solo
                        tip: root.mapping
                             ? qsTr("Tap to bind Reverse to the next control.")
                             : qsTr("Play the loop backwards. Rec on top writes in that direction too.")
                        onClicked: {
                            if (root.mapping) { root.armParam(root.pReverse, 0, 1); return }
                            root.reverse = !root.reverse
                            root.setParam(root.pReverse, root.reverse ? 1 : 0)
                        }
                        MapDot { param: root.pReverse }
                    }
                    StripButton {
                        Layout.preferredWidth: Px.px(56)
                        label: qsTr("ONCE")
                        active: root.once
                        tip: root.mapping
                             ? qsTr("Tap to bind Once to the next control.")
                             : qsTr("Stop after this pass. Reverse once walks back to the start and rests.")
                        onClicked: {
                            if (root.mapping) { root.armParam(root.pOnce, 0, 1); return }
                            root.once = !root.once
                            root.setParam(root.pOnce, root.once ? 1 : 0)
                        }
                        MapDot { param: root.pOnce }
                    }
                    StripButton {
                        Layout.preferredWidth: Px.px(72)
                        label: qsTr("REPLACE")
                        active: root.replace
                        activeColor: Skin.arm
                        tip: root.mapping
                             ? qsTr("Tap to bind Replace to the next control.")
                             : qsTr("Next Rec overwrites the tape instead of stacking a layer.")
                        onClicked: {
                            if (root.mapping) { root.armParam(root.pReplace, 0, 1); return }
                            root.replace = !root.replace
                            root.setParam(root.pReplace, root.replace ? 1 : 0)
                        }
                        MapDot { param: root.pReplace }
                    }
                    StripButton {
                        Layout.preferredWidth: Px.px(56)
                        label: qsTr("MULT")
                        enabled: root.canMultiply
                        tip: qsTr("Double the loop: a copy of itself, so the next take can fill the new half.")
                        onClicked: {
                            Mixer.multiplyLooper(root.targetRow, root.targetSlot)
                            root.refreshAll()
                        }
                    }

                    Item { Layout.fillWidth: true }

                    RowLabel { text: qsTr("SPEED") }
                    Item { Layout.preferredWidth: Skin.spacingXS }

                    component SpeedButton: StripButton {
                        required property real forValue
                        Layout.preferredWidth: Px.px(44)
                        active: Math.abs(root.speed - forValue) < 0.01
                        activeColor: root.hueSpeed
                        onClicked: {
                            if (root.mapping) { root.armParam(root.pSpeed, 0.25, 4); return }
                            root.speed = forValue
                            root.setParam(root.pSpeed, forValue)
                        }
                    }
                    SpeedButton {
                        forValue: 0.5
                        label: qsTr("½")
                        tip: qsTr("Half speed. An octave down, twice as long to walk the tape.")
                    }
                    SpeedButton {
                        forValue: 1
                        label: qsTr("×1")
                        tip: qsTr("Original speed. Pitch still sits on top.")
                    }
                    SpeedButton {
                        forValue: 2
                        label: qsTr("×2")
                        tip: qsTr("Double speed. An octave up, the cycle flies.")
                    }
                }
            }

            // --- the rides and the weather ------------------------------------------
            // A layout fills by default; this one is told not to, or it
            // takes the tape's room.
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
                        absolute: false
                        label: qsTr("FEEDBACK")
                        hue: root.hueFeedback
                        value: root.feedback
                        readout: Math.round(root.feedback * 100)
                        mapped: root.isMapped(root.pFeedback)
                        waiting: root.waitingParam === root.pFeedback
                        mapping: root.mapping
                        tip: qsTr("How much of the old stack survives each pass. Full, it stacks forever; lower it and the take fades under what you play next. Drags from where it is, never jumps - a slip here mid-take is the take gone.")
                        onEdited: v => {
                            root.feedback = v
                            root.setParam(root.pFeedback, v)
                        }
                        onArmed: root.armParam(root.pFeedback, 0, 1)
                    }

                    ParamBar {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        big: true
                        label: qsTr("GAIN")
                        hue: root.hueGain
                        value: root.gain / 2.0
                        readout: root.gain.toFixed(2)
                        mapped: root.isMapped(root.pGain)
                        waiting: root.waitingParam === root.pGain
                        mapping: root.mapping
                        tip: qsTr("How loud the loop sits under the live signal, 0 to 2×. The live signal is untouched.")
                        onEdited: v => {
                            root.gain = v * 2.0
                            root.setParam(root.pGain, root.gain)
                        }
                        onArmed: root.armParam(root.pGain, 0, 2)
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Px.px(150)
                    columns: 3
                    columnSpacing: Skin.spacingS
                    rowSpacing: Skin.spacingS

                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("PITCH"); hue: root.huePitch
                        bipolar: true
                        value: (root.pitch + 12) / 24
                        readout: (root.pitch >= 0 ? "+" : "−") + Math.abs(root.pitch).toFixed(1)
                        step: 1 / 24
                        fineStep: 0.1 / 24
                        mapped: root.isMapped(root.pPitch)
                        waiting: root.waitingParam === root.pPitch
                        mapping: root.mapping
                        tip: qsTr("The loop's pitch, an octave either way in semitones. The live signal is untouched. Wheel moves a semitone, Shift+wheel a tenth.")
                        onEdited: v => {
                            root.pitch = v * 24 - 12
                            root.setParam(root.pPitch, root.pitch)
                        }
                        onArmed: root.armParam(root.pPitch, -12, 12)
                    }
                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("SPEED"); hue: root.hueSpeed
                        bipolar: true
                        // ¼×..4× is a four-octave span, so the bar reads in
                        // octaves rather than linearly - unity sits at the
                        // middle the same way Pitch centres on 0.
                        value: (Math.log2(root.speed) + 2) / 4
                        readout: "×" + root.speed.toFixed(2)
                        step: 1 / 48
                        fineStep: 1 / 480
                        mapped: root.isMapped(root.pSpeed)
                        waiting: root.waitingParam === root.pSpeed
                        mapping: root.mapping
                        tip: qsTr("Tape speed, continuous from ¼× to 4×, on top of Pitch. The ½ ×1 ×2 buttons under the tape snap to an exact octave.")
                        onEdited: v => {
                            root.speed = Math.pow(2, v * 4 - 2)
                            root.setParam(root.pSpeed, root.speed)
                        }
                        onArmed: root.armParam(root.pSpeed, 0.25, 4)
                    }
                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("TONE"); hue: root.hueTone
                        value: root.tone
                        readout: Math.round(root.tone * 100)
                        mapped: root.isMapped(root.pTone)
                        waiting: root.waitingParam === root.pTone
                        mapping: root.mapping
                        tip: qsTr("Darker cuts the highs on the loop; at the top it is left open.")
                        onEdited: v => {
                            root.tone = v
                            root.setParam(root.pTone, v)
                        }
                        onArmed: root.armParam(root.pTone, 0, 1)
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
                if (root.countBeats > 0)
                    return qsTr("Counting in. Rec starts on the one - play then.")
                if (root.armed)
                    return root.hasLoop
                        ? qsTr("Armed. The overdub starts on the next %1 - play then.").arg(root.quantize === 1 ? qsTr("beat") : qsTr("bar"))
                        : qsTr("Armed. The tape starts on the next %1 - play then.").arg(root.quantize === 1 ? qsTr("beat") : qsTr("bar"))
                if (root.punchingOut)
                    return root.hasLoop
                        ? qsTr("Still writing until the next %1. Then the loop plays clean.").arg(root.quantize === 1 ? qsTr("beat") : qsTr("bar"))
                        : qsTr("Still writing until the next %1. The loop closes there and starts over.").arg(root.quantize === 1 ? qsTr("beat") : qsTr("bar"))
                const num = Mixer.timeNumerator()
                if (root.hasLoop) {
                    const length = root.lengthLabel(root.loopBeats)
                    if (root.writing)
                        return qsTr("Writing on top. Rec again and it stops on the next %1.").arg(root.quantize === 1 ? qsTr("beat") : qsTr("bar"))
                    return length.length > 0
                        ? qsTr("This take is %1. The lamps are its beats; the number is beats to the one. Rec again to stack a layer.").arg(length)
                        : qsTr("Trim with the tall handles, fade with the round ones just inside them. Rec again to stack a layer.")
                }
                if (wave.defining) {
                    // Still on the first, open pass: say how much room
                    // Length gives it and where the write head sits in that
                    // room, since there is no closed loop yet for the bar
                    // grid or handles to measure against.
                    if (wave.targetBeats <= 0)
                        return qsTr("Recording, free length. Press Rec again to close the loop right there.")
                    const totalBars = num > 0 ? wave.targetBeats / num : 0
                    const done = Math.max(0, Math.min(1, root.playPosition))
                    if (totalBars >= 0.95 && Math.abs(totalBars - Math.round(totalBars)) < 0.08) {
                        const bars = Math.round(totalBars)
                        const atBar = Math.min(bars, Math.floor(done * bars) + 1)
                        return qsTr("Recording bar %1 of %2. It closes on its own there.")
                                  .arg(atBar).arg(bars)
                    }
                    return qsTr("Recording. It closes on its own at %1 beat(s).")
                              .arg(wave.targetBeats)
                }
                return qsTr("Press Rec, play, press Rec again to close the loop. The tape keeps going with this window closed.")
            }
            color: root.mapping || Mixer.learning ? Skin.solo
                 : root.countBeats > 0 ? Skin.solo : Skin.textDim
            font.pixelSize: Skin.fontXS
            wrapMode: Text.WordWrap
        }
    }
}
