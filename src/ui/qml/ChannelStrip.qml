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
    property string outputLabel: ""
    property var inserts: []
    property color accent: Skin.accent

    signal insertSlotClicked(int slot)

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

        NodeSlot {
            width: parent.width
            label: root.inputLabel
            level: Math.max(root.peakLeft, root.peakRight)
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
            height: root.height - y - outputSlot.height - title.height - Skin.gap * 3
            clip: true
            spacing: Skin.gap
            // AUM keeps a few empty slots visible below the chain rather than a
            // single "add" affordance, so the next insert is always one tap away.
            model: root.inserts.length + 3
            boundsBehavior: Flickable.StopAtBounds

            delegate: InsertSlot {
                width: insertList.width
                pluginName: index < root.inserts.length ? root.inserts[index] : ""
                onClicked: root.insertSlotClicked(index)
                onLongPressed: {
                    if (index < root.inserts.length)
                        mixer.removeInsert(root.row, index)
                }
            }
        }

        NodeSlot {
            id: outputSlot
            width: parent.width
            label: root.outputLabel
            level: Math.max(root.peakLeft, root.peakRight)
        }

        Text {
            id: title
            width: parent.width
            height: 18
            text: root.channelName
            color: Skin.text
            font.pixelSize: 11
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }
}
