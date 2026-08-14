import QtQuick
import QtQuick.Controls.Basic

ApplicationWindow {
    id: window

    width: 1100
    height: 640
    visible: true
    title: qsTr("Nirbija")
    color: Skin.background

    TopBar {
        id: topBar
        width: parent.width
        peakLeft: mixer.masterPeakLeft
        peakRight: mixer.masterPeakRight
        status: mixer.status
        masterSink: mixer.masterSink
        tempo: mixer.tempo
        playing: mixer.playing
        recording: mixer.recording
        recordingLabel: mixer.recordingLabel
        onMasterOutputClicked: portPicker.openFor("sink", -1, topBar)
    }

    // Strips scroll horizontally as a session grows, which is the one direction
    // a mixer ever needs to grow in.
    MasterStrip {
        id: masterStrip
        anchors.top: topBar.bottom
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        gain: mixer.masterGain
        peakLeft: mixer.masterPeakLeft
        peakRight: mixer.masterPeakRight
        sink: mixer.masterSink
        onOutputClicked: portPicker.openFor("sink", -1, masterStrip)
    }

    Flickable {
        id: mixerArea
        anchors.top: topBar.bottom
        anchors.left: parent.left
        anchors.right: masterStrip.left
        anchors.bottom: parent.bottom
        anchors.margins: Skin.gap * 2
        contentWidth: stripRow.width
        flickableDirection: Flickable.HorizontalFlick
        clip: true

        Row {
            id: stripRow
            height: mixerArea.height
            spacing: Skin.gap

            Repeater {
                model: mixer

                ChannelStrip {
                    height: mixerArea.height
                    row: index
                    channelName: name
                    gain: model.gain
                    muted: model.muted
                    soloed: model.soloed
                    armed: model.armed
                    peakLeft: model.peakLeft
                    peakRight: model.peakRight
                    inputLabel: model.inputLabel
                    midiLabel: model.midiLabel
                    outputLabel: model.outputLabel
                    inserts: model.inserts
                    accent: model.accent
                    isBus: model.isBus
                    sends: model.sends

                    onInputSlotClicked: portPicker.openFor("audio", index, this)
                    onMidiSlotClicked: portPicker.openFor("midi", index, this)

                    onInputMenuRequested: slotMenu.openAt(this, [
                        { label: qsTr("Change input…"),
                          action: () => portPicker.openFor("audio", index, this) },
                        { label: qsTr("Disconnect"), danger: true,
                          enabled: model.inputLabel !== qsTr("no input"),
                          action: () => mixer.connectSource(index, "", false) }
                    ], model.inputLabel)

                    onMidiMenuRequested: slotMenu.openAt(this, [
                        { label: qsTr("Change MIDI source…"),
                          action: () => portPicker.openFor("midi", index, this) },
                        { label: qsTr("Disconnect"), danger: true,
                          enabled: model.midiLabel !== qsTr("no MIDI"),
                          action: () => mixer.connectSource(index, "", true) }
                    ], model.midiLabel)

                    onInsertMenuRequested: slot => slotMenu.openAt(this, [
                        { label: qsTr("Open editor"),
                          action: () => mixer.openInsertEditor(index, slot) },
                        { label: qsTr("Move up"),
                          enabled: slot > 0,
                          action: () => mixer.moveInsert(index, slot, -1) },
                        { label: qsTr("Move down"),
                          enabled: slot < model.inserts.length - 1,
                          action: () => mixer.moveInsert(index, slot, 1) },
                        { label: qsTr("Replace…"),
                          action: () => {
                              mixer.removeInsert(index, slot)
                              picker.targetRow = index
                              picker.open()
                          } },
                        { label: qsTr("Remove"), danger: true,
                          action: () => mixer.removeInsert(index, slot) }
                    ], model.inserts[slot])

                    onOutputSlotClicked: {
                        const options = mixer.destinationsFor(index)
                        const entries = []
                        for (let i = 0; i < options.length; ++i) {
                            const option = options[i]
                            entries.push({
                                label: option.label,
                                enabled: option.destination !== model.destination,
                                action: () => mixer.setDestination(index,
                                                                   option.destination)
                            })
                        }

                        // A send goes to a bus on top of the destination, so
                        // only buses can receive one.
                        for (let i = 0; i < options.length; ++i) {
                            const option = options[i]
                            if (option.destination < 0)
                                continue
                            entries.push({
                                label: qsTr("Send to %1").arg(option.label),
                                enabled: model.sends.length < 4,
                                action: () => mixer.setSend(index,
                                                            model.sends.length,
                                                            option.destination, 0.35)
                            })
                        }

                        slotMenu.openAt(this, entries, qsTr("Output"))
                    }

                    onSendLevelRequested: (slot, level) =>
                        mixer.setSend(index, slot, model.sends[slot].bus, level)

                    onSendMenuRequested: slot => slotMenu.openAt(this, [
                        { label: qsTr("Remove send"), danger: true,
                          action: () => mixer.removeSend(index, slot) }
                    ], model.sends[slot].name)

                    onTitleClicked: slotMenu.openAt(this, [
                        { label: qsTr("Rename…"),
                          action: () => renameDialog.openFor(index, model.name) },
                        { label: qsTr("Remove channel"), danger: true,
                          action: () => mixer.removeChannel(index) }
                    ], model.name)

                    onInsertSlotClicked: slot => {
                        // A filled slot opens the plugin's own editor; an empty
                        // one opens the picker to fill it.
                        if (slot < model.inserts.length
                                && model.inserts[slot].length > 0) {
                            if (!mixer.openInsertEditor(index, slot))
                                console.warn("no embeddable editor for",
                                             model.inserts[slot])
                            return
                        }
                        picker.targetRow = index
                        picker.targetSlot = slot
                        picker.open()
                    }
                }
            }

            // AUM's big square "+" that adds a channel, always at the end of the
            // row so it moves along as the session grows.
            Rectangle {
                width: Skin.stripWidth
                height: mixerArea.height
                radius: Skin.radius
                color: Skin.strip
                opacity: addArea.containsMouse ? 1.0 : 0.65

                Text {
                    anchors.centerIn: parent
                    text: "+"
                    color: Skin.textDim
                    font.pixelSize: 40
                }

                MouseArea {
                    id: addArea
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: mouse => {
                        if (mouse.button === Qt.LeftButton) {
                            mixer.addChannel("", 2)
                            return
                        }
                        slotMenu.openAt(parent, [
                            { label: qsTr("Stereo channel"),
                              action: () => mixer.addChannel("", 2) },
                            { label: qsTr("Mono channel"),
                              action: () => mixer.addChannel("", 1) },
                            { label: qsTr("Mix bus"),
                              action: () => mixer.addBus("") }
                        ], qsTr("Add"))
                    }
                    onPressAndHold: slotMenu.openAt(parent, [
                        { label: qsTr("Stereo channel"),
                          action: () => mixer.addChannel("", 2) },
                        { label: qsTr("Mono channel"),
                          action: () => mixer.addChannel("", 1) },
                        { label: qsTr("Mix bus"),
                          action: () => mixer.addBus("") }
                    ], qsTr("Add"))
                }
            }
        }
    }

    PluginPicker {
        id: picker
    }

    SlotMenu {
        id: slotMenu
    }

    RenameDialog {
        id: renameDialog
        onAccepted: name => mixer.renameChannel(targetRow, name)
    }

    PortPicker {
        id: portPicker
        onPicked: port => {
            if (kind === "sink")
                mixer.connectMaster(port)
            else
                mixer.connectSource(targetRow, port, kind === "midi")
        }
    }

    // A session that opens empty gives nothing to look at, and AUM starts with
    // a strip on screen too.
    Component.onCompleted: {
        if (mixer.rowCount() === 0) {
            mixer.addChannel("", 2)
            mixer.addChannel("", 2)
        }
    }
}
