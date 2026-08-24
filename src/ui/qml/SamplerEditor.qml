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
    property real gain: 1
    property int quantize: 0
    property var pads: []
    property var peaks: []
    property bool positioned: false
    property int heldPad: -1

    readonly property var quantizeNames: [qsTr("free"), qsTr("beat"), qsTr("bar")]

    function padAt(index) {
        const p = root.pads[index]
        return p && typeof p === "object" ? p : ({
            note: 36, name: "", oneShot: true, volume: 1, pan: 0,
            pitch: 0, start: 0, end: 1, hasAudio: false
        })
    }

    readonly property var focusedPad: root.padAt(root.focused)

    width: Px.px(620)
    height: Px.px(640)
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
        if (root.heldPad >= 0) {
            Mixer.releaseSamplerPad(root.targetRow, root.targetSlot, root.heldPad)
            root.heldPad = -1
        }
    }

    function refresh() {
        const snap = Mixer.insertSamplerSnapshot(root.targetRow, root.targetSlot)
        if (!snap || snap.pads === undefined) return
        root.focused = snap.focused
        root.recording = snap.recording
        root.recPad = snap.recPad
        root.gain = snap.gain
        root.quantize = snap.quantize
        root.sounding = snap.sounding
        root.pads = snap.pads
        root.peaks = Mixer.samplerWaveform(root.targetRow, root.targetSlot,
                                           root.focused, 64)
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
                text: root.recording ? qsTr("recording…") : qsTr("")
                color: Skin.arm
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
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Load")
                tip: qsTr("A file onto the focused pad. Rec does the same from the strip.")
                onClicked: loadDialog.open()
            }
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Clear")
                danger: true
                tip: qsTr("Throw away the focused pad.")
                onClicked: {
                    Mixer.clearSamplerPad(root.targetRow, root.targetSlot, root.focused)
                    root.refresh()
                }
            }
            StripButton {
                Layout.preferredWidth: Px.px(56)
                label: qsTr("Rec")
                activeColor: Skin.arm
                active: root.recording
                danger: true
                tip: qsTr("Record the strip's input onto the focused pad. Press again to close the take.")
                onClicked: {
                    Mixer.setSamplerRecord(root.targetRow, root.targetSlot, !root.recording)
                    root.recording = !root.recording
                }
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
                        border.width: cell.isFocused ? Px.px(2) : 1
                        border.color: cell.isRecording ? Skin.arm
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
                        text: qsTr("%1 — MIDI %2. Tap to play, Rec to capture the strip.")
                              .arg(cell.info.name).arg(cell.info.note)
                        visible: hover.hovered
                    }

                    HoverHandler { id: hover }

                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onPressedChanged: {
                            if (pressed) {
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

        Item {
            id: wave
            Layout.fillWidth: true
            Layout.preferredHeight: Px.px(56)

            Rectangle {
                anchors.fill: parent
                color: Skin.slotEmpty
                radius: Skin.radius
                border.width: 1
                border.color: Skin.border
            }

            Row {
                id: bars
                anchors.fill: parent
                anchors.margins: 1
                spacing: 0

                Repeater {
                    model: root.peaks

                    Rectangle {
                        required property int index
                        required property var modelData
                        width: bars.width / Math.max(1, root.peaks.length)
                        height: Math.max(1, parent.height
                                         * Math.min(1, Number(modelData)))
                        anchors.bottom: parent.bottom
                        color: Skin.accent
                        opacity: 0.75
                    }
                }
            }

            Text {
                visible: root.peaks.length === 0
                anchors.centerIn: parent
                text: qsTr("empty")
                color: Skin.disabled
                font.pixelSize: Skin.fontS
            }
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
                tip: qsTr("One-shot plays to the end. Hold stops when the note does.")
                onClicked: root.writePad({ oneShot: !root.focusedPad.oneShot })
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
            tip: qsTr("Pad volume.")
            onMoved: (v) => root.writePad({ volume: v * 2 })
        }
        ValueTrack {
            Layout.fillWidth: true
            label: qsTr("pan")
            value: (root.focusedPad.pan + 1) * 0.5
            valueText: root.focusedPad.pan.toFixed(2)
            tip: qsTr("Pad pan.")
            onMoved: (v) => root.writePad({ pan: v * 2 - 1 })
        }
        ValueTrack {
            Layout.fillWidth: true
            label: qsTr("pitch")
            value: (root.focusedPad.pitch + 24) / 48
            valueText: (root.focusedPad.pitch >= 0 ? "+" : "")
                       + Math.round(root.focusedPad.pitch)
            tip: qsTr("Pad pitch, in semitones.")
            onMoved: (v) => root.writePad({ pitch: Math.round(v * 48 - 24) })
        }
        ValueTrack {
            Layout.fillWidth: true
            label: qsTr("gain")
            value: Math.max(0, Math.min(1, root.gain / 2))
            valueText: root.gain.toFixed(2)
            tip: qsTr("Master gain for every pad.")
            onMoved: (v) => {
                Mixer.setInsertParameter(root.targetRow, root.targetSlot, 0, v * 2)
                root.gain = v * 2
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacing
            ValueTrack {
                Layout.fillWidth: true
                label: qsTr("start")
                value: root.focusedPad.start
                valueText: root.focusedPad.start.toFixed(2)
                tip: qsTr("Where playback begins in the take.")
                onMoved: (v) => Mixer.setSamplerTrim(
                    root.targetRow, root.targetSlot, root.focused,
                    v, root.focusedPad.end)
            }
            ValueTrack {
                Layout.fillWidth: true
                label: qsTr("end")
                value: root.focusedPad.end
                valueText: root.focusedPad.end.toFixed(2)
                tip: qsTr("Where playback stops in the take.")
                onMoved: (v) => Mixer.setSamplerTrim(
                    root.targetRow, root.targetSlot, root.focused,
                    root.focusedPad.start, v)
            }
        }
    }
}
