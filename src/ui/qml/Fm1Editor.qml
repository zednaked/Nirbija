pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-only

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// The FM-1 with Jangada, seen from the mixer (design/fm1.md). The sound is
// made in the box; this is a window onto its four tracks: each one's engine
// and the parameters a hand reaches for, as bars that move the device as they
// move. What the session keeps of the FM-1 - its whole project - is shown on
// the right, with what it is waiting for when it cannot give it back yet.
//
// Everything is read from the device through the snapshot on every tick: a
// knob turned on the FM-1 moves its bar here, an engine changed there
// changes the bars.
EditorPopup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property var snap: ({})
    property int track: 0
    // Values the hand set here, kept for a moment so the bar does not jump
    // back to the device's last report while the next one is on its way.
    property var held: ({})

    // One hue per track, the same as on the device's TRACKS page order.
    readonly property var trackHues: [Skin.accent, "#d9677a", "#e5c452", "#45c17a"]
    readonly property color hue: root.trackHues[root.track]

    readonly property var tracks: root.snap.tracks || []
    readonly property var current: root.tracks.length > root.track ? root.tracks[root.track] : null
    readonly property bool connected: root.snap.state === "connected"
    readonly property int soundNow: root.current ? (root.current.sound || 0) : 0

    function soundLabel(index) {
        const labels = root.snap.sounds || []
        return "#" + index + " " + (index - 1 < labels.length ? labels[index - 1] : "")
    }

    function refresh() {
        root.snap = Mixer.insertFm1Snapshot(root.targetRow, root.targetSlot)
        const now = Date.now()
        const kept = {}
        for (const id in root.held)
            if (now - root.held[id].at < 400) kept[id] = root.held[id]
        root.held = kept
    }

    function valueOf(param) {
        const h = root.held[param.id]
        return h !== undefined ? h.value : param.value
    }

    function setP(param, value) {
        const v = Math.max(param.min, Math.min(param.max, Math.round(value)))
        Mixer.setFm1Param(root.targetRow, root.targetSlot, param.id, v)
        const next = Object.assign({}, root.held)
        next[param.id] = { value: v, at: Date.now() }
        root.held = next
    }

    function readout(param) {
        const v = root.valueOf(param)
        if (param.names && param.names.length > 0) {
            const i = v - param.min
            return i >= 0 && i < param.names.length ? param.names[i] : String(v)
        }
        return (param.min < 0 && v > 0 ? "+" : "") + v
    }

    // The shown parameters of the current track by group, in the device's
    // order: keys 49..56 are the engine's EDIT, 1..4 the envelope, 33..36 the
    // sends, 9..10 the LFO.
    function group(keys) {
        if (!root.current) return []
        return root.current.params.filter(p => keys.indexOf(p.key) >= 0)
    }

    width: Px.px(980)
    height: Px.px(676)
    holdOpen: Mixer.learning
    glowHue: root.hue
    glowOpacity: root.connected ? 0.14 : 0.0
    eatsWheel: false

    function openFor(row, slot) {
        root.targetRow = row
        root.targetSlot = slot
        root.held = ({})
        root.refresh()
        root.place()
        root.open()
    }

    Connections {
        target: Mixer
        enabled: root.visible
        function onTick() { root.refresh() }
    }

    contentItem: ColumnLayout {
        spacing: Skin.spacing

        // --- header ------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingS

            Text {
                text: qsTr("FM-1")
                color: Skin.text
                font.pixelSize: Skin.fontL
                font.bold: true
                font.letterSpacing: Px.px(2)
            }

            Item { Layout.preferredWidth: Skin.spacing }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Skin.buttonHeight + Px.px(6)
                radius: Skin.radius
                color: Skin.slotEmpty
                border.width: 1
                border.color: Skin.border

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: Skin.spacing
                    spacing: Skin.spacing
                    Rectangle {
                        width: Px.px(8)
                        height: Px.px(8)
                        radius: width / 2
                        anchors.verticalCenter: parent.verticalCenter
                        color: root.connected ? Skin.meterLow
                             : root.snap.state === "outdated" ? Skin.solo : Skin.disabled
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        color: Skin.text
                        font.pixelSize: Skin.font
                        font.bold: true
                        font.letterSpacing: Px.px(1)
                        text: root.connected ? qsTr("CONNECTED")
                            : root.snap.state === "outdated" ? qsTr("JANGADA TOO OLD")
                            : qsTr("NOT CONNECTED")
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        color: Skin.textDim
                        font.pixelSize: Skin.fontS
                        text: root.connected ? (root.snap.version || "")
                            : root.snap.state === "outdated"
                              ? qsTr("%1: update to Jangada 1.0.1 or later").arg(root.snap.version || "")
                              : qsTr("plug the FM-1 in by USB; this strip finds it by itself")
                    }
                }
            }

            StripButton {
                Layout.preferredHeight: Skin.buttonHeight + Px.px(6)
                label: qsTr("READ FROM FM-1")
                enabled: root.connected
                tip: qsTr("Take the FM-1's project into the session now: sounds, patterns and mix of the four tracks. It is also read by itself a little after the FM-1 changes.")
                onClicked: Mixer.fm1ReadNow(root.targetRow, root.targetSlot)
            }
            StripButton {
                Layout.preferredHeight: Skin.buttonHeight + Px.px(6)
                label: qsTr("SEND BACK")
                enabled: (root.snap.bytes || 0) > 0
                tip: qsTr("Give the FM-1 the session's project again, over whatever it has now. Opening the session does this by itself.")
                onClicked: Mixer.fm1SendProject(root.targetRow, root.targetSlot)
            }
        }

        // --- the four tracks ---------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingS
            Repeater {
                model: 4
                delegate: Rectangle {
                    id: tab
                    required property int index
                    readonly property var info: root.tracks.length > tab.index ? root.tracks[tab.index] : null
                    readonly property bool chosen: root.track === tab.index
                    Layout.fillWidth: true
                    Layout.preferredHeight: Px.px(46)
                    radius: Skin.radius
                    color: tab.chosen ? Skin.stripAlt : Skin.strip
                    border.width: 1
                    border.color: tab.chosen ? root.trackHues[tab.index] : Skin.border

                    Rectangle {
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: Px.px(3)
                        radius: Skin.radius
                        color: root.trackHues[tab.index]
                    }
                    Column {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Skin.spacing
                        spacing: Px.px(2)
                        Text {
                            text: (tab.index + 1) + " · " + (tab.info && tab.info.name ? tab.info.name : "—")
                            color: Skin.text
                            font.pixelSize: Skin.font
                            font.bold: true
                            font.letterSpacing: Px.px(1)
                        }
                        Text {
                            text: qsTr("MIDI ch %1").arg(tab.index + 1)
                                  + (root.snap.selected === tab.index && root.connected ? qsTr(" · on the FM-1's screen") : "")
                            color: Skin.textDim
                            font.pixelSize: Skin.fontXS
                            font.family: Skin.monoFamily
                        }
                    }
                    TapHandler { onTapped: root.track = tab.index }
                }
            }
        }

        // --- the bars and the session ------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Skin.spacingL

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Skin.spacing

                // The track's whole sound, for the scenes: which stored one its
                // SOUND names, a capture of what it plays now, the list.
                RowLayout {
                    visible: root.track < 3 && root.current !== null && root.current.params.length > 0
                    Layout.fillWidth: true
                    spacing: Skin.spacingS

                    Text {
                        text: qsTr("SOUND")
                        color: Skin.textDim
                        font.pixelSize: Skin.fontXS
                        font.bold: true
                        font.letterSpacing: Px.px(1.5)
                    }
                    Rectangle {
                        Layout.preferredWidth: Px.px(150)
                        Layout.preferredHeight: Skin.buttonHeight
                        radius: Skin.radius
                        color: Skin.slotEmpty
                        border.width: 1
                        border.color: root.soundNow > 0 ? root.hue : Skin.border
                        Text {
                            anchors.centerIn: parent
                            color: root.soundNow > 0 ? Skin.text : Skin.textDim
                            font.pixelSize: Skin.fontS
                            font.family: Skin.monoFamily
                            text: root.soundNow > 0 ? root.soundLabel(root.soundNow) : qsTr("none set")
                        }
                    }
                    StripButton {
                        label: qsTr("CAPTURE")
                        enabled: root.connected
                        tip: qsTr("Keep the sound this track plays now: it joins the session's sounds and becomes the track's SOUND. With Record scene lit, the scene takes it and plays it on its bar line.")
                        onClicked: Mixer.fm1CaptureSound(root.targetRow, root.targetSlot, root.track)
                    }
                    StripButton {
                        id: soundsButton
                        label: qsTr("SOUNDS")
                        enabled: (root.snap.sounds || []).length > 0
                        tip: qsTr("Put one of the session's sounds on this track now.")
                        onClicked: {
                            const entries = []
                            const labels = root.snap.sounds || []
                            for (let i = 0; i < labels.length; ++i) {
                                const index = i + 1
                                entries.push({ label: root.soundLabel(index),
                                               action: () => Mixer.fm1UseSound(root.targetRow, root.targetSlot, root.track, index) })
                            }
                            soundMenu.openAt(soundsButton, entries, qsTr("Sounds"))
                        }
                    }
                    SlotMenu {
                        id: soundMenu
                        parent: Overlay.overlay
                    }
                    Item { Layout.fillWidth: true }
                }

                Text {
                    visible: !root.current || root.current.params.length === 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.WordWrap
                    color: Skin.textDim
                    font.pixelSize: Skin.font
                    text: root.connected ? qsTr("Reading the FM-1…")
                                         : qsTr("The bars appear when the FM-1 is connected.")
                }

                Repeater {
                    model: [
                        { title: root.current && root.current.drum ? "" : (root.current ? root.current.name : ""),
                          note: qsTr("the eight of EDIT, as on the device"), keys: [49, 50, 51, 52, 53, 54, 55, 56], tall: true },
                        { title: qsTr("ENVELOPE"), note: "", keys: [1, 2, 3, 4], tall: false },
                        { title: qsTr("FX SENDS"), note: root.current && root.current.drum
                              ? qsTr("the drum track: its kit levels are not on the protocol yet") : "",
                          keys: [33, 34, 35, 36], tall: false },
                        { title: qsTr("LFO"), note: "", keys: [9, 10], tall: false }
                    ]
                    delegate: ColumnLayout {
                        id: groupBox
                        required property var modelData
                        readonly property var params: root.group(groupBox.modelData.keys)
                        visible: groupBox.params.length > 0
                        Layout.fillWidth: true
                        spacing: Skin.spacingXS

                        Row {
                            spacing: Skin.spacing
                            Text {
                                text: groupBox.modelData.title
                                color: Skin.textDim
                                font.pixelSize: Skin.fontXS
                                font.bold: true
                                font.letterSpacing: Px.px(1.5)
                            }
                            Text {
                                text: groupBox.modelData.note
                                color: Skin.disabled
                                font.pixelSize: Skin.fontXS
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Skin.spacingS
                            Repeater {
                                model: groupBox.params
                                delegate: ParamBar {
                                    id: bar
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: Px.px(96)
                                    Layout.preferredHeight: groupBox.modelData.tall ? Px.px(128) : Px.px(76)
                                    label: bar.modelData.name
                                    hue: root.hue
                                    bipolar: bar.modelData.min < 0
                                    big: groupBox.modelData.tall
                                    mapped: bar.modelData.mapped === true
                                    value: (root.valueOf(bar.modelData) - bar.modelData.min)
                                           / Math.max(1, bar.modelData.max - bar.modelData.min)
                                    readout: root.readout(bar.modelData)
                                    step: 1 / Math.max(1, bar.modelData.max - bar.modelData.min)
                                    fineStep: bar.step
                                    tip: qsTr("Track %1 %2: drag, roll the wheel, or turn it on the FM-1.")
                                         .arg(root.track + 1).arg(bar.modelData.name)
                                    onEdited: v => root.setP(bar.modelData,
                                        bar.modelData.min + v * (bar.modelData.max - bar.modelData.min))
                                }
                            }
                            Item { Layout.fillWidth: true }
                        }
                    }
                }
                Item { Layout.fillHeight: true }
            }

            // --- what the session keeps ----------------------------------------
            Rectangle {
                Layout.preferredWidth: Px.px(230)
                Layout.alignment: Qt.AlignTop
                implicitHeight: sessionColumn.implicitHeight + 2 * Skin.spacingL
                radius: Skin.radius
                color: Skin.strip
                border.width: 1
                border.color: Skin.border

                ColumnLayout {
                    id: sessionColumn
                    anchors.fill: parent
                    anchors.margins: Skin.spacingL
                    spacing: Skin.spacingS

                    Text {
                        text: qsTr("IN THE SESSION")
                        color: Skin.textDim
                        font.pixelSize: Skin.fontXS
                        font.bold: true
                        font.letterSpacing: Px.px(1.5)
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Skin.text
                        font.pixelSize: Skin.font
                        text: (root.snap.bytes || 0) > 0
                              ? qsTr("The FM-1's whole project: sounds, patterns and mix of the four tracks.")
                              : qsTr("Nothing yet: the project is read once the FM-1 is connected.")
                    }
                    Text {
                        visible: (root.snap.bytes || 0) > 0
                        color: Skin.textDim
                        font.pixelSize: Skin.fontS
                        font.family: Skin.monoFamily
                        text: ((root.snap.bytes || 0) / 1024).toFixed(1) + " KB"
                              + (root.snap.readAt ? qsTr(" · read %1").arg(root.snap.readAt) : "")
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: Skin.fontS
                        visible: text.length > 0
                        color: root.snap.put === "waiting" || root.snap.put === "refused" ? Skin.solo : Skin.textDim
                        text: root.snap.put === "waiting" ? qsTr("Stop the FM-1's song to give it the session's project.")
                            : root.snap.put === "pending" && !root.connected ? qsTr("Gives the FM-1 the session's project when it is back.")
                            : root.snap.put === "pending" ? qsTr("Giving the FM-1 the session's project…")
                            : root.snap.put === "refused" ? qsTr("The FM-1 refused the session's project. A project from newer firmware?")
                            : root.snap.put === "done" ? qsTr("The FM-1 has the session's project.")
                            : ""
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Skin.disabled
                        font.pixelSize: Skin.fontXS
                        text: qsTr("Saving the session keeps the latest read; opening it gives the project back, stopped.")
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            color: Skin.textDim
            font.pixelSize: Skin.fontXS
            text: qsTr("Drag a bar: the FM-1 hears it at once. With Record scene lit, what you move goes into the scene.")
        }
    }
}
