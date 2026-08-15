import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs

ApplicationWindow {
    id: window

    width: 1100
    height: 640
    minimumWidth: 640
    minimumHeight: 420
    visible: true
    title: mixer.dirty ? qsTr("Nirbija •") : qsTr("Nirbija")
    color: Skin.background

    Shortcut {
        sequences: [StandardKey.Cancel, "Escape"]
        onActivated: mixer.cancelLearn()
    }
    Shortcut {
        sequence: "Space"
        onActivated: mixer.togglePlay()
    }
    Shortcut {
        sequence: StandardKey.Undo
        onActivated: mixer.undo()
    }
    Shortcut {
        sequence: StandardKey.Redo
        onActivated: mixer.redo()
    }

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
        metronome: mixer.metronome
        recordingLabel: mixer.recordingLabel
        onMasterOutputClicked: portPicker.openFor("sink", -1, topBar)
        onNavigatorClicked: navigator.open()
        onMatrixClicked: midiMatrix.open()
        onMenuRequested: item => slotMenu.openAt(item, [
            { label: qsTr("Undo"),
              enabled: mixer.canUndo,
              action: () => mixer.undo() },
            { label: qsTr("Redo"),
              enabled: mixer.canRedo,
              action: () => mixer.redo() },
            { label: qsTr("Rescan plugins"),
              action: () => mixer.plugins.rescan() },
            { label: qsTr("Recordings folder"),
              action: () => Qt.openUrlExternally(mixer.recordingsUrl()) },
            { label: qsTr("Save session as…"),
              action: () => sessionSaveDialog.open() },
            { label: qsTr("Load session…"),
              action: () => sessionLoadDialog.open() },
            { label: qsTr("New session"), danger: true,
              action: () => mixer.newSession() }
        ], qsTr("Nirbija"))
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
                    pan: model.pan
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

                    onInputSlotClicked: item => portPicker.openFor("audio", index, item)
                    onMidiSlotClicked: item => portPicker.openFor("midi", index, item)

                    onInputMenuRequested: item => slotMenu.openAt(item, [
                        { label: qsTr("Change input…"),
                          action: () => portPicker.openFor("audio", index, item) },
                        { label: qsTr("Disconnect"), danger: true,
                          enabled: model.inputLabel !== qsTr("no input"),
                          action: () => mixer.connectSource(index, "", false) }
                    ], model.inputLabel)

                    onMidiMenuRequested: item => slotMenu.openAt(item, [
                        { label: qsTr("Change MIDI source…"),
                          action: () => portPicker.openFor("midi", index, item) },
                        { label: qsTr("Disconnect"), danger: true,
                          enabled: model.midiLabel !== qsTr("no MIDI"),
                          action: () => mixer.connectSource(index, "", true) }
                    ], model.midiLabel)

                    onInsertMenuRequested: (slot, item) => slotMenu.openAt(item, [
                        { label: qsTr("Load file…"),
                          enabled: mixer.insertIsFilePlayer(index, slot),
                          action: () => {
                              fileDialog.targetRow = index
                              fileDialog.targetSlot = slot
                              fileDialog.open()
                          } },
                        { label: qsTr("Open editor"),
                          action: () => {
                              if (!mixer.openInsertEditor(index, slot))
                                  paramEditor.openFor(index, slot)
                          } },
                        { label: mixer.insertBypassed(index, slot)
                              ? qsTr("Enable") : qsTr("Bypass"),
                          action: () => mixer.setInsertBypassed(
                              index, slot, !mixer.insertBypassed(index, slot)) },
                        { label: mixer.insertPostFader(index, slot)
                              ? qsTr("Pre-fader") : qsTr("Post-fader"),
                          action: () => mixer.setInsertPostFader(
                              index, slot, !mixer.insertPostFader(index, slot)) },
                        { label: qsTr("Create extra outputs"),
                          enabled: mixer.extraOutputPairs(index, slot) > 0,
                          action: () => mixer.addTapChannels(index, slot) },
                        { label: qsTr("Move up"),
                          enabled: slot > 0,
                          action: () => mixer.moveInsert(index, slot, -1) },
                        { label: qsTr("Move down"),
                          enabled: slot < model.inserts.length - 1,
                          action: () => mixer.moveInsert(index, slot, 1) },
                        { label: qsTr("Replace…"),
                          action: () => {
                              picker.targetRow = index
                              picker.targetSlot = slot
                              picker.replace = true
                              picker.open()
                          } },
                        { label: qsTr("Remove"), danger: true,
                          action: () => mixer.removeInsert(index, slot) }
                    ], model.inserts[slot])

                    onOutputSlotClicked: item => {
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
                            // Sends only feed buses. Channel destinations are
                            // 1000 + slot; a bus index is a small number.
                            if (option.destination >= 1000)
                                continue
                            entries.push({
                                label: qsTr("Send to %1").arg(option.label),
                                enabled: model.sends.length < 4,
                                action: () => mixer.setSend(index,
                                                            model.sends.length,
                                                            option.destination, 0.35)
                            })
                        }

                        // Always reachable, so a session with no bus yet is not
                        // a dead end: this is how one strip comes to feed
                        // another.
                        entries.push({
                            label: qsTr("New mix bus…"),
                            action: () => mixer.sendRowToNewBus(index)
                        })

                        slotMenu.openAt(item, entries, qsTr("Output"))
                    }

                    onSendLevelRequested: (slot, level) =>
                        mixer.setSend(index, slot, model.sends[slot].bus, level)

                    onSendMenuRequested: (slot, item) => slotMenu.openAt(item, [
                        { label: qsTr("Remove send"), danger: true,
                          action: () => mixer.removeSend(index, slot) }
                    ], model.sends[slot].name)

                    onTitleClicked: item => slotMenu.openAt(item, [
                        { label: qsTr("Rename…"),
                          action: () => renameDialog.openFor(index, model.name) },
                        { label: qsTr("MIDI learn: fader"),
                          action: () => mixer.learnGain(index) },
                        { label: qsTr("MIDI learn: pan"),
                          action: () => mixer.learnPan(index) },
                        { label: qsTr("MIDI learn: mute"),
                          action: () => mixer.learnMute(index) },
                        { label: qsTr("Clear MIDI maps"),
                          action: () => mixer.clearMidiMaps(index) },
                        { label: qsTr("Duplicate"),
                          action: () => mixer.duplicateChannel(index) },
                        { label: qsTr("Move left"),
                          enabled: index > 0,
                          action: () => mixer.moveChannel(index, -1) },
                        { label: qsTr("Move right"),
                          enabled: index < mixer.rowCount() - 1,
                          action: () => mixer.moveChannel(index, 1) },
                        { label: qsTr("Direct output…"),
                          action: () => portPicker.openFor("channelSink", index, item) },
                        { label: qsTr("Remove channel"), danger: true,
                          action: () => mixer.removeChannel(index) }
                    ], model.name)

                    onInsertSlotClicked: (slot, item) => {
                        // A filled slot opens the plugin's own editor; an empty
                        // one opens the picker to fill it.
                        if (slot < model.inserts.length
                                && model.inserts[slot].length > 0) {
                            // The plugin's own editor when it has one; sliders
                            // built from its parameters when it does not.
                            if (!mixer.openInsertEditor(index, slot))
                                paramEditor.openFor(index, slot)
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

    ParamEditor {
        id: paramEditor
    }

    FileDialog {
        id: sessionSaveDialog
        title: qsTr("Save session as")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Nirbija sessions (*.json)")]
        defaultSuffix: "json"
        onAccepted: mixer.saveSessionAs(selectedFile)
    }

    FileDialog {
        id: sessionLoadDialog
        title: qsTr("Load session")
        nameFilters: [qsTr("Nirbija sessions (*.json)")]
        onAccepted: mixer.loadSessionFrom(selectedFile)
    }

    FileDialog {
        id: fileDialog
        property int targetRow: -1
        property int targetSlot: -1
        title: qsTr("Choose an audio file")
        nameFilters: [qsTr("Audio files (*.wav *.flac *.ogg *.aiff *.aif)"),
                      qsTr("All files (*)")]
        onAccepted: mixer.setInsertFile(targetRow, targetSlot, selectedFile)
    }

    SlotMenu {
        id: slotMenu
    }

    RenameDialog {
        id: renameDialog
        onAccepted: name => mixer.renameChannel(targetRow, name)
    }

    MidiMatrix {
        id: midiMatrix
        anchors.centerIn: Overlay.overlay
    }

    Navigator {
        id: navigator
        x: parent.width - width - 8
        y: topBar.height + 8
        onJumpTo: row => mixerArea.contentX =
                      Math.max(0, Math.min(row * (Skin.stripWidth + Skin.gap),
                                           mixerArea.contentWidth - mixerArea.width))
        onOpenInsert: (row, slot) => {
            if (!mixer.openInsertEditor(row, slot))
                paramEditor.openFor(row, slot)
        }
    }

    PortPicker {
        id: portPicker
        onPicked: port => {
            if (kind === "sink")
                mixer.connectMaster(port)
            else if (kind === "channelSink")
                mixer.connectChannelSink(targetRow, port)
            else
                mixer.connectSource(targetRow, port, kind === "midi")
        }
    }

    // A session that opens empty gives nothing to look at, and AUM starts with
    // a strip on screen too.
    Connections {
        target: mixer
        function onErrorOccurred(message) {
            statusToast.text = message
            statusToast.visible = true
            toastTimer.restart()
        }
    }

    Text {
        id: statusToast
        visible: false
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        padding: 8
        color: Skin.text
        font.pixelSize: 12
        text: ""
        z: 20
        Rectangle {
            anchors.fill: parent
            z: -1
            radius: Skin.radius
            color: Skin.strip
            border.color: Skin.line
            border.width: 1
        }
    }

    Timer {
        id: toastTimer
        interval: 3500
        onTriggered: statusToast.visible = false
    }

    Component.onCompleted: {
        if (mixer.rowCount() === 0 && mixer.shouldSeedSession()) {
            mixer.addChannel("", 2)
            mixer.addChannel("", 2)
        }
    }
}
