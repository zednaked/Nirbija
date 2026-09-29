pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-only

import QtQuick
import QtQuick.Layouts
import Nirbija

// One channel, in AUM's order from top to bottom: input node, fader with mute
// and solo beside it, record-arm, the scrollable insert chain, the output node,
// and the channel title at the foot.
//
// Laid out by ColumnLayout rather than by arithmetic. The insert list used to
// take its height from a subtraction over five siblings — the title, the output
// slot, the number of sends — recomputed by hand in a binding, which meant
// every new row in the strip was a change to that sum as well. Here it simply
// takes what is left.
Rectangle {
    id: root

    property int row: 0
    property string channelName: ""
    property real gain: 1.0
    property real pan: 0
    property bool muted: false
    property bool soloed: false
    property bool armed: false
    property real positionLeft: 0
    property real positionRight: 0
    property real holdLeft: 0
    property real holdRight: 0
    property string inputLabel: ""
    property string midiLabel: ""
    property bool inputConnected: false
    property bool midiConnected: false
    property string outputLabel: ""
    // [{name, filled, missing, bypassed, postFader, ...}] - see
    // MixerModel::InsertDetailsRole. `filled` is false for a hole left by a
    // removal, which is a slot to fill, not a plugin to open.
    property var inserts: []
    property bool isBus: false
    property var sends: []
    property color accent: Skin.accent
    // Scenes: see MixerModel::SceneMarksRole for the bits.
    property bool followScenes: true
    property bool sceneOn: true
    property int sceneMarks: 0
    property int sceneHands: 0

    // A small dot beside a control: filled in the strip's colour when the
    // scene in view holds it, an amber ring when the player has taken it from
    // the scene - a ring, since a strip's own colour can be amber too.
    // An inline component cannot see this file's ids, so it is handed what
    // it draws.
    component SceneDot: Rectangle {
        property int bit: 1
        property int marks: 0
        property int hands: 0
        property color tint: Skin.accent
        readonly property bool held: (hands & bit) !== 0
        width: Px.px(6)
        height: width
        radius: width / 2
        visible: held || (marks & bit) !== 0
        color: held ? "transparent" : tint
        border.width: held ? Px.px(1.5) : Mixer.sceneRecording ? 1 : 0
        border.color: held ? Skin.solo : Skin.arm
    }

    signal insertSlotClicked(int slot, var item)
    signal insertMenuRequested(int slot, var item)
    signal titleClicked(var item)
    signal inputSlotClicked(var item)
    signal midiSlotClicked(var item)
    signal inputMenuRequested(var item)
    signal midiMenuRequested(var item)
    signal outputSlotClicked(var item)
    signal sendLevelRequested(int slot, real level)
    signal sendMenuRequested(int slot, var item)

    // How far one strip's left edge sits from the next one's, in the Row that
    // lays these out - the same width and spacing that Row itself uses, so a
    // drag past half of it lands past the neighbour's own midpoint.
    readonly property real stripStep: Skin.stripWidth + Skin.gap
    // Set from the title's DragHandler below. A transform rather than `x`
    // itself: `x` here belongs to the parent Row, which repositions every
    // strip on every reorder - fighting it for the same property is what
    // broke the meter's mask (see Meter.qml). A transform rides on top of
    // whatever the Row decides without contesting it.
    property real dragOffsetX: 0
    property bool dragging: false

    width: Skin.stripWidth
    color: stripHover.hovered ? Skin.stripAlt : Skin.strip
    radius: Skin.radius
    z: root.dragging ? 10 : 0
    transform: Translate { x: root.dragOffsetX }

    Behavior on color {
        ColorAnimation { duration: Skin.medium }
    }
    Behavior on dragOffsetX {
        enabled: !root.dragging
        NumberAnimation { duration: Skin.fast; easing.type: Easing.OutQuad }
    }

    HoverHandler {
        id: stripHover
    }

    // The accent is a stripe rather than a fill, so a dozen strips side by side
    // stay readable. Broken into dashes on a strip the scenes leave alone.
    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Px.px(3)
        radius: Skin.radiusS
        color: root.accent
        visible: root.followScenes
    }
    Row {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Px.px(3)
        spacing: Px.px(4)
        clip: true
        visible: !root.followScenes
        Repeater {
            model: Math.ceil(root.width / Px.px(10))
            Rectangle {
                width: Px.px(6)
                height: Px.px(3)
                radius: Skin.radiusS
                color: root.accent
            }
        }
    }

    // The player has hold of something a scene wanted: until the next scene
    // starts, that control is theirs.
    Rectangle {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: Px.px(6)
        anchors.rightMargin: Px.px(6)
        width: handText.implicitWidth + Px.px(8)
        height: handText.implicitHeight + Px.px(2)
        radius: Skin.radiusS
        color: Skin.solo
        visible: root.sceneHands !== 0
        z: 5
        Text {
            id: handText
            anchors.centerIn: parent
            text: qsTr("HAND")
            color: Skin.onAccent
            font.pixelSize: Skin.fontXS
            font.bold: true
            font.letterSpacing: Px.px(1)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Skin.gap
        anchors.topMargin: Skin.gap + Px.px(3)
        spacing: Skin.gap

        // A bus is fed by the strips pointed at it, so it has nothing to pick
        // an input from.
        NodeSlot {
            visible: !root.isBus
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? Skin.slotHeight : 0
            label: root.inputLabel
            tip: qsTr("Audio input for this channel. Click to pick a source; right-click for the full list.")
            filled: root.inputConnected
            position: Math.max(root.positionLeft, root.positionRight)
            onClicked: root.inputSlotClicked(this)
            onMenuRequested: root.inputMenuRequested(this)
        }

        // MIDI gets its own node slot rather than hiding in a menu: on this
        // mixer any channel can host a synth, so any channel can want MIDI.
        NodeSlot {
            visible: !root.isBus
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? Skin.slotHeight : 0
            label: root.midiLabel
            tip: qsTr("MIDI input. Click to pick a source; right-click to filter which MIDI channels get through.")
            filled: root.midiConnected
            position: 0
            onClicked: root.midiSlotClicked(this)
            onMenuRequested: root.midiMenuRequested(this)
        }

        MidiKeyboard {
            visible: !root.isBus
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? Px.px(56) : 0
            targetRow: root.row
        }

        // --- fader, mute and solo -------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: Px.px(150)
            Layout.preferredHeight: Px.px(200)
            // Past about this, a taller fader is no easier to set, while the
            // insert chain is always easier to read with another slot showing.
            Layout.maximumHeight: Px.px(250)
            spacing: Skin.gap

            Fader {
                Layout.fillHeight: true
                Layout.preferredWidth: Px.px(58)
                gain: root.gain
                positionLeft: root.positionLeft
                positionRight: root.positionRight
                holdLeft: root.holdLeft
                holdRight: root.holdRight
                accent: root.accent
                onGainRequested: value => Mixer.setGain(root.row, value)
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignTop
                spacing: Skin.gap

                StripButton {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Px.px(26)
                    label: "M"
                    tip: qsTr("Mute this channel.")
                    active: root.muted
                    activeColor: Skin.mute
                    onClicked: Mixer.toggleMute(root.row)
                }

                StripButton {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Px.px(26)
                    label: "S"
                    tip: qsTr("Solo. While anything is soloed, only soloed strips reach the master.")
                    active: root.soloed
                    activeColor: Skin.solo
                    onClicked: Mixer.toggleSolo(root.row)
                }

                StripButton {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Px.px(26)
                    label: "R"
                    tip: qsTr("Arm for recording. Which tracks get recorded is decided when recording starts, so arming mid-take does nothing until the next one.")
                    active: root.armed
                    activeColor: Skin.arm
                    onClicked: Mixer.toggleArm(root.row)
                }

                // The strip's own on/off, the switch a scene fades: apart
                // from M, which stays the player's.
                StripButton {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Px.px(26)
                    label: root.sceneOn ? qsTr("ON") : qsTr("OFF")
                    tip: root.followScenes
                         ? qsTr("On or off for the scenes. A scene fades this over its bars; while recording a scene, pressing it goes into the scene.")
                         : qsTr("On or off. This strip ignores scenes, so only you switch it.")
                    active: !root.sceneOn
                    activeColor: Skin.disabled
                    onClicked: Mixer.toggleSceneOn(root.row)

                    SceneDot {
                        bit: 1
                        marks: root.sceneMarks
                        hands: root.sceneHands
                        tint: root.accent
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.margins: Px.px(3)
                    }
                }

                Item {
                    Layout.fillWidth: true
                    implicitHeight: gainText.implicitHeight

                    Text {
                        id: gainText
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: Mixer.gainLabel(root.gain)
                        color: Skin.textDim
                        font.pixelSize: Skin.fontS
                        font.family: Skin.monoFamily
                    }
                    SceneDot {
                        bit: 2
                        marks: root.sceneMarks
                        hands: root.sceneHands
                        tint: root.accent
                        anchors.verticalCenter: gainText.verticalCenter
                        anchors.right: gainText.left
                        anchors.rightMargin: Px.px(3)
                    }
                }

                PanControl {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Px.px(18)
                    pan: root.pan
                    onPanRequested: value => Mixer.setPan(root.row, value)
                }

                Item {
                    Layout.fillHeight: true
                }
            }
        }

        // --- insert chain ----------------------------------------------------
        // Scrollable, with a few empty slots always waiting at the bottom,
        // which is how AUM lets a chain grow without a separate "add" step.
        ListView {
            id: insertList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: Skin.slotHeight + Skin.gap
            Layout.preferredHeight: (root.inserts.length + 1)
                                    * (Skin.slotHeight + Skin.gap)
            clip: true
            spacing: Skin.gap
            model: root.inserts.length + 3
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true

            delegate: InsertSlot {
                id: slot
                required property int index

                width: insertList.width
                readonly property bool present: index < root.inserts.length
                                                && root.inserts[index].filled === true
                pluginName: slot.present ? root.inserts[index].name : ""
                missing: slot.present && root.inserts[index].missing === true
                uid: slot.present && root.inserts[index].uid !== undefined
                     ? root.inserts[index].uid : ""
                bypassed: index < root.inserts.length
                          && root.inserts[index].bypassed === true
                postFader: index < root.inserts.length
                           && root.inserts[index].postFader === true
                looperRecording: index < root.inserts.length
                                 && root.inserts[index].looperRecording === true
                looperWriting: index < root.inserts.length
                               && root.inserts[index].looperWriting === true
                looperPlaying: index < root.inserts.length
                               && root.inserts[index].looperPlaying === true
                looperHasAudio: index < root.inserts.length
                                && root.inserts[index].looperHasAudio === true
                samplerRecording: index < root.inserts.length
                                  && root.inserts[index].samplerRecording === true
                sequencerRecording: index < root.inserts.length
                                    && root.inserts[index].sequencerRecording === true
                onClicked: root.insertSlotClicked(slot.index, slot)
                onMenuRequested: {
                    if (slot.present) root.insertMenuRequested(slot.index, slot)
                }
            }
        }

        // Sends sit just above the output, which is where they leave from.
        Repeater {
            model: root.sends

            SendRow {
                id: send
                required property int index
                required property var modelData

                Layout.fillWidth: true
                Layout.preferredHeight: Px.px(20)
                busName: modelData.name
                level: modelData.level
                onLevelRequested: value => root.sendLevelRequested(send.index, value)
                onMenuRequested: root.sendMenuRequested(send.index, send)
            }
        }

        NodeSlot {
            Layout.fillWidth: true
            Layout.preferredHeight: Skin.slotHeight
            label: root.outputLabel
            tip: qsTr("Where this strip sends its output: the master, a bus, or a later channel.")
            position: Math.max(root.positionLeft, root.positionRight)
            onClicked: root.outputSlotClicked(this)
            onMenuRequested: root.outputSlotClicked(this)
        }

        // The channel title is where AUM keeps renaming and removal, so it is
        // a button rather than a label. A sideways drag on it reorders the
        // strip instead: the same handle for both, since AUM users already
        // reach for the title to do anything strip-level.
        StripButton {
            id: title
            Layout.fillWidth: true
            Layout.preferredHeight: Px.px(20)
            label: root.channelName
            flat: true
            tip: qsTr("%1 — click to rename this strip, duplicate it, or remove it; drag sideways to move it.").arg(root.channelName)
            onClicked: root.titleClicked(title)

            DragHandler {
                id: reorderDrag
                target: null
                xAxis.enabled: true
                yAxis.enabled: false
                // Reordering the model mid-drag (one call per strip crossed)
                // is what made the drag jump around: each call needed `row`
                // read back from the Repeater's `index`, and that binding
                // does not necessarily settle before the pointer moves
                // again, so a fast drag applied a step to whatever row was
                // still hanging around from the previous one. Instead this
                // only tracks how many strips the drag is currently over -
                // the strip itself just floats on the transform below,
                // following the pointer 1:1 - and the real reorder happens
                // once, on release, in one call.
                property int pendingSteps: 0

                onActiveChanged: {
                    root.dragging = active
                    if (active) {
                        pendingSteps = 0
                        Mixer.beginChannelReorder()
                    } else {
                        root.dragOffsetX = 0
                        if (pendingSteps !== 0)
                            Mixer.moveChannelLiveBy(root.row, pendingSteps)
                        Mixer.endChannelReorder()
                    }
                }

                onCentroidChanged: {
                    if (!active) return
                    const step = root.stripStep
                    const minRow = -root.row
                    const maxRow = Mixer.count - 1 - root.row
                    const travelled = centroid.position.x - centroid.pressPosition.x
                    // A half-step of "give" past the last strip it can still
                    // promise to land on, so the drag does not feel like it
                    // hit a hard wall the instant it runs out of neighbours.
                    root.dragOffsetX = Math.max(minRow * step - step / 2,
                                       Math.min(maxRow * step + step / 2, travelled))
                    pendingSteps = Math.max(minRow, Math.min(maxRow,
                                            Math.round(root.dragOffsetX / step)))
                }
            }
        }
    }
}
