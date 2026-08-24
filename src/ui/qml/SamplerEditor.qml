pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Nirbija

// Sixteen pads, Rec from the strip, a file onto the one in focus. The
// generic parameter list would be ten sliders and no grid; this is the
// instrument. Not modal: the mixer stays live, same as the looper.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property int focused: 0
    property bool recording: false
    property int recPad: 0
    property int sounding: 0
    property int hitFlash: 0
    property int lastNote: -1
    property int lastCc: -1
    property real gain: 1
    property int quantize: 0
    property bool countIn: false
    property bool countingIn: false
    property int countInLeft: 0
    property var pads: []
    property var peaks: []
    property bool positioned: false
    property int heldPad: -1
    property bool mapping: false
    property int waitingPad: -1
    property int waitingParam: -1
    property var mapped: ({})

    // Mirrors the focused pad's own trim/fade, ahead of the next poll — a
    // drag moves this immediately, the way the looper's waveform does, and
    // refresh() reconciles it once the engine echoes the write back.
    property real trimStart: 0.0
    property real trimEnd: 1.0
    property real fadeIn: 0.0
    property real fadeOut: 0.0

    readonly property var quantizeNames: [qsTr("free"), qsTr("beat"), qsTr("bar")]

    function padAt(index) {
        const p = root.pads[index]
        return p && typeof p === "object" ? p : ({
            note: 36, name: "", oneShot: true, volume: 1, pan: 0,
            pitch: 0, start: 0, end: 1, fadeIn: 0, fadeOut: 0,
            hasAudio: false, canUndo: false
        })
    }

    readonly property var focusedPad: root.padAt(root.focused)

    width: Px.px(620)
    height: Px.px(700)
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

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Skin.fast }
    }

    function openFor(row, slot) {
        root.targetRow = row
        root.targetSlot = slot
        root.refresh()
        root.refreshMapped()
        Mixer.listenSamplerMidi(row, slot, true)
        if (!root.positioned) {
            root.x = Math.round((Overlay.overlay.width - root.width) / 2)
            root.y = Math.round((Overlay.overlay.height - root.height) / 2)
            root.positioned = true
        }
        root.open()
    }

    onOpened: {
        Mixer.listenSamplerMidi(root.targetRow, root.targetSlot, true)
        poll.start()
    }
    onClosed: {
        poll.stop()
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
        const ids = [0, 1, 4, 5, 6, 7, 8, 10, 11]
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
        Mixer.setSamplerFocus(root.targetRow, root.targetSlot, index)
        root.focused = index
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

    function refresh() {
        const snap = Mixer.insertSamplerSnapshot(root.targetRow, root.targetSlot)
        if (!snap || snap.pads === undefined) return
        root.focused = snap.focused
        root.recording = snap.recording
        root.recPad = snap.recPad
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
        root.peaks = Mixer.samplerWaveform(root.targetRow, root.targetSlot,
                                           root.focused, 160)
        const fp = root.padAt(root.focused)
        root.trimStart = fp.start
        root.trimEnd = fp.end
        root.fadeIn = fp.fadeIn
        root.fadeOut = fp.fadeOut
        if (!nameField.activeFocus)
            nameField.text = root.padAt(root.focused).name
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

    Timer {
        id: poll
        interval: 66
        repeat: true
        onTriggered: root.refresh()
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
        }
    }

    contentItem: ColumnLayout {
        spacing: Skin.spacing

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
            Text {
                text: root.waitingPad >= 0 ? qsTr("hit a pad…")
                    : root.waitingParam >= 0 ? qsTr("move a control…")
                    : root.mapping ? qsTr("tap a control")
                    : root.countingIn ? qsTr("in %1…").arg(root.countInLeft)
                    : root.recording ? qsTr("recording…")
                    : root.lastNote >= 0 ? qsTr("note %1").arg(root.lastNote)
                    : root.lastCc >= 0 ? qsTr("CC %1").arg(root.lastCc)
                    : qsTr("listening")
                color: root.mapping || Mixer.learning ? Skin.solo
                    : (root.lastNote >= 0 || root.lastCc >= 0) ? Skin.focus
                    : Skin.arm
                font.pixelSize: Skin.fontS
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            StripButton {
                Layout.preferredWidth: Px.px(64)
                label: root.quantizeNames[Math.max(0, Math.min(2, root.quantize))]
                tip: qsTr("Rec start and length: free, the next beat, or the next bar.")
                onClicked: {
                    const next = (root.quantize + 1) % 3
                    Mixer.setInsertParameter(root.targetRow, root.targetSlot, 3, next)
                    root.quantize = next
                }
            }
            StripButton {
                Layout.preferredWidth: Px.px(64)
                label: qsTr("count-in")
                active: root.countIn
                tip: root.mapping
                     ? qsTr("Tap to bind Count-in to the next control.")
                     : qsTr("A bar of clicks before Rec punches in, so the take does not start with the button being pressed.")
                onClicked: {
                    if (root.mapping) { root.armParam(10, 0, 1); return }
                    Mixer.setSamplerCountIn(root.targetRow, root.targetSlot, !root.countIn)
                    root.countIn = !root.countIn
                }
                MapDot { param: 10 }
            }
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Load")
                tip: qsTr("A file onto the focused pad. Rec does the same from the strip.")
                onClicked: loadDialog.open()
            }
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Undo")
                enabled: root.mapping || root.focusedPad.canUndo === true
                tip: root.mapping
                     ? qsTr("Tap to bind Undo to the next control.")
                     : qsTr("Swap back to what the pad held before its last Rec, Load or Clear. Press again to swap forward.")
                onClicked: {
                    if (root.mapping) { root.armParam(11, 0, 1); return }
                    Mixer.undoSamplerPad(root.targetRow, root.targetSlot, root.focused)
                    root.refresh()
                }
                MapDot { param: 11 }
            }
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Clear")
                danger: true
                tip: root.mapping
                     ? qsTr("Tap to bind Clear to the next control.")
                     : qsTr("Throw away the focused pad.")
                onClicked: {
                    if (root.mapping) { root.armParam(4, 0, 1); return }
                    Mixer.clearSamplerPad(root.targetRow, root.targetSlot, root.focused)
                    root.refresh()
                }
                MapDot { param: 4 }
            }
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Rec")
                activeColor: Skin.arm
                active: root.recording || root.countingIn
                danger: true
                tip: root.mapping
                     ? qsTr("Tap to bind Rec to the next control.")
                     : qsTr("Record the strip's input onto the focused pad. Press again to close the take.")
                onClicked: {
                    if (root.mapping) { root.armParam(1, 0, 1); return }
                    Mixer.setSamplerRecord(root.targetRow, root.targetSlot,
                                           !(root.recording || root.countingIn))
                    root.recording = !root.recording
                }
                MapDot { param: 1 }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingS

            StripButton {
                Layout.preferredWidth: Px.px(48)
                label: qsTr("MAP")
                active: root.mapping || Mixer.learning
                activeColor: Skin.solo
                tip: qsTr("Bind Rec, a knob, or a pad to the controller. Press MAP, tap what you want, then hit the control. MAP stays on so the next one can follow.")
                onClicked: {
                    if (root.mapping || Mixer.learning) root.stopMapping()
                    else {
                        root.mapping = true
                        root.waitingPad = -1
                        root.waitingParam = -1
                    }
                }
            }
            Text {
                text: qsTr("pack")
                color: Skin.textDim
                font.pixelSize: Skin.fontXS
                font.family: Skin.monoFamily
            }
            Item { Layout.fillWidth: true }
            StripButton {
                Layout.preferredWidth: Px.px(84)
                label: qsTr("Save Pack")
                tip: qsTr("All sixteen pads, as they are now, to a file you can carry or reload later — the kit, not the rest of the strip.")
                onClicked: packSaveDialog.open()
            }
            StripButton {
                Layout.preferredWidth: Px.px(84)
                label: qsTr("Open Pack")
                tip: qsTr("Replace all sixteen pads with a kit saved earlier.")
                onClicked: packOpenDialog.open()
            }
        }

        GridLayout {
            id: grid
            Layout.fillWidth: true
            Layout.preferredHeight: Px.px(280)
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
                    readonly property bool isSounding: (root.sounding
                                                        & (1 << cell.index)) !== 0
                    readonly property bool isHitFlash: (root.hitFlash
                                                        & (1 << cell.index)) !== 0
                    readonly property bool hasAudio: cell.info.hasAudio === true
                    property real pulse: 0.4

                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    SequentialAnimation on pulse {
                        running: cell.isRecording
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
                        radius: Skin.radiusL
                        color: cell.hasAudio ? Skin.slot : Skin.slotEmpty
                        border.width: (cell.isFocused || cell.isHitFlash) ? Px.px(2) : 1
                        border.color: root.waitingPad === cell.index ? Skin.solo
                                    : cell.isRecording ? Skin.arm
                                    : cell.isHitFlash ? Skin.focus
                                    : cell.isFocused ? Skin.focus
                                    : cell.isSounding ? Skin.meterLow
                                    : Skin.border

                        Rectangle {
                            visible: cell.hasAudio
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 1
                            height: Math.max(Px.px(3), parent.height * 0.18)
                            radius: Skin.radiusS
                            color: Skin.accent
                            opacity: cell.isSounding ? 0.95 : 0.45
                        }

                        // A wash across the whole pad on any matching MIDI
                        // note, even one with nothing loaded to actually
                        // play — confirmation a controller's note reached
                        // this pad, for wiring one up by eye.
                        Rectangle {
                            anchors.fill: parent
                            radius: parent.radius
                            color: Skin.focus
                            opacity: cell.isHitFlash ? 0.72 : 0.0
                            Behavior on opacity {
                                NumberAnimation { duration: 70 }
                            }
                        }

                        Rectangle {
                            visible: cell.isRecording
                            anchors.fill: parent
                            radius: parent.radius
                            color: Skin.arm
                            opacity: cell.pulse * 0.28
                        }
                    }

                    Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: Skin.spacingS
                        spacing: Px.px(2)

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: parent.width
                            text: cell.info.name || ("P" + (cell.index + 1))
                            color: Skin.text
                            font.pixelSize: Skin.fontS
                            font.bold: true
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: cell.info.note
                            color: Skin.textDim
                            font.pixelSize: Skin.fontXS
                            font.family: Skin.monoFamily
                        }
                    }

                    Tip {
                        text: root.mapping
                              ? qsTr("Tap, then hit the pad on the controller that should play %1.")
                                    .arg(cell.info.name || ("P" + (cell.index + 1)))
                              : qsTr("%1 — MIDI %2. Tap to play, Rec to capture the strip.")
                                    .arg(cell.info.name).arg(cell.info.note)
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
                                Mixer.setSamplerFocus(root.targetRow,
                                                      root.targetSlot, cell.index)
                                root.focused = cell.index
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

        // --- the waveform: trim by dragging its edges, fade by dragging the
        // small marks just inside them, same as the looper's editor --------
        Item {
            id: wave
            Layout.fillWidth: true
            Layout.preferredHeight: Px.px(100)

            // A miss on a handle used to fall through the graph onto the
            // strip behind it. Eat the gesture here; the handles take over
            // when they actually get the press.
            HoverHandler { cursorShape: Qt.ArrowCursor }
            TapHandler {}
            DragHandler {
                target: null
                grabPermissions: PointerHandler.TakeOverForbidden
            }

            readonly property bool hasAudio: root.focusedPad.hasAudio === true
            readonly property int trimStartX: root.trimStart * wave.width
            readonly property int trimEndX: root.trimEnd * wave.width
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
                    visible: !wave.hasAudio
                    text: qsTr("empty")
                    color: Skin.disabled
                    font.pixelSize: Skin.fontS
                }

                Row {
                    id: bars
                    visible: wave.hasAudio
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    height: parent.height - 2 * Skin.spacingS

                    Repeater {
                        model: root.peaks

                        Rectangle {
                            required property int index
                            required property real modelData
                            width: Math.max(1, bars.width / Math.max(1, root.peaks.length))
                            height: Math.max(Px.px(2),
                                             Math.min(1, modelData) * bars.height)
                            anchors.verticalCenter: bars.verticalCenter
                            color: Skin.accent
                            opacity: 0.8
                        }
                    }
                }

                // Dims what trimming leaves out of the take.
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

                // The fade ramps, as a gradient rather than a number: opaque
                // where the pad is silent, clear where it is at full volume.
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
            }

            // Trim handles: full-height, dragged to wherever the pad should
            // start and stop playing.
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

        Text {
            Layout.fillWidth: true
            visible: wave.hasAudio
            text: qsTr("Drag the tall handles to trim, the round ones just inside them to fade in and out.")
            color: Skin.textDim
            font.pixelSize: Skin.fontXS
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingS

            TextField {
                id: nameField
                Layout.preferredWidth: Px.px(140)
                color: Skin.text
                font.pixelSize: Skin.font
                selectByMouse: true
                background: Rectangle {
                    color: Skin.slotEmpty
                    radius: Skin.radius
                    border.width: 1
                    border.color: nameField.activeFocus ? Skin.focus : Skin.border
                }
                onEditingFinished: Mixer.setSamplerPadName(
                    root.targetRow, root.targetSlot, root.focused, text)
            }
            StripButton {
                Layout.preferredWidth: Px.px(88)
                label: root.focusedPad.oneShot ? qsTr("one-shot") : qsTr("hold")
                active: root.focusedPad.oneShot
                tip: root.mapping
                     ? qsTr("Tap to bind One-shot to the next control.")
                     : qsTr("One-shot plays to the end. Hold stops when the note does.")
                onClicked: {
                    if (root.mapping) { root.armParam(5, 0, 1); return }
                    root.writePad({ oneShot: !root.focusedPad.oneShot })
                }
                MapDot { param: 5 }
            }
            Text {
                text: qsTr("note %1").arg(Math.round(root.focusedPad.note))
                color: Skin.textDim
                font.pixelSize: Skin.fontS
                font.family: Skin.monoFamily
            }
            Item { Layout.fillWidth: true }
        }

        ValueTrack {
            Layout.fillWidth: true
            label: qsTr("volume")
            value: Math.max(0, Math.min(1, root.focusedPad.volume / 2))
            valueText: root.focusedPad.volume.toFixed(2)
            tip: root.mapping
                 ? qsTr("Tap to bind pad volume to the next control.")
                 : qsTr("Pad volume.")
            pickOnly: root.mapping
            onMoved: (v) => root.writePad({ volume: v * 2 })
            onPicked: root.armParam(7, 0, 2)
        }
        ValueTrack {
            Layout.fillWidth: true
            label: qsTr("pan")
            value: (root.focusedPad.pan + 1) * 0.5
            valueText: root.focusedPad.pan.toFixed(2)
            tip: root.mapping
                 ? qsTr("Tap to bind pad pan to the next control.")
                 : qsTr("Pad pan.")
            pickOnly: root.mapping
            onMoved: (v) => root.writePad({ pan: v * 2 - 1 })
            onPicked: root.armParam(8, -1, 1)
        }
        ValueTrack {
            Layout.fillWidth: true
            label: qsTr("pitch")
            value: (root.focusedPad.pitch + 24) / 48
            valueText: (root.focusedPad.pitch >= 0 ? "+" : "")
                       + Math.round(root.focusedPad.pitch)
            tip: root.mapping
                 ? qsTr("Tap to bind pad pitch to the next control.")
                 : qsTr("Pad pitch, in semitones.")
            pickOnly: root.mapping
            onMoved: (v) => root.writePad({ pitch: Math.round(v * 48 - 24) })
            onPicked: root.armParam(6, -24, 24)
        }
        ValueTrack {
            Layout.fillWidth: true
            label: qsTr("gain")
            value: Math.max(0, Math.min(1, root.gain / 2))
            valueText: root.gain.toFixed(2)
            tip: root.mapping
                 ? qsTr("Tap to bind master gain to the next control.")
                 : qsTr("Master gain for every pad.")
            pickOnly: root.mapping
            onMoved: (v) => {
                Mixer.setInsertParameter(root.targetRow, root.targetSlot, 0, v * 2)
                root.gain = v * 2
            }
            onPicked: root.armParam(0, 0, 2)
        }
    }
}
