import QtQuick

// One channel, in AUM's order from top to bottom: input node, fader with mute
// and solo beside it, record-arm, the scrollable insert chain, the output node,
// and the channel title at the foot.
Rectangle {
    id: root

    property int row: 0
    property string channelName: ""
    property real gain: 1.0
    property bool muted: false
    property bool soloed: false
    property bool armed: false
    property real peakLeft: 0
    property real peakRight: 0
    property string inputLabel: ""
    property string midiLabel: ""
    property string outputLabel: ""
    property var inserts: []
    property bool isBus: false
    property var sends: []
    property color accent: Skin.accent

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

    width: Skin.stripWidth
    color: Skin.strip
    radius: Skin.radius

    // The accent is a stripe rather than a fill, so a dozen strips side by side
    // stay readable.
    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 3
        radius: Skin.radius
        color: root.accent
    }

    Column {
        anchors.fill: parent
        anchors.margins: Skin.gap
        anchors.topMargin: Skin.gap + 3
        spacing: Skin.gap

        // A bus is fed by the strips pointed at it, so it has nothing to pick
        // an input from.
        NodeSlot {
            visible: !root.isBus
            height: visible ? Skin.slotHeight : 0
            width: parent.width
            label: root.inputLabel
            filled: root.inputLabel !== qsTr("no input")
            level: Math.max(root.peakLeft, root.peakRight)
            onClicked: root.inputSlotClicked(this)
            onMenuRequested: root.inputMenuRequested(this)
        }

        // MIDI gets its own node slot rather than hiding in a menu: on this
        // mixer any channel can host a synth, so any channel can want MIDI.
        NodeSlot {
            visible: !root.isBus
            height: visible ? Skin.slotHeight : 0
            width: parent.width
            label: root.midiLabel
            filled: root.midiLabel !== qsTr("no MIDI")
            level: 0
            onClicked: root.midiSlotClicked(this)
            onMenuRequested: root.midiMenuRequested(this)
        }

        // --- fader, mute and solo -------------------------------------------
        Item {
            width: parent.width
            height: 168

            Fader {
                id: fader
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 62
                gain: root.gain
                peakLeft: root.peakLeft
                peakRight: root.peakRight
                accent: root.accent
                onGainRequested: value => mixer.setGain(root.row, value)
            }

            Column {
                anchors.right: parent.right
                anchors.top: parent.top
                width: parent.width - fader.width - Skin.gap
                spacing: Skin.gap

                StripButton {
                    width: parent.width
                    height: 26
                    label: "M"
                    active: root.muted
                    activeColor: Skin.mute
                    onClicked: mixer.toggleMute(root.row)
                }

                StripButton {
                    width: parent.width
                    height: 26
                    label: "S"
                    active: root.soloed
                    activeColor: Skin.solo
                    onClicked: mixer.toggleSolo(root.row)
                }

                StripButton {
                    width: parent.width
                    height: 26
                    label: "R"
                    active: root.armed
                    activeColor: Skin.arm
                    onClicked: mixer.toggleArm(root.row)
                }

                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: mixer.gainLabel(root.gain)
                    color: Skin.textDim
                    font.pixelSize: 10
                }
            }
        }

        // --- insert chain ----------------------------------------------------
        // Scrollable, with one empty slot always waiting at the bottom, which is
        // how AUM lets a chain grow without a separate "add" step.
        ListView {
            id: insertList
            width: parent.width
            height: root.height - y - outputSlot.height - title.height
                    - root.sends.length * (20 + Skin.gap) - Skin.gap * 3
            clip: true
            spacing: Skin.gap
            // AUM keeps a few empty slots visible below the chain rather than a
            // single "add" affordance, so the next insert is always one tap away.
            model: root.inserts.length + 3
            boundsBehavior: Flickable.StopAtBounds

            delegate: InsertSlot {
                width: insertList.width
                pluginName: index < root.inserts.length ? root.inserts[index] : ""
                onClicked: root.insertSlotClicked(index, this)
                onMenuRequested: {
                    if (index < root.inserts.length
                            && root.inserts[index].length > 0)
                        root.insertMenuRequested(index, this)
                }
            }
        }

        // Sends sit just above the output, which is where they leave from.
        Column {
            width: parent.width
            spacing: Skin.gap

            Repeater {
                model: root.sends

                SendRow {
                    width: parent.width
                    busName: modelData.name
                    level: modelData.level
                    onLevelRequested: value => root.sendLevelRequested(index, value)
                    onMenuRequested: root.sendMenuRequested(index, this)
                }
            }
        }

        NodeSlot {
            id: outputSlot
            width: parent.width
            label: root.outputLabel
            level: Math.max(root.peakLeft, root.peakRight)
            onClicked: root.outputSlotClicked(this)
            onMenuRequested: root.outputSlotClicked(this)
        }

        // The channel title is where AUM keeps renaming and removal, so it is
        // a button rather than a label.
        Item {
            id: title
            width: parent.width
            height: 18

            Text {
                anchors.fill: parent
                text: root.channelName
                color: titleHover.hovered ? Skin.text : Skin.textDim
                font.pixelSize: 11
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            HoverHandler {
                id: titleHover
            }

            MouseArea {
                anchors.fill: parent
                onClicked: root.titleClicked(title)
            }
        }
    }
}
