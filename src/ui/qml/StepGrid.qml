pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// The step sequencer's own editor. The generic one renders it as eighty-five
// sliders, which proves the plugin works and is no way to play it.
//
// One column per step: the bar's height is the note, tapping it turns the step
// on or off, and the thin strip at the foot is how hard it hits. The step the
// audio thread is on lights as it goes past.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property string pluginName: ""

    // Ids the plugin publishes. Kept here rather than looked up by name so the
    // grid does not depend on how a parameter happens to be labelled.
    readonly property int idDivision: 0
    readonly property int idLength: 1
    readonly property int idGate: 2
    readonly property int idTranspose: 3
    readonly property int idChannel: 4
    readonly property int idStepNote: 16
    readonly property int idStepVelocity: 48
    readonly property int idStepActive: 80
    readonly property int stepCount: 16

    // The window the grid draws: three octaves is enough to write a line in
    // and few enough that one step is a comfortable target.
    readonly property int lowNote: 36
    readonly property int highNote: 84

    // Mirrors of the plugin's values, so a drag repaints without a round trip
    // through the model on every pixel.
    property var notes: []
    property var velocities: []
    property var actives: []
    property int division: 2
    property int length: 16
    property real gate: 0.5
    property int transpose: 0
    property int playhead: -1

    width: Skin.px(620)
    height: Skin.px(430)
    modal: true
    anchors.centerIn: Overlay.overlay
    padding: Skin.spacingL
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Skin.popup
        border.width: 1
        border.color: Skin.border
        radius: Skin.radiusL
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
        for (let i = 0; i < root.stepCount; ++i) {
            notes.push(byId[root.idStepNote + i])
            velocities.push(byId[root.idStepVelocity + i])
            actives.push(byId[root.idStepActive + i] >= 0.5)
        }
        root.notes = notes
        root.velocities = velocities
        root.actives = actives
        root.division = byId[root.idDivision]
        root.length = byId[root.idLength]
        root.gate = byId[root.idGate]
        root.transpose = byId[root.idTranspose]
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

    // Copy-on-write so the change reaches the delegates: mutating in place
    // leaves the binding thinking nothing happened.
    function setStep(list, index, value) {
        const copy = list.slice()
        copy[index] = value
        return copy
    }

    // A note number as something readable. Sharps only; nobody is spelling
    // enharmonics on a step grid.
    function noteName(midi) {
        const names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
        return names[Math.round(midi) % 12] + (Math.floor(Math.round(midi) / 12) - 1)
    }

    // Only while it is on screen and only while the transport moves, so a shut
    // grid costs nothing.
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
            spacing: Skin.spacing

            Text {
                text: root.pluginName
                color: Skin.text
                font.pixelSize: Skin.fontL
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            Text {
                text: qsTr("tap a step to switch it on · drag it to pitch it")
                color: Skin.textDim
                font.pixelSize: Skin.fontS
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
                spacing: Skin.px(2)

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

                        width: (columns.width - (root.stepCount - 1) * Skin.px(2))
                               / root.stepCount
                        height: columns.height
                        opacity: column.inPattern ? 1.0 : 0.3

                        // Every fourth column is a downbeat, which is what makes
                        // a pattern readable at a glance.
                        Rectangle {
                            anchors.fill: parent
                            color: column.index % 4 === 0 ? Skin.strip : "transparent"
                            radius: Skin.radiusS
                        }

                        // The playing column, drawn behind the bar so it reads
                        // as a light rather than as a border.
                        Rectangle {
                            anchors.fill: parent
                            radius: Skin.radiusS
                            color: Skin.accent
                            opacity: column.atPlayhead ? 0.22 : 0
                            Behavior on opacity {
                                NumberAnimation { duration: Skin.fast }
                            }
                        }

                        // The note. Height is pitch, so a melody is a shape.
                        Rectangle {
                            id: bar
                            readonly property real fraction:
                                (column.note - root.lowNote) /
                                (root.highNote - root.lowNote)

                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.margins: Skin.px(3)
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: velocityStrip.height + Skin.px(4)
                            height: Math.max(Skin.px(4),
                                             Math.min(1, Math.max(0, bar.fraction)) *
                                             (column.height - velocityStrip.height
                                              - Skin.px(8)))
                            radius: Skin.radiusS
                            color: column.on ? Skin.accent : Skin.slot
                            opacity: column.on ? 0.55 + 0.45 * (column.velocity / 127)
                                               : 0.5

                            Behavior on color {
                                ColorAnimation { duration: Skin.fast }
                            }
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: bar.top
                            anchors.bottomMargin: Skin.px(2)
                            visible: column.on && column.width > Skin.px(26)
                            text: root.noteName(column.note)
                            color: Skin.textDim
                            font.pixelSize: Skin.fontXS
                        }

                        // How hard the step hits, on its own strip so a drag
                        // for pitch can never change it by accident.
                        Rectangle {
                            id: velocityStrip
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: Skin.px(3)
                            height: Skin.px(10)
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

                        // Tap toggles, drag pitches. A DragHandler and a
                        // TapHandler negotiate the gesture between them, so a
                        // tap that wanders a pixel is still a tap.
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
                                // A full column height is the whole range, so
                                // the gesture is the same size as the picture.
                                const span = root.highNote - root.lowNote
                                const moved = -pitchDrag.translation.y
                                              / column.height * span
                                const value = Math.round(Math.max(root.lowNote,
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

        // --- the knobs that are not per-step ----------------------------------
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Skin.spacing
            rowSpacing: Skin.spacingXS

            ValueTrack {
                Layout.fillWidth: true
                Layout.preferredHeight: Skin.px(22)
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
                Layout.preferredHeight: Skin.px(22)
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
                Layout.preferredHeight: Skin.px(22)
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
                Layout.preferredHeight: Skin.px(22)
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
        }
    }
}
