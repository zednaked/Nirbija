pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import Nirbija

ApplicationWindow {
    id: window

    // In scaled pixels, like everything else: at NIRBIJA_UI_SCALE=1.35 a window
    // still 700 px tall squeezes the insert chain out of every strip.
    width: Skin.px(1200)
    height: Skin.px(700)
    minimumWidth: Skin.px(660)
    minimumHeight: Skin.px(520)
    visible: true
    title: Mixer.dirty ? qsTr("Nirbija •") : qsTr("Nirbija")
    color: Skin.background

    // Meters are the only thing in this window that redraws by itself. When the
    // window is not on screen there is nobody to redraw for, so the model stops
    // announcing levels — it still reads them, because reading is what clears
    // the peak on the audio side.
    Binding {
        target: Mixer
        property: "metersActive"
        value: window.visible && window.visibility !== Window.Minimized
    }

    Shortcut {
        sequences: [StandardKey.Cancel, "Escape"]
        onActivated: Mixer.cancelLearn()
    }
    Shortcut {
        sequence: "Space"
        onActivated: Mixer.togglePlay()
    }
    Shortcut {
        sequence: StandardKey.Undo
        onActivated: Mixer.undo()
    }
    Shortcut {
        sequence: StandardKey.Redo
        onActivated: Mixer.redo()
    }
    Shortcut {
        sequences: [StandardKey.HelpContents, "F1"]
        onActivated: window.openShortcuts()
    }

    // --- menus -----------------------------------------------------------------
    // Built here as plain functions rather than inline in the delegate. The
    // strip delegate used to carry two hundred lines of menu literal, which put
    // every one of those closures inside the delegate's own scope: they saw
    // whichever `index` was nearest, and the day a Repeater was nested inside
    // another the wrong one won.

    function insertMenu(row, slot, inserts) {
        return [
            { label: qsTr("Load file…"),
              enabled: Mixer.insertIsFilePlayer(row, slot),
              action: () => window.openFileFor(row, slot) },
            { label: qsTr("Open editor"),
              action: () => window.openEditor(row, slot) },
            { label: Mixer.insertBypassed(row, slot) ? qsTr("Enable")
                                                     : qsTr("Bypass"),
              action: () => Mixer.setInsertBypassed(
                  row, slot, !Mixer.insertBypassed(row, slot)) },
            { label: Mixer.insertPostFader(row, slot) ? qsTr("Pre-fader")
                                                      : qsTr("Post-fader"),
              action: () => Mixer.setInsertPostFader(
                  row, slot, !Mixer.insertPostFader(row, slot)) },
            { label: qsTr("Create extra outputs"),
              enabled: Mixer.extraOutputPairs(row, slot) > 0,
              action: () => Mixer.addTapChannels(row, slot) },
            { label: qsTr("Move up"),
              enabled: slot > 0,
              action: () => Mixer.moveInsert(row, slot, -1) },
            { label: qsTr("Move down"),
              enabled: slot < inserts.length - 1,
              action: () => Mixer.moveInsert(row, slot, 1) },
            { label: qsTr("Replace…"),
              action: () => window.openPicker(row, slot, true) },
            { label: qsTr("Remove"), danger: true,
              action: () => Mixer.removeInsert(row, slot) }
        ]
    }

    function sourceMenu(row, label, midi, item) {
        return [
            { label: midi ? qsTr("Change MIDI source…") : qsTr("Change input…"),
              action: () => window.openPortPicker(midi ? "midi" : "audio",
                                                  row, item) },
            { label: qsTr("Disconnect"), danger: true,
              enabled: label !== (midi ? qsTr("no MIDI") : qsTr("no input")),
              action: () => Mixer.connectSource(row, "", midi) }
        ]
    }

    // Where a strip can send its signal: one destination, plus a copy to any
    // bus, plus the way to make a bus when there is none.
    function outputMenu(row, destination, sends) {
        const options = Mixer.destinationsFor(row)
        const entries = []
        for (const option of options) {
            entries.push({
                label: option.label,
                enabled: option.destination !== destination,
                action: () => Mixer.setDestination(row, option.destination)
            })
        }

        // A send goes to a bus on top of the destination, so only buses can
        // receive one. Channel destinations are 1000 + slot; a bus index is a
        // small number.
        for (const option of options) {
            if (option.destination < 0 || option.destination >= 1000) continue
            entries.push({
                label: qsTr("Send to %1").arg(option.label),
                enabled: sends.length < 4,
                action: () => Mixer.setSend(row, sends.length,
                                            option.destination, 0.35)
            })
        }

        // Always reachable, so a session with no bus yet is not a dead end:
        // this is how one strip comes to feed another.
        entries.push({
            label: qsTr("New mix bus…"),
            action: () => Mixer.sendRowToNewBus(row)
        })
        return entries
    }

    function titleMenu(row, name) {
        return [
            { label: qsTr("Rename…"),
              action: () => window.openRename(row, name) },
            { label: qsTr("MIDI learn: fader"),
              action: () => Mixer.learnGain(row) },
            { label: qsTr("MIDI learn: pan"),
              action: () => Mixer.learnPan(row) },
            { label: qsTr("MIDI learn: mute"),
              action: () => Mixer.learnMute(row) },
            { label: qsTr("Clear MIDI maps"),
              action: () => Mixer.clearMidiMaps(row) },
            { label: qsTr("Duplicate"),
              action: () => Mixer.duplicateChannel(row) },
            { label: qsTr("Move left"),
              enabled: row > 0,
              action: () => Mixer.moveChannel(row, -1) },
            { label: qsTr("Move right"),
              enabled: row < Mixer.rowCount() - 1,
              action: () => Mixer.moveChannel(row, 1) },
            { label: qsTr("Direct output…"),
              action: () => window.openPortPicker("channelSink", row, null) },
            { label: qsTr("Remove channel"), danger: true,
              action: () => Mixer.removeChannel(row) }
        ]
    }

    function sessionMenu() {
        return [
            { label: qsTr("Undo"), enabled: Mixer.canUndo,
              action: () => Mixer.undo() },
            { label: qsTr("Redo"), enabled: Mixer.canRedo,
              action: () => Mixer.redo() },
            { label: qsTr("Rescan plugins"),
              action: () => Mixer.plugins.rescan() },
            { label: qsTr("Recordings folder"),
              action: () => Qt.openUrlExternally(Mixer.recordingsUrl()) },
            { label: qsTr("Keyboard shortcuts"),
              action: () => window.openShortcuts() },
            { label: qsTr("Save session as…"),
              action: () => sessionSaveDialog.open() },
            { label: qsTr("Load session…"),
              action: () => sessionLoadDialog.open() },
            { label: qsTr("New session"), danger: true,
              action: () => Mixer.newSession() }
        ]
    }

    function addMenu() {
        return [
            { label: qsTr("Stereo channel"), action: () => Mixer.addChannel("", 2) },
            { label: qsTr("Mono channel"), action: () => Mixer.addChannel("", 1) },
            { label: qsTr("Mix bus"), action: () => Mixer.addBus("") }
        ]
    }

    // --- lazily built windows --------------------------------------------------
    // Every picker, editor and sheet used to be instantiated at startup, whether
    // or not it was ever opened: a plugin list of several hundred rows and a
    // parameter editor built before the first strip existed. A Loader means the
    // cost arrives with the first use.
    //
    // `Loader.item` is a bare QObject as far as the linter is concerned — it
    // cannot know what a sourceComponent will build — so the calls below are
    // exempted rather than left to warn on every run.
    // qmllint disable missing-property

    function openPicker(row, slot, replace) {
        pickerLoader.active = true
        pickerLoader.item.targetRow = row
        pickerLoader.item.targetSlot = slot
        pickerLoader.item.replace = replace === true
        pickerLoader.item.open()
    }

    function openEditor(row, slot) {
        // The plugin's own editor when it has one; sliders built from its
        // parameters when it does not. The step sequencer is neither: it is
        // ours, so it gets a grid rather than eighty-five rows.
        if (Mixer.insertIsStepSequencer(row, slot)) {
            stepLoader.active = true
            stepLoader.item.openFor(row, slot)
            return
        }
        if (Mixer.insertIsScript(row, slot)) {
            scriptLoader.active = true
            scriptLoader.item.openFor(row, slot)
            return
        }
        if (Mixer.openInsertEditor(row, slot)) return
        paramLoader.active = true
        paramLoader.item.openFor(row, slot)
    }

    function openPortPicker(kind, row, item) {
        portLoader.active = true
        portLoader.item.openFor(kind, row, item)
    }

    function openRename(row, name) {
        renameLoader.active = true
        renameLoader.item.openFor(row, name)
    }

    function openShortcuts() {
        shortcutLoader.active = true
        shortcutLoader.item.open()
    }

    function openNavigator() {
        navigatorLoader.active = true
        navigatorLoader.item.open()
    }

    function openMatrix() {
        matrixLoader.active = true
        matrixLoader.item.open()
    }

    function openFileFor(row, slot) {
        fileDialog.targetRow = row
        fileDialog.targetSlot = slot
        fileDialog.open()
    }
    // qmllint enable missing-property

    TopBar {
        id: topBar
        width: parent.width
        positionLeft: Mixer.masterPositionLeft
        positionRight: Mixer.masterPositionRight
        holdLeft: Mixer.masterHoldLeft
        holdRight: Mixer.masterHoldRight
        status: Mixer.status
        masterSink: Mixer.masterSink
        tempo: Mixer.tempo
        playing: Mixer.playing
        recording: Mixer.recording
        metronome: Mixer.metronome
        recordingLabel: Mixer.recordingLabel
        onMasterOutputClicked: window.openPortPicker("sink", -1, topBar)
        onNavigatorClicked: window.openNavigator()
        onMatrixClicked: window.openMatrix()
        onHelpClicked: window.openShortcuts()
        onMenuRequested: item => slotMenu.openAt(item, window.sessionMenu(),
                                                 qsTr("Nirbija"))
    }

    // Strips scroll horizontally as a session grows, which is the one direction
    // a mixer ever needs to grow in.
    MasterStrip {
        id: masterStrip
        anchors.top: topBar.bottom
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        gain: Mixer.masterGain
        positionLeft: Mixer.masterPositionLeft
        positionRight: Mixer.masterPositionRight
        holdLeft: Mixer.masterHoldLeft
        holdRight: Mixer.masterHoldRight
        sink: Mixer.masterSink
        onOutputClicked: window.openPortPicker("sink", -1, masterStrip)
    }

    Flickable {
        id: mixerArea
        anchors.top: topBar.bottom
        anchors.left: parent.left
        anchors.right: masterStrip.left
        anchors.bottom: parent.bottom
        anchors.margins: Skin.spacing
        contentWidth: stripRow.width
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        clip: true

        ScrollBar.horizontal: ScrollBar {
            policy: mixerArea.contentWidth > mixerArea.width
                    ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        }

        // A mixer only grows sideways, so a plain wheel moves along it. The
        // faders and lists inside carry their own wheel handlers and are deeper
        // in the tree, so they are asked first.
        WheelHandler {
            orientation: Qt.Vertical | Qt.Horizontal
            acceptedModifiers: Qt.NoModifier
            onWheel: event => {
                const delta = event.angleDelta.x !== 0 ? event.angleDelta.x
                                                       : event.angleDelta.y
                mixerArea.contentX = Math.max(
                    0, Math.min(mixerArea.contentWidth - mixerArea.width,
                                mixerArea.contentX - delta))
            }
        }

        Row {
            id: stripRow
            height: mixerArea.height
            spacing: Skin.gap

            Repeater {
                model: Mixer

                ChannelStrip {
                    id: strip
                    required property int index
                    required property var model

                    height: mixerArea.height
                    row: strip.index
                    channelName: strip.model.name
                    gain: strip.model.gain
                    pan: strip.model.pan
                    muted: strip.model.muted
                    soloed: strip.model.soloed
                    armed: strip.model.armed
                    positionLeft: strip.model.positionLeft
                    positionRight: strip.model.positionRight
                    holdLeft: strip.model.holdLeft
                    holdRight: strip.model.holdRight
                    inputLabel: strip.model.inputLabel
                    midiLabel: strip.model.midiLabel
                    outputLabel: strip.model.outputLabel
                    inserts: strip.model.insertDetails
                    accent: strip.model.accent
                    isBus: strip.model.isBus
                    sends: strip.model.sends

                    onInputSlotClicked: item =>
                        window.openPortPicker("audio", strip.index, item)
                    onMidiSlotClicked: item =>
                        window.openPortPicker("midi", strip.index, item)

                    onInputMenuRequested: item => slotMenu.openAt(
                        item,
                        window.sourceMenu(strip.index, strip.model.inputLabel,
                                          false, item),
                        strip.model.inputLabel)

                    onMidiMenuRequested: item => slotMenu.openAt(
                        item,
                        window.sourceMenu(strip.index, strip.model.midiLabel,
                                          true, item),
                        strip.model.midiLabel)

                    onInsertMenuRequested: (slot, item) => slotMenu.openAt(
                        item,
                        window.insertMenu(strip.index, slot,
                                          strip.model.insertDetails),
                        strip.model.insertDetails[slot].name)

                    onOutputSlotClicked: item => slotMenu.openAt(
                        item,
                        window.outputMenu(strip.index, strip.model.destination,
                                          strip.model.sends),
                        qsTr("Output"))

                    onSendLevelRequested: (slot, level) =>
                        Mixer.setSend(strip.index, slot,
                                      strip.model.sends[slot].bus, level)

                    onSendMenuRequested: (slot, item) => slotMenu.openAt(item, [
                        { label: qsTr("Remove send"), danger: true,
                          action: () => Mixer.removeSend(strip.index, slot) }
                    ], strip.model.sends[slot].name)

                    onTitleClicked: item => slotMenu.openAt(
                        item, window.titleMenu(strip.index, strip.model.name),
                        strip.model.name)

                    onInsertSlotClicked: (slot, item) => {
                        // A filled slot opens the plugin's own editor; an empty
                        // one opens the picker to fill it.
                        if (slot < strip.model.insertDetails.length) {
                            window.openEditor(strip.index, slot)
                            return
                        }
                        window.openPicker(strip.index, slot, false)
                    }
                }
            }

            // AUM's big square "+" that adds a channel, always at the end of the
            // row so it moves along as the session grows.
            Rectangle {
                id: addSquare
                width: Skin.stripWidth
                height: mixerArea.height
                radius: Skin.radius
                color: addHover.hovered ? Skin.stripAlt : Skin.strip
                opacity: addHover.hovered ? 1.0 : 0.7

                Behavior on opacity {
                    NumberAnimation { duration: Skin.fast }
                }

                Column {
                    anchors.centerIn: parent
                    spacing: Skin.spacingS

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "+"
                        color: addHover.hovered ? Skin.text : Skin.textDim
                        font.pixelSize: Skin.px(40)
                    }

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("add strip")
                        color: Skin.disabled
                        font.pixelSize: Skin.fontS
                    }
                }

                HoverHandler {
                    id: addHover
                }

                // The right-click menu is the only way to a mono channel or a
                // bus, and nothing on the square hints that it is there.
                Tip {
                    text: qsTr("Add a stereo channel. Right-click for a mono channel or a bus.")
                    visible: addHover.hovered
                }

                // A MouseArea, not TapHandlers. This square is the full height
                // of the mixer, so a good deal of what opens over it lands
                // inside its bounds - and a handler still answered those taps,
                // because handlers sit on the pointer delivery path rather than
                // in the stacking order. Clicking a menu that had opened over
                // here quietly added a channel. A MouseArea is covered by what
                // is drawn on top of it, which is the whole point.
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: mouse => {
                        if (mouse.button === Qt.RightButton)
                            slotMenu.openAt(addSquare, window.addMenu(), qsTr("Add"))
                        else
                            Mixer.addChannel("", 2)
                    }
                    // The touch route to the same menu.
                    onPressAndHold: slotMenu.openAt(addSquare, window.addMenu(),
                                                    qsTr("Add"))
                }
            }
        }
    }

    // --- popups, built on first use --------------------------------------------
    Loader {
        id: pickerLoader
        active: false
        sourceComponent: PluginPicker {}
    }

    Loader {
        id: paramLoader
        active: false
        sourceComponent: ParamEditor {}
    }

    Loader {
        id: stepLoader
        active: false
        sourceComponent: StepGrid {}
    }

    Loader {
        id: scriptLoader
        active: false
        sourceComponent: ScriptEditor {}
    }

    Loader {
        id: portLoader
        active: false
        sourceComponent: PortPicker {
            onPicked: port => {
                if (kind === "sink")
                    Mixer.connectMaster(port)
                else if (kind === "channelSink")
                    Mixer.connectChannelSink(targetRow, port)
                else
                    Mixer.connectSource(targetRow, port, kind === "midi")
            }
        }
    }

    Loader {
        id: renameLoader
        active: false
        sourceComponent: RenameDialog {
            onAccepted: name => Mixer.renameChannel(targetRow, name)
        }
    }

    Loader {
        id: navigatorLoader
        active: false
        sourceComponent: Navigator {
            x: window.width - width - Skin.spacing
            y: topBar.height + Skin.spacing
            onJumpTo: row => mixerArea.contentX =
                          Math.max(0, Math.min(row * (Skin.stripWidth + Skin.gap),
                                               mixerArea.contentWidth
                                               - mixerArea.width))
            onOpenInsert: (row, slot) => window.openEditor(row, slot)
        }
    }

    Loader {
        id: matrixLoader
        active: false
        sourceComponent: MidiMatrix {
            anchors.centerIn: Overlay.overlay
        }
    }

    Loader {
        id: shortcutLoader
        active: false
        sourceComponent: ShortcutSheet {}
    }

    FileDialog {
        id: sessionSaveDialog
        title: qsTr("Save session as")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Nirbija sessions (*.json)")]
        defaultSuffix: "json"
        onAccepted: Mixer.saveSessionAs(selectedFile)
    }

    FileDialog {
        id: sessionLoadDialog
        title: qsTr("Load session")
        nameFilters: [qsTr("Nirbija sessions (*.json)")]
        onAccepted: Mixer.loadSessionFrom(selectedFile)
    }

    FileDialog {
        id: fileDialog
        property int targetRow: -1
        property int targetSlot: -1
        title: qsTr("Choose an audio file")
        nameFilters: [qsTr("Audio files (*.wav *.flac *.ogg *.aiff *.aif)"),
                      qsTr("All files (*)")]
        onAccepted: Mixer.setInsertFile(targetRow, targetSlot, selectedFile)
    }

    SlotMenu {
        id: slotMenu
    }

    // --- what went wrong -------------------------------------------------------
    Connections {
        target: Mixer
        function onErrorOccurred(message) {
            statusToast.message = message
            statusToast.show()
        }
    }

    Rectangle {
        id: statusToast

        property string message: ""

        function show() {
            opacity = 1
            toastTimer.restart()
        }

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Skin.spacingL
        width: Math.min(parent.width - Skin.px(40),
                        toastText.implicitWidth + Skin.px(28))
        height: toastText.implicitHeight + Skin.spacingL
        radius: Skin.radius
        color: Skin.popup
        border.width: 1
        border.color: Skin.mute
        opacity: 0
        visible: opacity > 0
        z: 20

        Behavior on opacity {
            NumberAnimation { duration: Skin.medium }
        }

        Text {
            id: toastText
            anchors.centerIn: parent
            width: parent.width - Skin.px(24)
            text: statusToast.message
            color: Skin.text
            font.pixelSize: Skin.font
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }

        // Dismissable, because a message that has been read is in the way.
        TapHandler {
            onSingleTapped: statusToast.opacity = 0
        }
    }

    Timer {
        id: toastTimer
        interval: 5000
        onTriggered: statusToast.opacity = 0
    }

    // A session that opens empty gives nothing to look at, and AUM starts with
    // a strip on screen too.
    Component.onCompleted: {
        if (Mixer.rowCount() === 0 && Mixer.shouldSeedSession()) {
            Mixer.addChannel("", 2)
            Mixer.addChannel("", 2)
        }
    }
}
