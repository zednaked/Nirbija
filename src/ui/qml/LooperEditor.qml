pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// The Looper's own editor: a waveform to trim and fade by dragging, instead
// of the generic per-parameter sliders every other insert without an editor
// gets. Record, play, clear, quantise and gain are still numbers underneath -
// LooperInstance keeps them reachable by id for MIDI-learn - but nothing here
// asks to be read as one.
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
    property bool recording: false
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
    readonly property string stageLabel:
        root.countBeats > 0 ? qsTr("%1").arg(root.countBeats)
        : root.recording
            ? (root.hasLoop
                   ? (root.replace ? qsTr("replacing…") : qsTr("overdubbing…"))
                   : qsTr("recording…"))
            : !root.hasAudio ? qsTr("empty")
            : !root.playing ? qsTr("stopped")
            : root.reverse && root.once ? qsTr("reverse once")
            : root.reverse ? qsTr("reverse")
            : root.once ? qsTr("once")
            : qsTr("playing")

    // Widened from 620: the header row's buttons were already close to that
    // edge, and the loop-level Meter added to it needs room the old width
    // did not have - without it, Undo/Redo got pushed past the popup's own
    // background and drew over whatever was behind it.
    width: Px.px(720)
    height: Px.px(518)
    // A row that outgrows this width again should look cramped, not spill
    // buttons out past the panel and over the mixer behind it.
    clip: true
    modal: true
    anchors.centerIn: Overlay.overlay
    padding: Skin.spacingL
    closePolicy: (root.mapping || Mixer.learning)
                 ? Popup.NoAutoClose
                 : Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // The default modal dimmer is a MouseArea. Pointer handlers on the
    // mixer (faders, strip hover, the sideways flick) do not care about
    // MouseAreas and keep seeing the pointer through the glass. A handler
    // on the dimmer is what actually stops a drag on the waveform from
    // grabbing a fader that happens to sit underneath.
    Overlay.modal: Rectangle {
        color: Qt.rgba(0, 0, 0, 0.45)

        HoverHandler {}
        TapHandler {
            onTapped: {
                if (root.mapping || Mixer.learning) return
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

        // Same leak, inside the popup: empty chrome is not a handler, so a
        // press on the padding or the graph falls through onto the strip.
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

    function openFor(row, slot) {
        root.targetRow = row
        root.targetSlot = slot
        root.refreshAll()
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

    SlotMenu {
        id: syncMenu
    }

    component MapDot: Rectangle {
        required property int param
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 3
        width: Px.px(6)
        height: Px.px(6)
        radius: width / 2
        z: 2
        visible: root.isMapped(param)
        color: root.waitingParam === param ? Skin.solo : Skin.focus
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
            if (p.id === 3) root.quantize = p.value
            else if (p.id === 4) root.gain = p.value
            else if (p.id === 5) root.pitch = p.value
            else if (p.id === 6) root.tone = p.value
            else if (p.id === 7) root.reverse = p.value >= 0.5
            else if (p.id === 8) root.feedback = p.value
            else if (p.id === 9) root.replace = p.value >= 0.5
            else if (p.id === 10) root.once = p.value >= 0.5
            else if (p.id === 11) root.speed = p.value
            else if (p.id === 12) root.countIn = p.value >= 0.5
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
        root.playPosition = Mixer.looperPosition(root.targetRow, root.targetSlot)
        root.countBeats = Mixer.looperCountBeats(root.targetRow, root.targetSlot)
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
    Timer {
        id: positionTimer
        interval: 66
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

        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingS

            Text {
                text: qsTr("Looper")
                color: Skin.text
                font.pixelSize: Skin.fontL
                font.bold: true
            }

            Text {
                text: root.waitingParam >= 0
                      ? qsTr("turn a knob…")
                      : root.mapping
                        ? qsTr("tap a control")
                        : root.stageLabel
                color: root.mapping || Mixer.learning ? Skin.solo
                     : root.countBeats > 0 ? Skin.solo
                     : root.recording ? Skin.arm : Skin.textDim
                font.pixelSize: Skin.fontS
                font.bold: root.countBeats > 0 || root.mapping
            }

            // The loop's own signal, separate from the channel meter beside
            // it in the mixer - that one is the loop plus whatever is
            // passing through live, which does not say whether the stack
            // itself is getting hot.
            Meter {
                visible: root.hasAudio
                vertical: false
                showHold: true
                Layout.preferredWidth: Px.px(72)
                Layout.alignment: Qt.AlignVCenter
                position: root.loopLevel
                hold: root.loopLevelHold
            }

            Item { Layout.fillWidth: true }

            StripButton {
                Layout.preferredWidth: Px.px(48)
                label: qsTr("MAP")
                active: root.mapping || Mixer.learning
                activeColor: Skin.solo
                tip: qsTr("Bind a looper control to a knob or pad. Press MAP, tap Rec, Play, Feedback… then turn the control. MAP stays on so the next one can follow.")
                onClicked: {
                    if (root.mapping || Mixer.learning) root.stopMapping()
                    else {
                        root.mapping = true
                        root.waitingParam = -1
                    }
                }
            }
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Count")
                active: root.countIn
                activeColor: Skin.solo
                tip: root.mapping
                     ? qsTr("Tap to bind Count to the next control.")
                     : qsTr("Latch: Rec waits one bar of clicks. Off, Rec starts at once. Turn it off during the count to cancel.")
                onClicked: {
                    if (root.mapping) { root.armParam(12, 0, 1); return }
                    root.countIn = !root.countIn
                    Mixer.setLooperCountIn(root.targetRow, root.targetSlot, root.countIn)
                    if (!root.countIn) {
                        root.countBeats = 0
                        root.recording = Mixer.looperRecording(
                            root.targetRow, root.targetSlot)
                    }
                }
                MapDot { param: 12 }
            }
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Rec")
                activeColor: Skin.arm
                active: root.recording
                tip: root.mapping
                     ? qsTr("Tap to bind Rec to the next control.")
                     : qsTr("Start recording, at the next cycle if quantised. The take closes at Length and keeps looping; press again to stop recording.")
                onClicked: {
                    if (root.mapping) { root.armParam(0, 0, 1); return }
                    root.recording = !root.recording
                    Mixer.setLooperRecord(root.targetRow, root.targetSlot, root.recording)
                    root.refreshTransport()
                }
                MapDot { param: 0 }
            }
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Play")
                activeColor: Skin.meterLow
                active: root.playing
                tip: root.mapping
                     ? qsTr("Tap to bind Play to the next control.")
                     : qsTr("Play the recorded loop; press again to mute it without losing it.")
                onClicked: {
                    if (root.mapping) { root.armParam(1, 0, 1); return }
                    root.playing = !root.playing
                    Mixer.setLooperPlay(root.targetRow, root.targetSlot, root.playing)
                }
                MapDot { param: 1 }
            }
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Clear")
                danger: true
                tip: root.mapping
                     ? qsTr("Tap to bind Clear to the next control.")
                     : qsTr("Throw the loop away and drop Rec.")
                onClicked: {
                    if (root.mapping) { root.armParam(2, 0, 1); return }
                    Mixer.clearLooper(root.targetRow, root.targetSlot)
                    root.recording = false
                    root.hasAudio = false
                    root.hasLoop = false
                    root.refreshAll()
                }
                MapDot { param: 2 }
            }
            StripButton {
                Layout.preferredWidth: Px.px(76)
                label: qsTr("Undo")
                enabled: root.canUndo
                tip: qsTr("Peel the last thing you played — the phrase between silences — not the whole Rec pass. Press again from Redo to put it back.")
                onClicked: {
                    Mixer.undoLooper(root.targetRow, root.targetSlot)
                    root.refreshAll()
                }
            }
            StripButton {
                Layout.preferredWidth: Px.px(76)
                label: qsTr("Redo")
                enabled: root.canRedo
                tip: qsTr("Put back what Undo just peeled.")
                onClicked: {
                    Mixer.redoLooper(root.targetRow, root.targetSlot)
                    root.refreshAll()
                }
            }
        }

        // --- the waveform: trim by dragging its edges, fade by dragging the
        // small marks just inside them -------------------------------------
        Item {
            id: wave
            Layout.fillWidth: true
            Layout.fillHeight: true

            // A miss on a handle used to fall through the graph onto the
            // strip behind it. Eat the gesture here; the handles take over
            // when they actually get the press.
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
            // True during the first, still-open pass: audio is on the tape
            // but there is no closed loop to trim yet.
            readonly property bool defining: root.hasAudio && !root.hasLoop
            // How many beats the first pass will close at, mirroring
            // LooperInstance::unit_beats() - known from Length even before
            // the loop exists, so the grid can be drawn ahead of time.
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
            // The beat count the bar grid should draw against: the closed
            // loop's own length once it has one, otherwise the length it is
            // heading for.
            readonly property real gridBeats: root.hasLoop ? root.loopBeats
                                               : (wave.defining ? wave.targetBeats : 0)
            // Fades are capped at half the trim window in the engine, so a
            // fade fraction of 1.0 walks its handle exactly to the midpoint.
            readonly property real halfWindow: (trimEndX - trimStartX) / 2
            readonly property int fadeInX: trimStartX + root.fadeIn * wave.halfWindow
            readonly property int fadeOutX: trimEndX - root.fadeOut * wave.halfWindow

            Rectangle {
                anchors.fill: parent
                radius: Skin.radius
                color: Skin.slotEmpty
                border.width: 1
                border.color: Skin.border
                clip: true

                Text {
                    anchors.centerIn: parent
                    visible: !root.hasAudio
                    text: qsTr("Nothing recorded yet. Rec, play something, Rec again to close the loop.")
                    color: Skin.disabled
                    font.pixelSize: Skin.font
                    horizontalAlignment: Text.AlignHCenter
                    width: parent.width - 2 * Skin.spacingL
                    wrapMode: Text.WordWrap
                }

                // The waveform itself, drawn from the middle out - the usual
                // shape, and one Rectangle per bucket is what every other bar
                // in this app is already built from.
                Row {
                    id: bars
                    visible: root.hasAudio
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    height: parent.height - 2 * Skin.spacingS

                    Repeater {
                        model: root.peaks

                        Rectangle {
                            id: bar
                            required property real modelData
                            required property int index
                            readonly property real peak: Math.min(1, modelData)
                            // Which overdub pass most recently touched this
                            // stretch - 0 for the base take. Falls back to 0
                            // if the layer scan has not landed yet, so a bar
                            // never flashes an unrelated colour for a frame.
                            // Named layerId, not layer: Item already has a
                            // FINAL `layer` grouped property (layer.enabled,
                            // layer.effect…) that this would otherwise hide.
                            readonly property int layerId:
                                bar.index < root.layers.length ? root.layers[bar.index] : 0

                            width: Math.max(1, bars.width / Math.max(1, root.peaks.length))
                            height: Math.max(Px.px(2), peak * bars.height)
                            anchors.verticalCenter: bars.verticalCenter
                            color: root.layerColor(bar.layerId)
                            opacity: 0.8
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

                // The fade ramps, as a gradient rather than a number: opaque
                // where the loop is silent, clear where it is at full volume.
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

                // Bar lines from the length the close snap settled on, so a
                // 4-bar take reads as four rooms rather than a long smear.
                // While the first pass is still open these are drawn from
                // Length's own grid instead - the room to work with, laid
                // out before there is anything closed to measure.
                Repeater {
                    model: {
                        const num = Mixer.timeNumerator()
                        const bars = num > 0 ? Math.round(wave.gridBeats / num) : 0
                        return (root.hasLoop || wave.defining) && bars > 1 ? bars - 1 : 0
                    }
                    Rectangle {
                        required property int index
                        readonly property int bars: Math.round(wave.gridBeats /
                                                               Mixer.timeNumerator())
                        x: (index + 1) / bars * parent.width
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 1
                        color: Skin.border
                        opacity: wave.defining ? 0.45 : 0.85
                    }
                }

                // The playhead. -1 while nothing is playing.
                Rectangle {
                    visible: root.hasAudio && root.playPosition >= 0
                    x: root.playPosition * parent.width - width / 2
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: Px.px(2)
                    color: Skin.meterLow
                }
            }

            // Trim handles: full-height, dragged to wherever the loop should
            // start and stop.
            component TrimHandle: Rectangle {
                id: handle
                required property bool isStart
                property int atX: 0

                x: handle.atX - width / 2
                width: Px.px(6)
                height: wave.height
                radius: Skin.radiusS
                color: dragHover.hovered || drag.active ? Skin.focus : Skin.accent

                HoverHandler { id: dragHover; cursorShape: Qt.SizeHorCursor }

                DragHandler {
                    id: drag
                    target: null
                    // Both axes, or a slightly vertical drag is a better
                    // match for the fader sitting under this popup and
                    // steals the grab mid-trim.
                    xAxis.enabled: true
                    yAxis.enabled: true
                    grabPermissions: PointerHandler.CanTakeOverFromAnything
                                     | PointerHandler.ApprovesTakeOverByNothing
                    onCentroidChanged: if (drag.active) {
                        const fraction = Math.max(0, Math.min(1,
                            (handle.x + width / 2 + drag.centroid.position.x
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

            // Fade handles: small marks that only move between their own
            // trim edge and the window's midpoint.
            component FadeHandle: Rectangle {
                id: fadeHandle
                required property bool isIn
                property int atX: 0

                x: fadeHandle.atX - width / 2
                anchors.verticalCenter: wave.verticalCenter
                width: Px.px(10)
                height: Px.px(10)
                radius: width / 2
                color: fadeHover.hovered || fadeDrag.active ? Skin.focus : Skin.solo

                HoverHandler { id: fadeHover; cursorShape: Qt.SizeHorCursor }

                DragHandler {
                    id: fadeDrag
                    target: null
                    xAxis.enabled: true
                    yAxis.enabled: true
                    grabPermissions: PointerHandler.CanTakeOverFromAnything
                                     | PointerHandler.ApprovesTakeOverByNothing
                    onCentroidChanged: if (fadeDrag.active) {
                        const half = Math.max(1, wave.halfWindow)
                        const x = fadeHandle.x + width / 2
                                  + fadeDrag.centroid.position.x
                                  - fadeDrag.centroid.pressPosition.x
                        if (fadeHandle.isIn) {
                            const frac = Math.max(0, Math.min(1,
                                (x - wave.trimStartX) / half))
                            root.fadeIn = frac
                        } else {
                            const frac = Math.max(0, Math.min(1,
                                (wave.trimEndX - x) / half))
                            root.fadeOut = frac
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

        Text {
            Layout.fillWidth: true
            visible: root.hasLoop || wave.defining
            text: {
                const num = Mixer.timeNumerator()
                if (root.hasLoop) {
                    const bars = num > 0 ? root.loopBeats / num : 0
                    const length = bars >= 0.95 && Math.abs(bars - Math.round(bars)) < 0.08
                        ? qsTr("%1 bars").arg(Math.round(bars))
                        : root.loopBeats > 0
                            ? qsTr("%1 beats").arg(root.loopBeats.toFixed(1))
                            : ""
                    return length.length > 0
                        ? qsTr("This take is %1. Trim the tall handles, fade the round ones.")
                              .arg(length)
                        : qsTr("Drag the tall handles to trim, the round ones just inside them to fade in and out.")
                }
                // Still on the first, open pass: say how much room Length
                // gives it and where the write head sits in that room, since
                // there is no closed loop yet for the bar grid or handles to
                // measure against.
                if (wave.targetBeats <= 0)
                    return qsTr("Recording, free length — press Rec again whenever you want to close the loop.")
                const totalBars = num > 0 ? wave.targetBeats / num : 0
                const done = Math.max(0, Math.min(1, root.playPosition))
                if (totalBars >= 0.95 && Math.abs(totalBars - Math.round(totalBars)) < 0.08) {
                    const bars = Math.round(totalBars)
                    const atBar = Math.min(bars, Math.floor(done * bars) + 1)
                    return qsTr("Recording bar %1 of %2 — closes on its own there.")
                              .arg(atBar).arg(bars)
                }
                return qsTr("Recording — closes on its own at %1 beat(s).")
                          .arg(wave.targetBeats)
            }
            color: Skin.textDim
            font.pixelSize: Skin.fontXS
        }

        // How long the first take is. Rec left down closes on this grid and
        // stays in overdub, so the phrase loops instead of growing a tail of
        // silence that buries it. Punching out early still snaps to the same
        // grid, the way it always did.
        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingXS

            Text {
                text: qsTr("Length")
                color: Skin.textDim
                font.pixelSize: Skin.fontS
            }

            component QuantizeButton: StripButton {
                required property real forValue
                Layout.preferredWidth: Px.px(44)
                active: root.quantize === forValue
                onClicked: {
                    if (root.mapping) { root.armParam(3, 0, 6); return }
                    root.quantize = forValue
                    Mixer.setInsertParameter(root.targetRow, root.targetSlot,
                                             3, forValue)
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
                tip: qsTr("Follow another Looper's own length instead of a fixed bar count - pick which one below.")
            }

            Text {
                text: qsTr("bars")
                color: Skin.textDim
                font.pixelSize: Skin.fontS
                visible: root.quantize >= 2 && root.quantize <= 5
            }

            Item { Layout.fillWidth: true }
        }

        // Only meaningful once Length reads Sync: which other Looper to
        // chase, and what it currently measures - a picker rather than a
        // number, since the whole point is that this length is not this
        // instance's own to set.
        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingXS
            visible: root.quantize === 6

            Text {
                text: qsTr("Sync to")
                color: Skin.textDim
                font.pixelSize: Skin.fontS
            }

            StripButton {
                id: syncPickButton
                Layout.preferredWidth: Px.px(150)
                label: root.syncTargetRow >= 0 ? root.syncTargetLabel()
                                                : qsTr("choose…")
                tip: qsTr("Pick which other channel's Looper to follow. Its own Length (or free-running length once closed) becomes this one's.")
                onClicked: root.openSyncMenu(syncPickButton)
            }

            Text {
                text: {
                    if (root.syncTargetRow < 0) return qsTr("no target chosen")
                    if (root.syncTargetLabel().length === 0)
                        return qsTr("that Looper is gone - pick another")
                    const beats = Mixer.looperBeats(root.syncTargetRow, root.syncTargetSlot)
                    if (beats <= 0) return qsTr("waiting for it to close a loop…")
                    const num = Mixer.timeNumerator()
                    const bars = num > 0 ? beats / num : 0
                    return bars >= 0.95 && Math.abs(bars - Math.round(bars)) < 0.08
                        ? qsTr("%1 bars").arg(Math.round(bars))
                        : qsTr("%1 beats").arg(beats.toFixed(1))
                }
                color: Skin.textDim
                font.pixelSize: Skin.fontXS
            }

            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingXS

            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Rev")
                active: root.reverse
                activeColor: Skin.solo
                tip: root.mapping
                     ? qsTr("Tap to bind Reverse to the next control.")
                     : qsTr("Play the loop backwards. Rec on top writes in that direction too.")
                onClicked: {
                    if (root.mapping) { root.armParam(7, 0, 1); return }
                    root.reverse = !root.reverse
                    Mixer.setInsertParameter(root.targetRow, root.targetSlot,
                                             7, root.reverse ? 1 : 0)
                }
                MapDot { param: 7 }
            }
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Once")
                active: root.once
                tip: root.mapping
                     ? qsTr("Tap to bind Once to the next control.")
                     : qsTr("Stop after this pass. Reverse once walks back to the start and rests.")
                onClicked: {
                    if (root.mapping) { root.armParam(10, 0, 1); return }
                    root.once = !root.once
                    Mixer.setInsertParameter(root.targetRow, root.targetSlot,
                                             10, root.once ? 1 : 0)
                }
                MapDot { param: 10 }
            }
            StripButton {
                Layout.preferredWidth: Px.px(64)
                label: qsTr("Replace")
                active: root.replace
                tip: root.mapping
                     ? qsTr("Tap to bind Replace to the next control.")
                     : qsTr("Next Rec overwrites the tape instead of stacking a layer.")
                onClicked: {
                    if (root.mapping) { root.armParam(9, 0, 1); return }
                    root.replace = !root.replace
                    Mixer.setInsertParameter(root.targetRow, root.targetSlot,
                                             9, root.replace ? 1 : 0)
                }
                MapDot { param: 9 }
            }
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Mult")
                enabled: root.canMultiply
                tip: qsTr("Double the loop: a copy of itself, so the next take can fill the new half.")
                onClicked: {
                    Mixer.multiplyLooper(root.targetRow, root.targetSlot)
                    root.refreshAll()
                }
            }

            Item { Layout.fillWidth: true }

            component SpeedButton: StripButton {
                required property real forValue
                Layout.preferredWidth: Px.px(44)
                active: Math.abs(root.speed - forValue) < 0.01
                onClicked: {
                    if (root.mapping) { root.armParam(11, 0.25, 4); return }
                    root.speed = forValue
                    Mixer.setInsertParameter(root.targetRow, root.targetSlot,
                                             11, forValue)
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

        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacing

            ValueTrack {
                Layout.fillWidth: true
                label: qsTr("Feedback")
                valueText: Math.round(root.feedback * 100) + "%"
                value: root.feedback
                absolute: false
                pickOnly: root.mapping
                fillColor: root.waitingParam === 8 ? Skin.solo : Skin.accent
                tip: qsTr("How much of the old layer survives an overdub. 100% stacks forever; drag down only if you want the take to fade. A tap on the word is not a jump. Right-click to bind.")
                onMoved: v => {
                    root.feedback = v
                    Mixer.setInsertParameter(root.targetRow, root.targetSlot, 8, v)
                }
                onPicked: root.armParam(8, 0, 1)
                onMenuRequested: root.armParam(8, 0, 1)
            }

            ValueTrack {
                Layout.fillWidth: true
                label: qsTr("Pitch")
                valueText: (root.pitch >= 0 ? "+" : "") + root.pitch.toFixed(1)
                value: (root.pitch + 12) / 24
                pickOnly: root.mapping
                fillColor: root.waitingParam === 5 ? Skin.solo : Skin.accent
                tip: qsTr("Loop pitch in semitones, −12 to +12. Does not change the live input. Right-click to bind.")
                onMoved: v => {
                    root.pitch = v * 24 - 12
                    Mixer.setInsertParameter(root.targetRow, root.targetSlot,
                                             5, root.pitch)
                }
                onPicked: root.armParam(5, -12, 12)
                onMenuRequested: root.armParam(5, -12, 12)
            }

            ValueTrack {
                Layout.fillWidth: true
                label: qsTr("Speed")
                // ¼×..4× is a four-octave span, so the track reads in
                // octaves rather than linearly - unity speed sits at the
                // middle the same way Pitch centers on 0, instead of
                // hiding in the first fifth of the travel.
                valueText: "×" + root.speed.toFixed(2)
                value: (Math.log2(root.speed) + 2) / 4
                pickOnly: root.mapping
                fillColor: root.waitingParam === 11 ? Skin.solo : Skin.accent
                tip: qsTr("Tape speed, continuous from ¼× to 4×, stacked on top of Pitch above. The ½/×1/×2 buttons below still snap to an exact octave. Right-click to bind.")
                onMoved: v => {
                    root.speed = Math.pow(2, v * 4 - 2)
                    Mixer.setInsertParameter(root.targetRow, root.targetSlot,
                                             11, root.speed)
                }
                onPicked: root.armParam(11, 0.25, 4)
                onMenuRequested: root.armParam(11, 0.25, 4)
            }

            ValueTrack {
                Layout.fillWidth: true
                label: qsTr("Tone")
                valueText: Math.round(root.tone * 100) + "%"
                value: root.tone
                pickOnly: root.mapping
                fillColor: root.waitingParam === 6 ? Skin.solo : Skin.accent
                tip: qsTr("Darker cuts the highs on the loop; 100% leaves it open. Right-click to bind.")
                onMoved: v => {
                    root.tone = v
                    Mixer.setInsertParameter(root.targetRow, root.targetSlot, 6, v)
                }
                onPicked: root.armParam(6, 0, 1)
                onMenuRequested: root.armParam(6, 0, 1)
            }

            ValueTrack {
                Layout.fillWidth: true
                label: qsTr("Gain")
                valueText: root.gain.toFixed(2)
                value: root.gain / 2.0
                pickOnly: root.mapping
                fillColor: root.waitingParam === 4 ? Skin.solo : Skin.accent
                tip: qsTr("Loop gain, 0 to 200%. Right-click to bind.")
                onMoved: v => {
                    root.gain = v * 2.0
                    Mixer.setInsertParameter(root.targetRow, root.targetSlot, 4, root.gain)
                }
                onPicked: root.armParam(4, 0, 2)
                onMenuRequested: root.armParam(4, 0, 2)
            }
        }
    }
}
