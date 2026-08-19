pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// The computer keyboard's own editor. Open it once to pick which strip
// listens - after that, typing plays notes whether this window is open,
// closed, or sitting behind three others, the same as the looper keeps
// looping with its editor shut. The layout is GarageBand's "Musical
// Typing" - A through ; walks one octave and a half, white and black keys
// interleaved the way a real keyboard tells them apart - so it needs no
// explanation for anyone who has used one of these before. Z/X shift the
// octave, C/V the velocity, both while a key is down or not.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property string pluginName: ""

    readonly property int idChannel: 0

    property int channel: 1
    property int octave: 4
    property int velocity: 100

    // Physical key code -> the note it sent. Kept so a release always lets
    // go of exactly what its press started, even if the octave moved while
    // the key was still down.
    property var heldKeys: ({})
    // Z/X/C/V held right now. Some compositors (Hyprland among them) don't
    // reliably flag a synthesized OS repeat as autoRepeat, so a key held a
    // little too long could shift the octave or velocity many steps in one
    // breath instead of once - the note keys never had this problem because
    // pressKey already refuses a second down while heldKeys still has the
    // first. This gives the control keys the same self-guard rather than
    // trusting the flag alone.
    property var heldControls: ({})
    // Set once, the first time this ever opens - after that the popup stays
    // wherever it was last dragged, the same as a real tool window would.
    property bool positioned: false

    readonly property var keyCodes: [
        Qt.Key_A, Qt.Key_W, Qt.Key_S, Qt.Key_E, Qt.Key_D,
        Qt.Key_F, Qt.Key_T, Qt.Key_G, Qt.Key_Y, Qt.Key_H,
        Qt.Key_U, Qt.Key_J, Qt.Key_K, Qt.Key_O, Qt.Key_L,
        Qt.Key_P, Qt.Key_Semicolon
    ]
    readonly property var keyLabels: [
        "A", "W", "S", "E", "D", "F", "T", "G", "Y", "H",
        "U", "J", "K", "O", "L", "P", ";"
    ]
    // Which of the seventeen semitones above the octave's root fall on a
    // white key, same shape a real octave and a half has.
    readonly property var whiteOffsets: [0, 2, 4, 5, 7, 9, 11, 12, 14, 16]

    function offsetFor(qtKey) { return root.keyCodes.indexOf(qtKey) }

    width: Px.px(560)
    height: Px.px(260)
    // Not modal: this is meant to stay open while you watch meters and work
    // the rest of the mixer with the other hand. Dragging the empty
    // background moves it wherever you want it.
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

    // Closing mid-chord must not leave a note ringing forever downstream -
    // there is no more key-up coming for it once this popup is gone. Notes
    // keep coming after this, though: see the Connections below.
    onClosed: root.releaseAll()

    // The actual listening happens here, not in the popup's own content -
    // Mixer.globalKeyEvent fires for every key in the window regardless of
    // which item has focus or whether this editor is even open, the same
    // way the looper keeps looping with its editor closed. Once a strip has
    // been picked below, that strip keeps hearing the keyboard until
    // another Computer Keyboard editor is opened to repoint it.
    Connections {
        target: Mixer
        function onGlobalKeyEvent(key, pressed, autoRepeat) {
            if (root.targetRow < 0) return
            const isControl = key === Qt.Key_Z || key === Qt.Key_X
                              || key === Qt.Key_C || key === Qt.Key_V
            const isNote = root.offsetFor(key) >= 0
            if (!isControl && !isNote) return
            if (!pressed) {
                if (isNote) root.releaseKey(key)
                else if (isControl) {
                    const held = Object.assign({}, root.heldControls)
                    delete held[key]
                    root.heldControls = held
                }
                return
            }
            if (isControl) {
                if (root.heldControls[key]) return  // still down - a repeat, flagged or not
                const held = Object.assign({}, root.heldControls)
                held[key] = true
                root.heldControls = held
            } else if (autoRepeat) {
                return  // OS repeat does not restrike a note
            }
            switch (key) {
            case Qt.Key_Z: root.octave = Math.max(0, root.octave - 1); break
            case Qt.Key_X: root.octave = Math.min(9, root.octave + 1); break
            case Qt.Key_C: root.velocity = Math.max(1, root.velocity - 10); break
            case Qt.Key_V: root.velocity = Math.min(127, root.velocity + 10); break
            default: root.pressKey(key)
            }
        }
    }

    function openFor(row, slot) {
        root.targetRow = row
        root.targetSlot = slot
        root.pluginName = Mixer.insertName(row, slot)
        const values = Mixer.insertParameters(row, slot)
        for (const entry of values)
            if (entry.id === root.idChannel) root.channel = Math.round(entry.value)
        root.heldKeys = {}
        root.heldControls = {}
        root.open()
    }

    function releaseAll() {
        for (const code in root.heldKeys)
            Mixer.releaseComputerKey(root.targetRow, root.targetSlot,
                                     root.heldKeys[code])
        root.heldKeys = {}
    }

    function pressKey(qtKey) {
        if (root.heldKeys[qtKey] !== undefined) return
        const offset = root.offsetFor(qtKey)
        if (offset < 0) return
        const note = Math.max(0, Math.min(127, root.octave * 12 + offset))
        Mixer.pressComputerKey(root.targetRow, root.targetSlot, note, root.velocity)
        const held = Object.assign({}, root.heldKeys)
        held[qtKey] = note
        root.heldKeys = held
    }

    function releaseKey(qtKey) {
        const note = root.heldKeys[qtKey]
        if (note === undefined) return
        Mixer.releaseComputerKey(root.targetRow, root.targetSlot, note)
        const held = Object.assign({}, root.heldKeys)
        delete held[qtKey]
        root.heldKeys = held
    }

    contentItem: Item {
        id: keyCapture

        ColumnLayout {
            anchors.fill: parent
            spacing: Skin.spacingS

            RowLayout {
                Layout.fillWidth: true
                spacing: Skin.spacingS

                Text {
                    text: root.pluginName
                    color: Skin.text
                    font.pixelSize: Skin.fontL
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: "C" + root.octave
                    color: Skin.textDim
                    font.pixelSize: Skin.fontS
                    font.family: Skin.monoFamily
                }
                Text {
                    text: qsTr("vel %1").arg(root.velocity)
                    color: Skin.textDim
                    font.pixelSize: Skin.fontS
                    font.family: Skin.monoFamily
                }
            }

            // --- the keys ---------------------------------------------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Skin.slotEmpty
                radius: Skin.radius
                border.width: 1
                border.color: Skin.border
                clip: true

                Row {
                    id: keys
                    anchors.fill: parent
                    anchors.margins: Skin.spacingXS
                    spacing: 1

                    Repeater {
                        model: root.keyLabels.length

                        Rectangle {
                            id: key
                            required property int index
                            readonly property int qtKey: root.keyCodes[key.index]
                            readonly property bool black:
                                root.whiteOffsets.indexOf(key.index) < 0
                            readonly property bool pressed:
                                root.heldKeys[key.qtKey] !== undefined

                            width: (keys.width - keys.spacing * (root.keyLabels.length - 1))
                                   / root.keyLabels.length
                            height: keys.height
                            radius: Skin.radiusS
                            color: key.pressed ? Skin.accent
                                 : key.black ? Skin.strip
                                 : Skin.slot

                            Behavior on color {
                                ColorAnimation { duration: Skin.fast }
                            }

                            Text {
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: Px.px(4)
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: root.keyLabels[key.index]
                                color: key.pressed ? Skin.onAccent : Skin.textDim
                                font.pixelSize: Skin.fontXS
                            }

                            // The mouse plays too, for testing the strip
                            // without hands on the keyboard.
                            TapHandler {
                                id: tap
                                onPressedChanged: {
                                    if (tap.pressed) root.pressKey(key.qtKey)
                                    else root.releaseKey(key.qtKey)
                                }
                            }
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("A W S E D F T G Y H U J K O L P ; play the keys · Z/X octave · C/V velocity · keeps listening after you close this")
                color: Skin.textDim
                font.pixelSize: Skin.fontXS
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Skin.spacingXS

                Text {
                    text: qsTr("channel")
                    color: Skin.textDim
                    font.pixelSize: Skin.fontXS
                }
                StripButton {
                    Layout.preferredWidth: Px.px(36)
                    label: String(root.channel)
                    tip: qsTr("MIDI channel out. Click to walk it.")
                    onClicked: {
                        root.channel = root.channel >= 16 ? 1 : root.channel + 1
                        Mixer.setInsertParameter(root.targetRow, root.targetSlot,
                                                 root.idChannel, root.channel)
                    }
                }
            }
        }
    }
}
