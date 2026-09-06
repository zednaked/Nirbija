pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Nirbija

// Sixteen pads and the one in your hand. The pads are the kit, drawn as a
// grid because that is what a finger drummer knows: each one shows its own
// sound as a small waveform, lights when it is hit, and the one in focus
// is drawn large underneath as a tape - trim it by dragging its edges, fade
// it by dragging the small marks just inside them, and watch the head cross
// it when the pad plays. Rec from the strip lands on the focused pad, so
// the tape is also where a take is watched arriving. The rides on the right
// are the focused pad's own volume and the kit's gain, with pitch and pan as
// the smaller weather below them.
//
// It reads the way the looper's and the drone's editors do. The window glows
// in the colour of what the sampler is doing - red while it takes, yellow
// while it counts in or waits for the bar, green while a pad sounds - as
// bright as the kit is loud, and a chip in the header says so in a word.
// Not modal: the mixer stays live, same as the looper.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property int focused: 0
    property bool recording: false
    property bool armed: false
    property int recPad: 0
    property real recFill: 0
    property int sounding: 0
    property int hitFlash: 0
    property int lastNote: -1
    property int lastCc: -1
    property real gain: 1
    property int quantize: 0
    property bool countIn: false
    property bool countingIn: false
    property int countInLeft: 0
    // The chip's own level, mapped onto the fader's travel like every other
    // meter in this app - see Mixer.gainToFader().
    property real level: 0
    property real levelHold: 0
    property var pads: []
    property var peaks: []
    // One small waveform per pad, and the pad version each was taken at,
    // so a thumbnail is refetched only when the pad's audio changes hands.
    property var thumbs: []
    property var thumbVersions: []
    property bool positioned: false
    property int heldPad: -1
    property bool mapping: false
    property int waitingPad: -1
    property int waitingParam: -1
    property var mapped: ({})
    // Lit for a moment when Rec punches in or out: the tape's border and a
    // wash over it flare in the state's colour.
    property real flash: 0
    property bool lastRecording: false

    // Mirrors the focused pad's own trim/fade, ahead of the next poll - a
    // drag moves this immediately, the way the looper's waveform does, and
    // refresh() reconciles it once the engine echoes the write back.
    property real trimStart: 0.0
    property real trimEnd: 1.0
    property real fadeIn: 0.0
    property real fadeOut: 0.0

    // Parameter ids, mirrored from SamplerInstance.
    readonly property int pGain: 0
    readonly property int pRecord: 1
    readonly property int pFocus: 2
    readonly property int pQuantize: 3
    readonly property int pClear: 4
    readonly property int pOneShot: 5
    readonly property int pPitch: 6
    readonly property int pVolume: 7
    readonly property int pPan: 8
    readonly property int pNote: 9
    readonly property int pCountIn: 10
    readonly property int pUndo: 11

    readonly property var noteNames: ["C", "C#", "D", "D#", "E", "F",
                                      "F#", "G", "G#", "A", "A#", "B"]

    function padAt(index) {
        const p = root.pads[index]
        return p && typeof p === "object" ? p : ({
            note: 36, name: "", oneShot: true, volume: 1, pan: 0,
            pitch: 0, start: 0, end: 1, fadeIn: 0, fadeOut: 0,
            hasAudio: false, canUndo: false, pos: -1, version: 0
        })
    }

    readonly property var focusedPad: root.padAt(root.focused)
    readonly property string focusedName: root.focusedPad.name || ("P" + (root.focused + 1))
    readonly property bool anyAudio: {
        for (let i = 0; i < root.pads.length; ++i)
            if (root.pads[i].hasAudio === true) return true
        return false
    }
    readonly property bool busy: root.recording || root.armed || root.countingIn

    function noteName(midi) {
        const n = Math.round(midi)
        return root.noteNames[((n % 12) + 12) % 12] + (Math.floor(n / 12) - 1)
    }

    // What the sampler is doing, in a word, and in a colour. The colour is
    // the one the glow, the chip, the tape's border and the playhead all
    // agree on. Yellow is always "about to", red is being written, green is
    // a pad sounding.
    readonly property string stateLabel:
        root.countingIn ? qsTr("COUNT")
        : root.armed ? qsTr("ARMED")
        : root.recording ? qsTr("TAKING")
        : root.sounding !== 0 ? qsTr("PLAYING")
        : !root.anyAudio ? qsTr("EMPTY")
        : qsTr("READY")
    readonly property color stateHue:
        root.countingIn || root.armed ? Skin.solo
        : root.recording ? Skin.arm
        : root.sounding !== 0 ? Skin.meterLow
        : !root.anyAudio ? Skin.accent
        : Skin.textDim

    // The rides and the weather, each in a colour of its own, kept clear of
    // what colour already means in here: Rec's red, Count's yellow, the
    // pads' accent blue, the playhead's green. Gain and Pitch share the
    // looper's hues, since they are the same knobs.
    readonly property color hueVolume: Qt.hsla(0.47, 0.50, 0.58, 1.0)
    readonly property color hueGain: Qt.hsla(0.40, 0.50, 0.58, 1.0)
    readonly property color huePitch: Qt.hsla(0.76, 0.55, 0.68, 1.0)
    readonly property color huePan: Qt.hsla(0.08, 0.70, 0.62, 1.0)

    width: Px.px(900)
    height: Px.px(640)
    // A row that outgrows this width should look cramped, not spill buttons
    // out past the panel and over the mixer behind it.
    clip: true
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
        // the kit is loud. A take keeps a floor so a quiet one still shows
        // the head is writing; counting in and waiting for the bar hold
        // steady in yellow.
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
            opacity: root.countingIn || root.armed ? 0.22
                   : root.recording ? Math.max(0.14, Math.min(0.32, root.level * 0.5))
                   : root.sounding !== 0 ? Math.min(0.32, 0.06 + root.level * 0.5)
                   : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        // Empty chrome is not a handler on its own, so without this a press
        // on the padding falls through onto the strip behind - and doubles
        // as how the popup moves.
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
        root.thumbs = []
        root.thumbVersions = []
        root.refresh()
        root.refreshWave()
        root.refreshMapped()
        Mixer.listenSamplerMidi(row, slot, true)
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
        Mixer.listenSamplerMidi(root.targetRow, root.targetSlot, true)
        poll.start()
        wavePoll.start()
    }
    onClosed: {
        poll.stop()
        wavePoll.stop()
        Mixer.listenSamplerMidi(root.targetRow, root.targetSlot, false)
        root.stopMapping()
        if (root.heldPad >= 0) {
            Mixer.releaseSamplerPad(root.targetRow, root.targetSlot, root.heldPad)
            root.heldPad = -1
        }
    }

    function isMapped(id) {
        return root.mapped[id] === true
    }

    function refreshMapped() {
        const next = {}
        const ids = [root.pGain, root.pRecord, root.pQuantize, root.pClear,
                     root.pOneShot, root.pPitch, root.pVolume, root.pPan,
                     root.pCountIn, root.pUndo]
        for (let i = 0; i < ids.length; ++i)
            next[ids[i]] = Mixer.insertParamMapped(root.targetRow, root.targetSlot,
                                                   ids[i])
        root.mapped = next
    }

    function armParam(id, min, max) {
        Mixer.learnInsertParam(root.targetRow, root.targetSlot, id, min, max)
        root.waitingParam = id
        root.waitingPad = -1
    }

    function armPadNote(index) {
        root.focusPad(index)
        Mixer.learnSamplerPadNote(root.targetRow, root.targetSlot, index)
        root.waitingPad = index
        root.waitingParam = -1
    }

    function stopMapping() {
        Mixer.cancelLearn()
        root.mapping = false
        root.waitingPad = -1
        root.waitingParam = -1
    }

    function focusPad(index) {
        Mixer.setSamplerFocus(root.targetRow, root.targetSlot, index)
        if (root.focused !== index) {
            root.focused = index
            root.refreshWave()
        }
    }

    Connections {
        target: Mixer
        function onLearnChanged() {
            if (!Mixer.learning) {
                root.waitingPad = -1
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
    // drone's and the looper's are, so the row reads as a transport.
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

    function refresh() {
        const snap = Mixer.insertSamplerSnapshot(root.targetRow, root.targetSlot)
        if (!snap || snap.pads === undefined) return
        const wasFocused = root.focused
        root.focused = snap.focused
        root.recording = snap.recording
        root.armed = snap.armed === true
        root.recPad = snap.recPad
        root.recFill = snap.recFill === undefined ? 0 : snap.recFill
        root.gain = snap.gain
        root.quantize = snap.quantize
        root.countIn = snap.countIn
        root.countingIn = snap.countingIn
        root.countInLeft = snap.countInLeft
        root.sounding = snap.sounding
        root.hitFlash = snap.hitFlash
        root.lastNote = snap.lastNote === undefined ? -1 : snap.lastNote
        root.lastCc = snap.lastCc === undefined ? -1 : snap.lastCc
        root.pads = snap.pads
        const level = Mixer.gainToFader(snap.level === undefined ? 0 : snap.level)
        root.level = level
        // Instant rise, steady fall - the same ballistic every other meter
        // in this app uses.
        root.levelHold = Math.max(level, root.levelHold - 0.012)
        // The head punched in or out: the moment the player was waiting for.
        if (root.recording !== root.lastRecording) {
            root.flare()
            root.lastRecording = root.recording
            // A take just closed: the pad has new audio to draw.
            if (!root.recording) root.refreshWave()
        }
        if (wasFocused !== root.focused) root.refreshWave()
        const fp = root.padAt(root.focused)
        root.trimStart = fp.start
        root.trimEnd = fp.end
        root.fadeIn = fp.fadeIn
        root.fadeOut = fp.fadeOut
        if (!nameField.activeFocus)
            nameField.text = fp.name
    }

    // The focused pad's waveform, and the small ones on the pads. The tape
    // is rescanned on its own slower clock - peaks change only on a take, a
    // load or a clear - and while a take is still being written, so the pad
    // can be watched filling.
    function refreshWave() {
        root.peaks = Mixer.samplerWaveform(root.targetRow, root.targetSlot,
                                           root.focused, 160)
        const thumbs = root.thumbs.slice()
        const versions = root.thumbVersions.slice()
        let changed = false
        for (let i = 0; i < 16; ++i) {
            const p = root.padAt(i)
            const stamp = (p.version === undefined ? 0 : p.version) * 2 + (p.hasAudio ? 1 : 0)
            if (versions[i] === stamp) continue
            thumbs[i] = p.hasAudio ? Mixer.samplerWaveform(root.targetRow, root.targetSlot, i, 24) : []
            versions[i] = stamp
            changed = true
        }
        if (changed) {
            root.thumbs = thumbs
            root.thumbVersions = versions
        }
    }

    function writePad(patch) {
        const p = root.padAt(root.focused)
        Mixer.setSamplerPad(root.targetRow, root.targetSlot, root.focused,
                            patch.note !== undefined ? patch.note : p.note,
                            patch.oneShot !== undefined ? patch.oneShot : p.oneShot,
                            patch.volume !== undefined ? patch.volume : p.volume,
                            patch.pan !== undefined ? patch.pan : p.pan,
                            patch.pitch !== undefined ? patch.pitch : p.pitch)
        root.refresh()
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

    Timer {
        id: poll
        interval: 40
        repeat: true
        onTriggered: root.refresh()
    }
    Timer {
        id: wavePoll
        interval: 250
        repeat: true
        onTriggered: {
            // Only the growing take needs the tape rescanned on a clock;
            // everything else is caught by the pad's version.
            if (root.recording && root.recPad === root.focused)
                root.peaks = Mixer.samplerWaveform(root.targetRow, root.targetSlot,
                                                   root.focused, 160)
            root.refreshWave()
        }
    }

    FileDialog {
        id: loadDialog
        title: qsTr("Load a sample")
        nameFilters: [qsTr("Audio files (*.wav *.flac *.ogg *.aiff *.aif)"),
                      qsTr("All files (*)")]
        onAccepted: {
            Mixer.loadSamplerPad(root.targetRow, root.targetSlot,
                                 root.focused, selectedFile)
            root.refresh()
            root.refreshWave()
        }
    }

    FileDialog {
        id: packSaveDialog
        title: qsTr("Save sampler pack as")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Nirbija sampler packs (*.json)")]
        defaultSuffix: "json"
        onAccepted: Mixer.saveSamplerPackTo(root.targetRow, root.targetSlot, selectedFile)
    }

    FileDialog {
        id: packOpenDialog
        title: qsTr("Open sampler pack")
        nameFilters: [qsTr("Nirbija sampler packs (*.json)")]
        onAccepted: {
            Mixer.loadSamplerPackFrom(root.targetRow, root.targetSlot, selectedFile)
            root.refresh()
            root.refreshWave()
        }
    }

    contentItem: ColumnLayout {
        spacing: Skin.spacing

        // --- header ------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingS

            Text {
                text: qsTr("SAMPLER")
                color: Skin.text
                font.pixelSize: Skin.fontL
                font.bold: true
                font.letterSpacing: Px.px(2)
            }

            Item { Layout.preferredWidth: Skin.spacing }

            // The state, on a chip: a dot in the state's colour, the word
            // for it, the pad in hand, the last thing the controller sent,
            // and the kit's own level - separate from the channel meter in
            // the mixer, which is the kit plus whatever passes through live.
            Rectangle {
                id: stageChip
                Layout.preferredWidth: Px.px(320)
                Layout.preferredHeight: Skin.buttonHeight + Px.px(6)
                radius: Skin.radius
                color: Skin.slotEmpty
                border.width: 1
                border.color: Qt.rgba(root.stateHue.r, root.stateHue.g, root.stateHue.b, 0.6)
                property real pulse: 1

                SequentialAnimation on pulse {
                    running: root.busy
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
                        opacity: root.busy ? stageChip.pulse
                               : root.sounding !== 0 ? 1 : 0.5
                    }

                    Text {
                        text: root.countingIn ? root.countInLeft : root.stateLabel
                        color: root.stateHue
                        font.pixelSize: root.countingIn ? Skin.fontXL : Skin.fontL
                        font.bold: true
                        font.letterSpacing: Px.px(1)
                    }

                    // The pad in hand, by name and by key.
                    Text {
                        text: root.focusedName
                        color: Skin.text
                        font.pixelSize: Skin.fontS
                        font.bold: true
                        elide: Text.ElideRight
                        Layout.maximumWidth: Px.px(90)
                    }
                    Text {
                        text: root.noteName(root.focusedPad.note)
                        color: Skin.textDim
                        font.pixelSize: Skin.fontS
                        font.family: Skin.monoFamily
                    }

                    // The last note or CC that reached this chip, whether
                    // or not a pad owns it: for wiring a controller by eye.
                    Text {
                        Layout.fillWidth: true
                        text: root.lastNote >= 0 ? qsTr("note %1").arg(root.lastNote)
                            : root.lastCc >= 0 ? qsTr("CC %1").arg(root.lastCc)
                            : ""
                        color: Skin.focus
                        font.pixelSize: Skin.fontXS
                        font.family: Skin.monoFamily
                        elide: Text.ElideRight
                    }

                    Meter {
                        visible: root.anyAudio || root.recording
                        vertical: false
                        showHold: true
                        Layout.preferredWidth: Px.px(56)
                        Layout.alignment: Qt.AlignVCenter
                        position: root.level
                        hold: root.levelHold
                    }
                }

                HoverHandler { id: stageHover }
                Tip {
                    text: qsTr("What the sampler is doing, which pad is in hand and on which key, the last note or CC the controller sent, and how hot the kit itself runs - apart from whatever is passing through live.")
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
                     : qsTr("Latch: Rec waits one bar of clicks, so the take does not start with the button being pressed. Turn it off during the count to cancel.")
                onClicked: {
                    if (root.mapping) { root.armParam(root.pCountIn, 0, 1); return }
                    Mixer.setSamplerCountIn(root.targetRow, root.targetSlot, !root.countIn)
                    root.countIn = !root.countIn
                }
                MapDot { param: root.pCountIn }
            }
            HeaderButton {
                Layout.preferredWidth: Px.px(64)
                label: qsTr("REC")
                activeColor: Skin.arm
                active: root.recording || root.countingIn || root.armed
                tip: root.mapping
                     ? qsTr("Tap to bind Rec to the next control.")
                     : qsTr("Record the strip's input onto the pad in hand - at the next beat or bar if Rec On says so. Press again to close the take.")
                onClicked: {
                    if (root.mapping) { root.armParam(root.pRecord, 0, 1); return }
                    const on = !(root.recording || root.countingIn || root.armed)
                    Mixer.setSamplerRecord(root.targetRow, root.targetSlot, on)
                    root.refresh()
                }
                MapDot { param: root.pRecord }
            }
            HeaderButton {
                label: qsTr("CLEAR")
                danger: true
                tip: root.mapping
                     ? qsTr("Tap to bind Clear to the next control.")
                     : qsTr("Throw away the pad in hand. Undo brings it back.")
                onClicked: {
                    if (root.mapping) { root.armParam(root.pClear, 0, 1); return }
                    Mixer.clearSamplerPad(root.targetRow, root.targetSlot, root.focused)
                    root.refresh()
                    root.refreshWave()
                }
                MapDot { param: root.pClear }
            }

            Item { Layout.preferredWidth: Skin.spacingS }

            HeaderButton {
                label: qsTr("UNDO")
                enabled: root.mapping || root.focusedPad.canUndo === true
                tip: root.mapping
                     ? qsTr("Tap to bind Undo to the next control.")
                     : qsTr("Swap back to what the pad held before its last Rec, Load or Clear. Press again to swap forward.")
                onClicked: {
                    if (root.mapping) { root.armParam(root.pUndo, 0, 1); return }
                    Mixer.undoSamplerPad(root.targetRow, root.targetSlot, root.focused)
                    root.refresh()
                    root.refreshWave()
                }
                MapDot { param: root.pUndo }
            }
            HeaderButton {
                label: qsTr("LOAD")
                tip: qsTr("A file onto the pad in hand. Rec does the same from the strip.")
                onClicked: loadDialog.open()
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
                tip: qsTr("Bind Rec, a bar, or a pad to the controller. Press MAP, tap what you want, then hit the control. A pad binds its note. MAP stays on so the next one can follow.")
                onClicked: {
                    if (root.mapping || Mixer.learning) root.stopMapping()
                    else {
                        root.mapping = true
                        root.waitingPad = -1
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

            // --- the kit, the tape, and the rows that shape the pad in hand ----------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Skin.spacing

                // The pads, in an inset panel.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: Skin.radiusL
                    color: Qt.rgba(0, 0, 0, 0.25)
                    border.width: 1
                    border.color: Skin.line

                    GridLayout {
                        id: grid
                        anchors.fill: parent
                        anchors.margins: Skin.spacing
                        columns: 4
                        rows: 4
                        columnSpacing: Skin.spacingS
                        rowSpacing: Skin.spacingS

                        Repeater {
                            model: 16

                            Item {
                                id: cell
                                required property int index
                                readonly property var info: root.padAt(cell.index)
                                readonly property bool isFocused: root.focused === cell.index
                                readonly property bool isRecording: root.recording
                                                                    && root.recPad === cell.index
                                readonly property bool isArmed: (root.armed || root.countingIn)
                                                                && root.focused === cell.index
                                readonly property bool isSounding: (root.sounding
                                                                    & (1 << cell.index)) !== 0
                                readonly property bool isHitFlash: (root.hitFlash
                                                                    & (1 << cell.index)) !== 0
                                readonly property bool hasAudio: cell.info.hasAudio === true
                                readonly property bool waiting: root.waitingPad === cell.index
                                readonly property var thumb: {
                                    const t = root.thumbs[cell.index]
                                    return t && t.length !== undefined ? t : []
                                }
                                readonly property real pos: cell.info.pos === undefined ? -1 : cell.info.pos
                                readonly property color hue: cell.isRecording ? Skin.arm
                                                           : cell.isArmed ? Skin.solo
                                                           : Skin.accent
                                property real pulse: 0.4

                                Layout.fillWidth: true
                                Layout.fillHeight: true

                                SequentialAnimation on pulse {
                                    running: cell.isRecording || cell.isArmed || cell.waiting
                                    loops: Animation.Infinite
                                    NumberAnimation {
                                        from: 0.25; to: 0.9; duration: 420
                                        easing.type: Easing.InOutSine
                                    }
                                    NumberAnimation {
                                        from: 0.9; to: 0.25; duration: 420
                                        easing.type: Easing.InOutSine
                                    }
                                }

                                Rectangle {
                                    id: face
                                    anchors.fill: parent
                                    radius: Skin.radius
                                    color: cell.hasAudio ? Skin.slot : Skin.slotEmpty
                                    border.width: (cell.isFocused || cell.isHitFlash || cell.waiting) ? Px.px(2) : 1
                                    border.color: cell.waiting ? Skin.solo
                                                : cell.isRecording ? Skin.arm
                                                : cell.isArmed ? Skin.solo
                                                : cell.isHitFlash ? Skin.focus
                                                : cell.isFocused ? Skin.focus
                                                : cell.isSounding ? Skin.meterLow
                                                : root.mapping ? Qt.rgba(Skin.focus.r, Skin.focus.g, Skin.focus.b, 0.5)
                                                : Skin.border
                                    clip: true

                                    // The pad's own sound, small, from the
                                    // middle out, in the lower half where
                                    // the name does not sit on it. A pad
                                    // that sounds draws it brighter, and a
                                    // head crosses it while it plays.
                                    Row {
                                        id: thumbRow
                                        visible: cell.hasAudio
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.bottom: parent.bottom
                                        anchors.leftMargin: Skin.spacingS
                                        anchors.rightMargin: Skin.spacingS
                                        anchors.bottomMargin: Skin.spacingS
                                        height: parent.height * 0.42
                                        Repeater {
                                            model: cell.thumb
                                            Rectangle {
                                                required property real modelData
                                                width: Math.max(1, thumbRow.width / Math.max(1, cell.thumb.length))
                                                height: Math.max(Px.px(2), Math.min(1, modelData) * thumbRow.height)
                                                anchors.verticalCenter: thumbRow.verticalCenter
                                                color: cell.isSounding ? Skin.meterLow : Skin.accent
                                                opacity: cell.isSounding ? 0.95 : 0.5
                                            }
                                        }
                                    }
                                    Rectangle {
                                        visible: cell.hasAudio && cell.pos >= 0
                                        x: Skin.spacingS + cell.pos * Math.max(0, parent.width - 2 * Skin.spacingS) - width / 2
                                        anchors.top: thumbRow.top
                                        anchors.bottom: thumbRow.bottom
                                        width: Px.px(2)
                                        color: Skin.meterLow
                                    }

                                    // A wash across the whole pad on any
                                    // matching MIDI note, even one with
                                    // nothing loaded to actually play -
                                    // confirmation a controller's note
                                    // reached this pad, for wiring one up
                                    // by eye.
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: parent.radius
                                        color: Skin.focus
                                        opacity: cell.isHitFlash ? 0.55 : 0.0
                                        Behavior on opacity {
                                            NumberAnimation { duration: 70 }
                                        }
                                    }

                                    // The take being written onto this pad,
                                    // or about to be: red filling the pad
                                    // as the buffer fills, yellow breathing
                                    // while the head waits for the bar.
                                    Rectangle {
                                        visible: cell.isRecording
                                        anchors.left: parent.left
                                        anchors.top: parent.top
                                        anchors.bottom: parent.bottom
                                        width: Math.max(0, Math.min(1, root.recFill)) * parent.width
                                        color: Skin.arm
                                        opacity: 0.35
                                    }
                                    Rectangle {
                                        visible: cell.isRecording || cell.isArmed || cell.waiting
                                        anchors.fill: parent
                                        radius: parent.radius
                                        color: cell.waiting ? Skin.solo : cell.hue
                                        opacity: cell.pulse * 0.22
                                    }
                                }

                                Text {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: Skin.spacingS
                                    text: cell.info.name || ("P" + (cell.index + 1))
                                    color: cell.hasAudio ? Skin.text : Skin.textDim
                                    font.pixelSize: Skin.fontS
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                Text {
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: Skin.spacingS
                                    anchors.topMargin: Skin.spacingS + Skin.fontS + Px.px(3)
                                    text: root.noteName(cell.info.note)
                                    color: Skin.textDim
                                    font.pixelSize: Skin.fontXS
                                    font.family: Skin.monoFamily
                                }
                                // Hold, not one-shot: a small mark, since
                                // it changes what a hit does.
                                Text {
                                    visible: cell.hasAudio && cell.info.oneShot === false
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.margins: Skin.spacingS
                                    anchors.topMargin: Skin.spacingS + Skin.fontS + Px.px(3)
                                    text: qsTr("hold")
                                    color: Skin.textDim
                                    font.pixelSize: Skin.fontXS
                                }

                                Tip {
                                    text: root.mapping
                                          ? qsTr("Tap, then hit the pad on the controller that should play %1.")
                                                .arg(cell.info.name || ("P" + (cell.index + 1)))
                                          : cell.hasAudio
                                            ? qsTr("%1 on %2. Tap to play and take it in hand; Rec takes the strip onto it.")
                                                .arg(cell.info.name || ("P" + (cell.index + 1)))
                                                .arg(root.noteName(cell.info.note))
                                            : qsTr("Empty, on %1. Tap to take it in hand, then Rec or Load.")
                                                .arg(root.noteName(cell.info.note))
                                    visible: hover.hovered
                                }

                                HoverHandler { id: hover }

                                TapHandler {
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onPressedChanged: {
                                        if (pressed) {
                                            if (root.mapping) {
                                                root.armPadNote(cell.index)
                                                return
                                            }
                                            root.focusPad(cell.index)
                                            Mixer.previewSamplerPad(root.targetRow,
                                                                    root.targetSlot,
                                                                    cell.index, 110)
                                            root.heldPad = cell.index
                                        } else if (root.heldPad === cell.index) {
                                            Mixer.releaseSamplerPad(root.targetRow,
                                                                    root.targetSlot,
                                                                    cell.index)
                                            root.heldPad = -1
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // The tape: the pad in hand, drawn wide. Trim by dragging
                // its edges, fade by dragging the small marks just inside
                // them, the way the looper's is.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Px.px(132)
                    radius: Skin.radiusL
                    color: Qt.rgba(0, 0, 0, 0.25)
                    border.width: 1
                    border.color: Skin.line

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

                        readonly property bool hasAudio: root.focusedPad.hasAudio === true
                        readonly property bool taking: root.recording && root.recPad === root.focused
                        readonly property real pos: root.focusedPad.pos === undefined ? -1 : root.focusedPad.pos
                        readonly property int trimStartX: root.trimStart * wave.width
                        readonly property int trimEndX: root.trimEnd * wave.width
                        // Fades are capped at half the trim window in the
                        // engine, so a fade fraction of 1.0 walks its handle
                        // exactly to the midpoint.
                        readonly property real halfWindow: (trimEndX - trimStartX) / 2
                        readonly property int fadeInX: trimStartX + root.fadeIn * wave.halfWindow
                        readonly property int fadeOutX: trimEndX - root.fadeOut * wave.halfWindow

                        Rectangle {
                            id: tape
                            anchors.fill: parent
                            radius: Skin.radius
                            color: Skin.slotEmpty
                            border.width: 1
                            border.color: Qt.rgba(root.stateHue.r, root.stateHue.g,
                                                  root.stateHue.b, 0.45)
                            clip: true

                            // The rest line, faint, so an empty tape is
                            // still a tape.
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                height: 1
                                color: Qt.rgba(Skin.accent.r, Skin.accent.g, Skin.accent.b, 0.12)
                            }

                            // Empty: say so, and say what to do about it.
                            Column {
                                anchors.centerIn: parent
                                visible: !wave.hasAudio && !wave.taking && !root.armed && !root.countingIn
                                spacing: Skin.spacingXS
                                width: parent.width - 2 * Skin.spacingL
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: qsTr("NOTHING ON %1").arg(root.focusedName.toUpperCase())
                                    color: Skin.disabled
                                    font.pixelSize: Skin.fontS
                                    font.bold: true
                                    font.letterSpacing: Px.px(2)
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: qsTr("Press REC to take the strip's input onto it, or LOAD a file.")
                                    color: Skin.textDim
                                    font.pixelSize: Skin.fontXS
                                    horizontalAlignment: Text.AlignHCenter
                                    width: parent.width
                                    wrapMode: Text.WordWrap
                                }
                            }

                            // The take as it is written: a wash of Rec's
                            // colour growing across the tape as the buffer
                            // fills, behind whatever the pad already held.
                            Rectangle {
                                visible: wave.taking
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: Math.max(0, Math.min(1, root.recFill)) * parent.width
                                color: Qt.rgba(Skin.arm.r, Skin.arm.g, Skin.arm.b, 0.14)
                            }

                            // The waveform itself, drawn from the middle out.
                            Row {
                                id: bars
                                visible: wave.hasAudio
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                height: parent.height - 2 * Skin.spacingL

                                Repeater {
                                    model: root.peaks

                                    Rectangle {
                                        required property real modelData
                                        width: Math.max(1, bars.width / Math.max(1, root.peaks.length))
                                        height: Math.max(Px.px(2),
                                                         Math.min(1, modelData) * bars.height)
                                        anchors.verticalCenter: bars.verticalCenter
                                        color: Skin.accent
                                        opacity: 0.85
                                    }
                                }
                            }

                            // Dims what trimming leaves out of the pad.
                            Rectangle {
                                visible: wave.hasAudio
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: wave.trimStartX
                                color: Skin.background
                                opacity: 0.72
                            }
                            Rectangle {
                                visible: wave.hasAudio
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: parent.width - wave.trimEndX
                                color: Skin.background
                                opacity: 0.72
                            }

                            // The fade ramps, as a gradient rather than a
                            // number: opaque where the pad is silent, clear
                            // where it is at full volume.
                            Rectangle {
                                visible: wave.hasAudio && root.fadeIn > 0
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
                                visible: wave.hasAudio && root.fadeOut > 0
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

                            // The playhead, with a little light around it,
                            // green while the pad sounds. -1 while it is
                            // silent.
                            Item {
                                visible: wave.hasAudio && wave.pos >= 0
                                x: wave.pos * parent.width - width / 2
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: Px.px(14)
                                Rectangle {
                                    anchors.fill: parent
                                    gradient: Gradient {
                                        orientation: Gradient.Horizontal
                                        GradientStop { position: 0.0; color: "transparent" }
                                        GradientStop { position: 0.5; color: Qt.rgba(Skin.meterLow.r, Skin.meterLow.g, Skin.meterLow.b, 0.35) }
                                        GradientStop { position: 1.0; color: "transparent" }
                                    }
                                }
                                Rectangle {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: Px.px(2)
                                    color: Skin.meterLow
                                }
                            }

                            // The sign over the tape while a take is coming:
                            // the count, in yellow; the wait for the bar;
                            // the take itself, in red.
                            Column {
                                anchors.centerIn: parent
                                visible: root.countingIn || root.armed || wave.taking
                                spacing: 0
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    visible: root.countingIn
                                    text: root.countInLeft
                                    color: Skin.solo
                                    font.pixelSize: Skin.fontXL * 2
                                    font.bold: true
                                    font.family: Skin.monoFamily
                                    style: Text.Outline
                                    styleColor: Qt.rgba(0, 0, 0, 0.6)
                                    opacity: 0.55 + 0.45 * stageChip.pulse
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: root.countingIn ? qsTr("COUNT-IN")
                                        : root.armed ? qsTr("REC ON THE %1").arg(root.quantize === 1 ? qsTr("BEAT") : qsTr("BAR"))
                                        : qsTr("TAKING ONTO %1").arg(root.focusedName.toUpperCase())
                                    color: wave.taking ? Skin.arm : Skin.solo
                                    font.pixelSize: Skin.fontS
                                    font.bold: true
                                    font.letterSpacing: Px.px(2)
                                    style: Text.Outline
                                    styleColor: Qt.rgba(0, 0, 0, 0.6)
                                    opacity: 0.55 + 0.45 * stageChip.pulse
                                }
                            }

                            // The flare on a punch in or out.
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
                        // pad should start and stop, with a tab at each end
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
                                      ? qsTr("Where the pad starts. Drag it in to trim the head.")
                                      : qsTr("Where the pad ends. Drag it in to trim the tail.")
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
                                    Mixer.setSamplerTrim(root.targetRow, root.targetSlot,
                                                         root.focused, root.trimStart, root.trimEnd)
                                }
                            }
                        }

                        TrimHandle {
                            isStart: true
                            atX: wave.trimStartX
                            visible: wave.hasAudio
                        }
                        TrimHandle {
                            isStart: false
                            atX: wave.trimEndX
                            visible: wave.hasAudio
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
                                      ? qsTr("Fade in. Drag it right and the pad swells from silence.")
                                      : qsTr("Fade out. Drag it left and the pad sinks to silence before the end.")
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
                                    Mixer.setSamplerFades(root.targetRow, root.targetSlot,
                                                          root.focused, root.fadeIn, root.fadeOut)
                                }
                            }
                        }

                        FadeHandle {
                            isIn: true
                            atX: wave.fadeInX
                            visible: wave.hasAudio
                        }
                        FadeHandle {
                            isIn: false
                            atX: wave.fadeOutX
                            visible: wave.hasAudio
                        }
                    }
                }

                // The pad in hand: its name, its key, and what a hit does.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Skin.spacingXS

                    RowLabel { text: qsTr("PAD") }
                    Item { Layout.preferredWidth: Skin.spacingXS }

                    TextField {
                        id: nameField
                        Layout.preferredWidth: Px.px(150)
                        Layout.preferredHeight: Skin.buttonHeight
                        color: Skin.text
                        font.pixelSize: Skin.fontS
                        selectByMouse: true
                        placeholderText: "P" + (root.focused + 1)
                        placeholderTextColor: Skin.disabled
                        background: Rectangle {
                            color: Skin.slotEmpty
                            radius: Skin.radius
                            border.width: 1
                            border.color: nameField.activeFocus ? Skin.focus : Skin.border
                        }
                        onEditingFinished: Mixer.setSamplerPadName(
                            root.targetRow, root.targetSlot, root.focused, text)
                    }

                    // The key. Drag it sideways or roll the wheel to move it
                    // a semitone; a pad that already had that key takes
                    // this one's, so no two pads share a note. MAP binds a
                    // pad to a controller's key by hitting it instead.
                    Rectangle {
                        id: noteChip
                        Layout.preferredWidth: Px.px(96)
                        Layout.preferredHeight: Skin.buttonHeight
                        radius: Skin.radius
                        color: Skin.slotEmpty
                        border.width: root.waitingPad === root.focused ? Px.px(2) : 1
                        border.color: root.waitingPad === root.focused ? Skin.solo
                                    : root.mapping ? Skin.focus
                                    : Qt.rgba(Skin.accent.r, Skin.accent.g, Skin.accent.b, 0.45)
                        readonly property int note: Math.round(root.focusedPad.note)

                        Row {
                            anchors.centerIn: parent
                            spacing: Skin.spacingS
                            Text {
                                text: root.noteName(noteChip.note)
                                color: Skin.accent
                                font.pixelSize: Skin.fontL
                                font.bold: true
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: noteChip.note
                                color: Skin.textDim
                                font.pixelSize: Skin.fontXS
                                font.family: Skin.monoFamily
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        HoverHandler { id: noteHover; cursorShape: Qt.SizeHorCursor }
                        Tip {
                            text: root.mapping
                                  ? qsTr("Tap, then hit the pad on the controller that should play %1.").arg(root.focusedName)
                                  : qsTr("The key that plays this pad. Drag sideways or roll the wheel for a semitone. A pad that already had the key takes this one's.")
                            visible: noteHover.hovered
                        }
                        TapHandler {
                            enabled: root.mapping
                            onTapped: root.armPadNote(root.focused)
                        }
                        DragHandler {
                            enabled: !root.mapping
                            target: null
                            property int pressNote: 0
                            onActiveChanged: if (active) pressNote = noteChip.note
                            onCentroidChanged: if (active) {
                                const dx = centroid.position.x - centroid.pressPosition.x
                                const next = Math.max(0, Math.min(127, Math.round(pressNote + dx / Px.px(14))))
                                if (next !== noteChip.note) {
                                    Mixer.assignSamplerPadNote(root.targetRow, root.targetSlot, root.focused, next)
                                    root.refresh()
                                }
                            }
                        }
                        WheelHandler {
                            onWheel: event => {
                                const next = Math.max(0, Math.min(127, noteChip.note + (event.angleDelta.y > 0 ? 1 : -1)))
                                Mixer.assignSamplerPadNote(root.targetRow, root.targetSlot, root.focused, next)
                                root.refresh()
                            }
                        }
                    }

                    StripButton {
                        Layout.preferredWidth: Px.px(88)
                        label: root.focusedPad.oneShot ? qsTr("ONE-SHOT") : qsTr("HOLD")
                        active: root.focusedPad.oneShot
                        tip: root.mapping
                             ? qsTr("Tap to bind One-shot to the next control.")
                             : qsTr("One-shot plays to the end however short the hit. Hold stops when the note does.")
                        onClicked: {
                            if (root.mapping) { root.armParam(root.pOneShot, 0, 1); return }
                            root.writePad({ oneShot: !root.focusedPad.oneShot })
                        }
                        MapDot { param: root.pOneShot }
                    }

                    Item { Layout.fillWidth: true }

                    // Where Rec lands: at once, or on the grid.
                    Item {
                        Layout.preferredWidth: recLabel.implicitWidth + Px.px(10)
                        Layout.preferredHeight: Skin.buttonHeight
                        RowLabel {
                            id: recLabel
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("REC ON")
                        }
                        MapDot { param: root.pQuantize }
                    }

                    component QuantizeButton: StripButton {
                        required property int forValue
                        Layout.preferredWidth: Px.px(48)
                        active: root.quantize === forValue
                        activeColor: Skin.solo
                        onClicked: {
                            if (root.mapping) { root.armParam(root.pQuantize, 0, 2); return }
                            root.quantize = forValue
                            Mixer.setInsertParameter(root.targetRow, root.targetSlot,
                                                     root.pQuantize, forValue)
                        }
                    }
                    QuantizeButton {
                        forValue: 0; label: qsTr("now")
                        tip: qsTr("Rec starts and stops the moment it is pressed.")
                    }
                    QuantizeButton {
                        forValue: 1; label: qsTr("beat")
                        tip: qsTr("Rec waits for the next beat, and closes on one.")
                    }
                    QuantizeButton {
                        forValue: 2; label: qsTr("bar")
                        tip: qsTr("Rec waits for the next bar, and closes on one.")
                    }
                }

                // The kit as a whole: carried out and brought back.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Skin.spacingXS

                    RowLabel { text: qsTr("KIT") }
                    Item { Layout.preferredWidth: Skin.spacingXS }

                    StripButton {
                        Layout.preferredWidth: Px.px(96)
                        label: qsTr("SAVE PACK")
                        tip: qsTr("All sixteen pads, as they are now, to a file you can carry or reload later - the kit, not the rest of the strip.")
                        onClicked: packSaveDialog.open()
                    }
                    StripButton {
                        Layout.preferredWidth: Px.px(96)
                        label: qsTr("OPEN PACK")
                        tip: qsTr("Replace all sixteen pads with a kit saved earlier. Gain, Rec On and Count stay as they are.")
                        onClicked: packOpenDialog.open()
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: {
                            let n = 0
                            for (let i = 0; i < root.pads.length; ++i)
                                if (root.pads[i].hasAudio === true) ++n
                            return n === 0 ? qsTr("no pads loaded")
                                 : n === 1 ? qsTr("1 pad loaded")
                                 : qsTr("%1 pads loaded").arg(n)
                        }
                        color: Skin.textDim
                        font.pixelSize: Skin.fontXS
                        font.family: Skin.monoFamily
                    }
                }
            }

            // --- the rides and the weather ------------------------------------------
            // A layout fills by default; this one is told not to, or it
            // takes the pads' room.
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
                        label: qsTr("VOLUME")
                        hue: root.hueVolume
                        value: Math.max(0, Math.min(1, root.focusedPad.volume / 2))
                        readout: root.focusedPad.volume.toFixed(2)
                        mapped: root.isMapped(root.pVolume)
                        waiting: root.waitingParam === root.pVolume
                        mapping: root.mapping
                        tip: qsTr("How loud the pad in hand is, 0 to 2×. Each pad has its own; the kit's Gain sits over all of them.")
                        onEdited: v => root.writePad({ volume: v * 2 })
                        onArmed: root.armParam(root.pVolume, 0, 2)
                    }

                    ParamBar {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        big: true
                        label: qsTr("GAIN")
                        hue: root.hueGain
                        value: Math.max(0, Math.min(1, root.gain / 2))
                        readout: root.gain.toFixed(2)
                        mapped: root.isMapped(root.pGain)
                        waiting: root.waitingParam === root.pGain
                        mapping: root.mapping
                        tip: qsTr("The whole kit, 0 to 2×, on top of every pad's own volume. The strip's input passing through during a take is untouched.")
                        onEdited: v => {
                            Mixer.setInsertParameter(root.targetRow, root.targetSlot, root.pGain, v * 2)
                            root.gain = v * 2
                        }
                        onArmed: root.armParam(root.pGain, 0, 2)
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Px.px(150)
                    columns: 2
                    columnSpacing: Skin.spacingS
                    rowSpacing: Skin.spacingS

                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("PITCH"); hue: root.huePitch
                        bipolar: true
                        value: (root.focusedPad.pitch + 24) / 48
                        readout: (root.focusedPad.pitch >= 0 ? "+" : "−")
                                 + Math.abs(Math.round(root.focusedPad.pitch))
                        step: 1 / 48
                        fineStep: 1 / 48
                        mapped: root.isMapped(root.pPitch)
                        waiting: root.waitingParam === root.pPitch
                        mapping: root.mapping
                        tip: qsTr("The pad's pitch, two octaves either way in semitones. It plays faster or slower to get there, the way a tape does.")
                        onEdited: v => root.writePad({ pitch: Math.round(v * 48 - 24) })
                        onArmed: root.armParam(root.pPitch, -24, 24)
                    }
                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("PAN"); hue: root.huePan
                        bipolar: true
                        value: (root.focusedPad.pan + 1) * 0.5
                        readout: Math.abs(root.focusedPad.pan) < 0.005 ? qsTr("C")
                               : (root.focusedPad.pan < 0 ? "L" : "R") + Math.round(Math.abs(root.focusedPad.pan) * 100)
                        mapped: root.isMapped(root.pPan)
                        waiting: root.waitingParam === root.pPan
                        mapping: root.mapping
                        tip: qsTr("Where the pad sits, left to right.")
                        onEdited: v => root.writePad({ pan: v * 2 - 1 })
                        onArmed: root.armParam(root.pPan, -1, 1)
                    }
                }
            }
        }

        // --- status line -------------------------------------------------------------
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: {
                if (root.waitingPad >= 0)
                    return qsTr("Hit the pad on the controller that should play %1… Esc cancels.").arg(root.focusedName)
                if (root.waitingParam >= 0)
                    return qsTr("Turn a knob on this strip's MIDI input… Esc cancels.")
                if (root.mapping)
                    return qsTr("Tap a pad, a bar or a button, then hit the control.")
                if (root.countingIn)
                    return qsTr("Counting in. The take starts on the one - play then.")
                if (root.armed)
                    return qsTr("Armed. The take starts on the next %1 - play then.").arg(root.quantize === 1 ? qsTr("beat") : qsTr("bar"))
                if (root.recording)
                    return qsTr("Taking the strip onto %1. Rec again to close it; eight seconds at most.").arg(root.focusedName)
                if (root.focusedPad.hasAudio === true)
                    return qsTr("Trim %1 with the tall handles, fade with the round ones. Tap any pad to play it and take it in hand.").arg(root.focusedName)
                if (root.anyAudio)
                    return qsTr("%1 is empty. Rec takes the strip's input onto it, Load a file.").arg(root.focusedName)
                return qsTr("Tap a pad, press Rec and play into the strip, or Load a file onto it. Open Pack brings a whole kit.")
            }
            color: root.mapping || Mixer.learning ? Skin.solo
                 : root.countingIn || root.armed ? Skin.solo : Skin.textDim
            font.pixelSize: Skin.fontXS
            wrapMode: Text.WordWrap
        }
    }
}
