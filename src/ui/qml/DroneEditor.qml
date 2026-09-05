pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// Six strings and a swell. The strings are the drone - each one an interval
// from the root, a few cents off, its own weight - and they are drawn as
// strings because that is what the hand understands: touch one where you want
// it to sound, slide sideways to bend it, roll the wheel to move it to the
// next note. The swell on the right is the one control a drone performer
// rides; everything else is weather, and lives in the small bars below it.
//
// Nothing here is a switch. Every gesture lands on a smoothed value in the
// plugin, so the drone can be played from this window without a click.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property var params: []
    property var mapped: []
    property var gains: [0, 0, 0, 0, 0, 0]
    property real swellPos: 0
    property real breath: 0
    property real peak: 0
    property real rootNow: 38
    property bool mapping: false
    property int waitingParam: -1
    property bool positioned: false
    // Seconds since the editor opened, advanced by the poll. The strings'
    // motion is drawn from it rather than from a per-string animation, so
    // six canvases stay in step and stop together when the popup closes.
    property real clock: 0

    // Parameter ids, mirrored from DroneInstance.
    readonly property int pSwell: 24
    readonly property int pRise: 25
    readonly property int pRoot: 26
    readonly property int pJust: 27
    readonly property int pDrift: 28
    readonly property int pTide: 29
    readonly property int pCutoff: 30
    readonly property int pResonance: 31
    readonly property int pMotion: 32
    readonly property int pSpace: 33
    readonly property int pGrit: 34
    readonly property int pWidth: 35
    readonly property int pGlide: 36

    readonly property var noteNames: ["C", "C#", "D", "D#", "E", "F",
                                      "F#", "G", "G#", "A", "A#", "B"]
    readonly property var intervalNames: ["8ve", "m2", "M2", "m3", "M3", "P4",
                                          "TT", "P5", "m6", "M6", "m7", "M7"]

    function p(id) {
        const v = root.params[id]
        return typeof v === "number" ? v : 0
    }

    function setP(id, value) {
        Mixer.setDroneParam(root.targetRow, root.targetSlot, id, value)
        const next = root.params.slice()
        next[id] = value
        root.params = next
    }

    function isMapped(id) { return root.mapped[id] === true }

    function noteName(midi) {
        const n = Math.round(midi)
        return root.noteNames[((n % 12) + 12) % 12] + (Math.floor(n / 12) - 1)
    }

    function hzOf(midi) { return 440 * Math.pow(2, (midi - 69) / 12) }

    // Every pitch class gets its own hue, so a fifth is always the same
    // colour whichever string carries it, and the root is always the same
    // as the glow behind the whole set. Octaves of one class share the hue
    // and differ in weight: lower is deeper, higher is paler.
    function pitchHue(semitones) {
        const n = Math.round(semitones)
        const cls = ((n % 12) + 12) % 12
        const oct = Math.max(-2, Math.min(2, Math.floor(n / 12)))
        return Qt.hsva(cls / 12, 0.58 - 0.09 * oct, 0.86 + 0.05 * oct, 1.0)
    }

    function intervalLabel(semitones) {
        if (semitones === 0) return qsTr("ROOT")
        return (semitones > 0 ? "+" : "−") + Math.abs(semitones)
    }

    function intervalSub(semitones) {
        const cls = ((semitones % 12) + 12) % 12
        const oct = Math.floor(semitones / 12)
        if (semitones === 0) return ""
        if (cls === 0) return (oct > 0 ? "+" : "") + oct + " " + qsTr("oct")
        return root.intervalNames[cls] + (oct !== 0 ? " " + (oct > 0 ? "+" : "") + oct : "")
    }

    // Rise, tide and glide are exponential in the plugin; the bars are linear
    // in the hand. These pairs go between the two.
    function riseToBar(seconds) { return Math.log2(Math.max(0.05, seconds) / 0.05) / Math.log2(600) }
    function barToRise(t) { return 0.05 * Math.pow(600, Math.max(0, Math.min(1, t))) }
    function tidePeriod(t) { return 1 / (0.01 * Math.pow(2, t * 6.64)) }
    function glideSeconds(t) { return 0.05 * Math.pow(2, t * 7.3) }
    function cutoffHz(t) { return 40 * Math.pow(2, t * 8.64) }

    function fmtSeconds(s) {
        if (s >= 10) return Math.round(s) + " s"
        if (s >= 1) return s.toFixed(1) + " s"
        return Math.round(s * 1000) + " ms"
    }

    function fmtHz(hz) {
        return hz >= 1000 ? (hz / 1000).toFixed(1) + " kHz" : Math.round(hz) + " Hz"
    }

    width: Px.px(900)
    height: Px.px(620)
    modal: false
    padding: Skin.spacingL
    closePolicy: (root.mapping || Mixer.learning)
                 ? Popup.NoAutoClose
                 : Popup.CloseOnEscape

    background: Rectangle {
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.lighter(Skin.popup, 1.08) }
            GradientStop { position: 1.0; color: Skin.popup }
        }
        border.width: 1
        border.color: Skin.border
        radius: Skin.radiusL
        clip: true

        // The glow: light under a door, in the root's colour, as bright as
        // the drone is loud. It is the only thing in the window that moves
        // when nobody is touching it, and it says the drone is alive with the
        // popup half seen across a dark room.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: parent.height * 0.4
            radius: parent.radius
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: root.pitchHue(0) }
            }
            opacity: Math.min(0.32, root.peak * 0.5)
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

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

    function openFor(row, slot) {
        root.targetRow = row
        root.targetSlot = slot
        root.refresh()
        if (!root.positioned) {
            // Clamped: opened before the window has its size, the centre of
            // a zero-sized overlay is off the top-left corner.
            root.x = Math.max(0, Math.round((Overlay.overlay.width - root.width) / 2))
            root.y = Math.max(0, Math.round((Overlay.overlay.height - root.height) / 2))
            root.positioned = true
        }
        root.open()
    }

    onOpened: poll.start()
    onClosed: {
        poll.stop()
        root.stopMapping()
    }

    function refresh() {
        const snap = Mixer.insertDroneSnapshot(root.targetRow, root.targetSlot)
        if (snap.params === undefined) return
        root.params = snap.params
        root.mapped = snap.mapped
        root.gains = snap.gains
        root.swellPos = snap.swell
        root.breath = snap.breath
        root.peak = snap.peak
        root.rootNow = snap.rootNow
    }

    function armParam(id, min, max) {
        Mixer.learnInsertParam(root.targetRow, root.targetSlot, id, min, max)
        root.waitingParam = id
    }

    function stopMapping() {
        Mixer.cancelLearn()
        root.mapping = false
        root.waitingParam = -1
    }

    Connections {
        target: Mixer
        function onLearnChanged() {
            if (!Mixer.learning) {
                root.waitingParam = -1
                root.refresh()
            }
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: root.mapping || Mixer.learning
        onActivated: root.stopMapping()
    }

    Timer {
        id: poll
        interval: 40
        repeat: true
        onTriggered: {
            root.refresh()
            root.clock += poll.interval / 1000
        }
    }

    contentItem: ColumnLayout {
        spacing: Skin.spacing

        // --- header ------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Skin.spacingS

            Text {
                text: qsTr("DRONE")
                color: Skin.text
                font.pixelSize: Skin.fontL
                font.bold: true
                font.letterSpacing: Px.px(2)
            }

            Item { Layout.preferredWidth: Skin.spacing }

            // The root. Drag it sideways or roll the wheel to move it a
            // semitone at a time; a MIDI note into the strip does the same.
            // The frequency shown is the one sounding now, so a glide can be
            // watched arriving.
            Rectangle {
                id: rootChip
                Layout.preferredWidth: Px.px(190)
                Layout.preferredHeight: Skin.buttonHeight + Px.px(6)
                radius: Skin.radius
                color: Skin.slotEmpty
                border.width: root.waitingParam === root.pRoot ? Px.px(2) : 1
                border.color: root.waitingParam === root.pRoot ? Skin.solo
                            : root.mapping ? Skin.focus
                            : Qt.rgba(root.pitchHue(0).r, root.pitchHue(0).g,
                                      root.pitchHue(0).b, 0.6)
                readonly property real rootParam: root.p(root.pRoot)

                Row {
                    anchors.centerIn: parent
                    spacing: Skin.spacing
                    Text {
                        text: root.noteName(rootChip.rootParam)
                        color: root.pitchHue(0)
                        font.pixelSize: Skin.fontXL
                        font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: root.hzOf(root.rootNow).toFixed(1) + " Hz"
                        color: Skin.textDim
                        font.pixelSize: Skin.fontS
                        font.family: Skin.monoFamily
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                Rectangle {
                    visible: root.isMapped(root.pRoot)
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Skin.spacingXS
                    width: Px.px(8)
                    height: Px.px(8)
                    radius: width / 2
                    color: "transparent"
                    border.width: Px.px(2)
                    border.color: root.pitchHue(0)
                }

                HoverHandler { id: rootHover; cursorShape: Qt.SizeHorCursor }
                Tip {
                    text: qsTr("The root every string hangs from. Drag sideways or roll the wheel for a semitone; a MIDI note on this strip re-roots the drone and the strings glide there.")
                    visible: rootHover.hovered
                }
                TapHandler {
                    enabled: root.mapping
                    onTapped: root.armParam(root.pRoot, 24, 84)
                }
                DragHandler {
                    enabled: !root.mapping
                    target: null
                    property real pressRoot: 0
                    onActiveChanged: if (active) pressRoot = rootChip.rootParam
                    onCentroidChanged: if (active) {
                        const dx = centroid.position.x - centroid.pressPosition.x
                        const next = Math.round(pressRoot + dx / Px.px(14))
                        if (next !== rootChip.rootParam)
                            root.setP(root.pRoot, Math.max(24, Math.min(84, next)))
                    }
                }
                WheelHandler {
                    onWheel: event => root.setP(root.pRoot,
                        Math.max(24, Math.min(84, rootChip.rootParam + (event.angleDelta.y > 0 ? 1 : -1))))
                }
            }

            // A set of strings by name. Only the strings and the tuning
            // change; the root, the swell and the weather stay, so a preset
            // can be swapped under a drone that is sounding and it glides.
            StripButton {
                id: presetButton
                Layout.preferredHeight: Skin.buttonHeight + Px.px(6)
                label: qsTr("STRINGS")
                tip: qsTr("Choose a set of strings: tanpura, octaves, the harmonic series… The root, the swell and the weather stay where they are.")
                onClicked: {
                    const names = Mixer.dronePresetNames()
                    const entries = []
                    for (let i = 0; i < names.length; ++i) {
                        const index = i
                        entries.push({ label: names[i], action: () => {
                            Mixer.applyDronePreset(root.targetRow, root.targetSlot, index)
                            root.refresh()
                        } })
                    }
                    presetMenu.openAt(presetButton, entries, qsTr("Strings"))
                }
            }

            // Parented to the overlay, not to this popup's content: openAt
            // positions in overlay coordinates, and a popup declared inside
            // another one would otherwise add the outer popup's offset again.
            SlotMenu {
                id: presetMenu
                parent: Overlay.overlay
            }

            StripButton {
                Layout.preferredHeight: Skin.buttonHeight + Px.px(6)
                label: root.p(root.pJust) >= 0.5 ? qsTr("JUST") : qsTr("12-TET")
                active: root.p(root.pJust) >= 0.5
                activeColor: root.pitchHue(7)
                tip: qsTr("Just intonation tunes every interval as a small ratio - a fifth is exactly 3:2 - so the strings lock instead of beating against the piano's temperament. Off, they are tuned like a piano.")
                onClicked: root.setP(root.pJust, root.p(root.pJust) >= 0.5 ? 0 : 1)
            }

            Item { Layout.fillWidth: true }

            Text {
                visible: root.mapping || Mixer.learning
                text: qsTr("MAPPING")
                color: Skin.solo
                font.pixelSize: Skin.fontXS
                font.bold: true
                font.letterSpacing: Px.px(1)
            }

            StripButton {
                Layout.preferredHeight: Skin.buttonHeight + Px.px(6)
                label: qsTr("MAP")
                active: root.mapping || Mixer.learning
                activeColor: Skin.solo
                tip: qsTr("Bind a control to a knob on this strip's MIDI input. Press MAP, tap a bar or a string, then turn the knob. A string binds its level.")
                onClicked: {
                    if (root.mapping || Mixer.learning) root.stopMapping()
                    else {
                        root.mapping = true
                        root.waitingParam = -1
                    }
                }
            }
        }

        // --- body -------------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Skin.spacingL

            // --- the strings -----------------------------------------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: Px.px(540)
                radius: Skin.radiusL
                color: Qt.rgba(0, 0, 0, 0.25)
                border.width: 1
                border.color: Skin.line

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: Skin.spacing
                    spacing: Skin.spacingS

                    Repeater {
                        model: 6

                        Item {
                            id: str
                            required property int index
                            readonly property int base: str.index * 4
                            readonly property int interval: Math.round(root.p(str.base))
                            readonly property real detune: root.p(str.base + 1)
                            readonly property real level: root.p(str.base + 2)
                            readonly property real shape: root.p(str.base + 3)
                            readonly property real gain: {
                                const g = root.gains[str.index]
                                return typeof g === "number" ? g : 0
                            }
                            readonly property color hue: root.pitchHue(str.interval)
                            // How many bellies the string vibrates in: its
                            // harmonic number, for a string that is one.
                            readonly property int modes: Math.max(1, Math.min(6,
                                Math.round(Math.pow(2, str.interval / 12))))
                            readonly property bool waiting: root.waitingParam === str.base + 2
                            readonly property bool isMapped: root.isMapped(str.base + 2)
                            property real pulse: 0.35

                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            SequentialAnimation on pulse {
                                running: str.waiting
                                loops: Animation.Infinite
                                NumberAnimation { from: 0.25; to: 0.85; duration: 480; easing.type: Easing.InOutSine }
                                NumberAnimation { from: 0.85; to: 0.25; duration: 480; easing.type: Easing.InOutSine }
                            }

                            activeFocusOnTab: true
                            Accessible.role: Accessible.Slider
                            Accessible.name: qsTr("String %1").arg(str.index + 1)

                            Rectangle {
                                anchors.fill: parent
                                radius: Skin.radius
                                color: str.activeFocus || str.waiting ? Qt.rgba(1, 1, 1, 0.03) : "transparent"
                                border.width: str.waiting ? Px.px(2) : (root.mapping || str.activeFocus ? 1 : 0)
                                border.color: str.waiting ? Skin.solo : Skin.focus
                                opacity: str.waiting ? str.pulse + 0.15 : 1
                            }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: Skin.spacingXS
                                spacing: Skin.spacingXS

                                // Interval, on a chip in the string's colour.
                                Rectangle {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.preferredWidth: Math.max(Px.px(52), intervalText.implicitWidth + Skin.spacing * 2)
                                    Layout.preferredHeight: Px.px(34)
                                    radius: Skin.radius
                                    color: Qt.rgba(str.hue.r, str.hue.g, str.hue.b, 0.16)
                                    border.width: 1
                                    border.color: Qt.rgba(str.hue.r, str.hue.g, str.hue.b, 0.5)
                                    Column {
                                        anchors.centerIn: parent
                                        spacing: 0
                                        Text {
                                            id: intervalText
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: root.intervalLabel(str.interval)
                                            color: str.hue
                                            font.pixelSize: Skin.fontL
                                            font.bold: true
                                            font.family: Skin.monoFamily
                                        }
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            visible: text.length > 0
                                            text: root.intervalSub(str.interval)
                                            color: Skin.textDim
                                            font.pixelSize: Skin.fontXS
                                        }
                                    }
                                }

                                // The string itself.
                                Item {
                                    id: stringArea
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true

                                    Canvas {
                                        id: canvas
                                        anchors.fill: parent
                                        // Bound to the clock so it repaints on
                                        // every poll while open, and only then.
                                        readonly property real tick: root.clock
                                        onTickChanged: requestPaint()
                                        onLevelChanged: requestPaint()
                                        readonly property real level: str.level
                                        onPaint: {
                                            const ctx = getContext("2d")
                                            const w = width, h = height
                                            ctx.clearRect(0, 0, w, h)
                                            const cx = w / 2
                                            const t = root.clock
                                            // Beat: how fast the string's swing
                                            // breathes. Detune is what makes a
                                            // drone beat, so it is what makes
                                            // the drawing breathe.
                                            const beat = 0.35 + Math.abs(str.detune) * 0.22
                                            const breathe = 0.6 + 0.4 * Math.sin(2 * Math.PI * beat * t)
                                            const sway = Math.sin(2 * Math.PI * (1.7 + str.index * 0.23) * t)
                                            const amp = Math.min(w * 0.42, Math.sqrt(Math.max(0, str.gain)) * w * 0.5) * breathe * sway
                                            const lowness = 1 - Math.max(0, Math.min(1, (str.interval + 24) / 48))
                                            const c = str.hue

                                            // Rest line, faint, so an idle
                                            // string is still a string.
                                            ctx.strokeStyle = Qt.rgba(c.r, c.g, c.b, 0.18)
                                            ctx.lineWidth = 1
                                            ctx.beginPath()
                                            ctx.moveTo(cx, 0)
                                            ctx.lineTo(cx, h)
                                            ctx.stroke()

                                            // The bellies.
                                            const alpha = 0.35 + 0.65 * Math.min(1, str.level)
                                            ctx.strokeStyle = Qt.rgba(c.r, c.g, c.b, alpha)
                                            ctx.lineWidth = 1.5 + 2.5 * lowness
                                            ctx.lineCap = "round"
                                            ctx.beginPath()
                                            const step = 3
                                            for (let y = 0; y <= h; y += step) {
                                                const x = cx + amp * Math.sin(Math.PI * str.modes * y / h)
                                                if (y === 0) ctx.moveTo(x, y)
                                                else ctx.lineTo(x, y)
                                            }
                                            ctx.stroke()

                                            // Nodes, where the string stands
                                            // still: one dot per boundary
                                            // between bellies.
                                            ctx.fillStyle = Qt.rgba(c.r, c.g, c.b, 0.7)
                                            for (let n = 1; n < str.modes; ++n) {
                                                ctx.beginPath()
                                                ctx.arc(cx, n * h / str.modes, 2.2, 0, 2 * Math.PI)
                                                ctx.fill()
                                            }

                                            // The bridge: where the level is
                                            // set, as a bar across the string.
                                            const ly = h - Math.max(0, Math.min(1, str.level)) * h
                                            ctx.fillStyle = Qt.rgba(c.r, c.g, c.b, 0.95)
                                            ctx.fillRect(cx - w * 0.28, ly - 1.5, w * 0.56, 3)
                                        }
                                    }

                                    // Where you touch a string is its level;
                                    // sliding sideways bends it, in cents. The
                                    // axis is chosen by the first movement so a
                                    // finger that wobbles does not do both.
                                    HoverHandler {
                                        id: stringHover
                                        cursorShape: Qt.SizeAllCursor
                                    }
                                    Tip {
                                        text: qsTr("Touch to set how loud this string is; slide sideways to bend it a few cents so it beats. Wheel moves it a semitone, Shift+wheel a cent. Keys: up/down level, left/right detune, PgUp/PgDn interval, Home undoes the bend.")
                                        visible: stringHover.hovered && !stringDrag.active
                                    }
                                    TapHandler {
                                        enabled: root.mapping
                                        onTapped: root.armParam(str.base + 2, 0, 1)
                                    }
                                    DragHandler {
                                        id: stringDrag
                                        enabled: !root.mapping
                                        target: null
                                        dragThreshold: 0
                                        grabPermissions: PointerHandler.CanTakeOverFromAnything
                                                         | PointerHandler.ApprovesTakeOverByNothing
                                        property int axis: 0  // 0 undecided, 1 level, 2 detune
                                        property real pressDetune: 0
                                        onActiveChanged: {
                                            if (active) {
                                                str.forceActiveFocus(Qt.MouseFocusReason)
                                                axis = 0
                                                pressDetune = str.detune
                                            }
                                        }
                                        onCentroidChanged: {
                                            if (!active) return
                                            const dx = centroid.position.x - centroid.pressPosition.x
                                            const dy = centroid.position.y - centroid.pressPosition.y
                                            if (axis === 0) {
                                                if (Math.abs(dx) < Px.px(6) && Math.abs(dy) < Px.px(6)) {
                                                    // A touch with no travel yet is a level.
                                                    root.setP(str.base + 2, 1 - Math.max(0, Math.min(1, centroid.position.y / Math.max(1, stringArea.height))))
                                                    return
                                                }
                                                axis = Math.abs(dx) > Math.abs(dy) ? 2 : 1
                                            }
                                            if (axis === 1)
                                                root.setP(str.base + 2, 1 - Math.max(0, Math.min(1, centroid.position.y / Math.max(1, stringArea.height))))
                                            else
                                                root.setP(str.base + 1, Math.max(-50, Math.min(50, Math.round(pressDetune + dx / Px.px(3)))))
                                        }
                                    }
                                    WheelHandler {
                                        acceptedModifiers: Qt.NoModifier
                                        onWheel: event => root.setP(str.base,
                                            Math.max(-24, Math.min(24, str.interval + (event.angleDelta.y > 0 ? 1 : -1))))
                                    }
                                    WheelHandler {
                                        acceptedModifiers: Qt.ShiftModifier
                                        onWheel: event => root.setP(str.base + 1,
                                            Math.max(-50, Math.min(50, str.detune + (event.angleDelta.y > 0 ? 1 : -1))))
                                    }
                                }

                                // Detune, in cents, in the string's colour.
                                Text {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: (str.detune > 0 ? "+" : str.detune < 0 ? "−" : "") + Math.abs(Math.round(str.detune)) + "¢"
                                    color: Math.abs(str.detune) > 0.5 ? str.hue : Skin.textDim
                                    font.pixelSize: Skin.fontS
                                    font.family: Skin.monoFamily
                                }

                                // Shape: sine on the left, saw on the right.
                                Item {
                                    id: shapeBar
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: Px.px(18)
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: Skin.radiusS
                                        color: Skin.slotEmpty
                                        border.width: 1
                                        border.color: Qt.rgba(str.hue.r, str.hue.g, str.hue.b, 0.35)
                                        clip: true
                                        Rectangle {
                                            anchors.left: parent.left
                                            anchors.top: parent.top
                                            anchors.bottom: parent.bottom
                                            width: Math.max(0, Math.min(1, str.shape)) * parent.width
                                            color: Qt.rgba(str.hue.r, str.hue.g, str.hue.b, 0.55)
                                        }
                                        Text {
                                            anchors.centerIn: parent
                                            text: str.shape < 0.25 ? qsTr("SINE")
                                                : str.shape < 0.75 ? qsTr("TRI") : qsTr("SAW")
                                            color: Skin.text
                                            font.pixelSize: Skin.fontXS
                                            font.bold: true
                                            style: Text.Outline
                                            styleColor: Qt.rgba(0, 0, 0, 0.6)
                                        }
                                    }
                                    HoverHandler { id: shapeHover; cursorShape: Qt.SizeHorCursor }
                                    Tip {
                                        text: qsTr("How many overtones the string has: a sine on the left, a triangle in the middle, a saw on the right.")
                                        visible: shapeHover.hovered
                                    }
                                    TapHandler {
                                        enabled: root.mapping
                                        onTapped: root.armParam(str.base + 3, 0, 1)
                                    }
                                    DragHandler {
                                        enabled: !root.mapping
                                        target: null
                                        dragThreshold: 0
                                        onActiveChanged: if (active) root.setP(str.base + 3, Math.max(0, Math.min(1, centroid.position.x / Math.max(1, shapeBar.width))))
                                        onCentroidChanged: if (active) root.setP(str.base + 3, Math.max(0, Math.min(1, centroid.position.x / Math.max(1, shapeBar.width))))
                                    }
                                }
                            }

                            Keys.onPressed: event => {
                                const fine = (event.modifiers & Qt.ShiftModifier) !== 0
                                switch (event.key) {
                                case Qt.Key_Up:
                                    root.setP(str.base + 2, Math.min(1, str.level + (fine ? 0.01 : 0.05)))
                                    event.accepted = true; break
                                case Qt.Key_Down:
                                    root.setP(str.base + 2, Math.max(0, str.level - (fine ? 0.01 : 0.05)))
                                    event.accepted = true; break
                                case Qt.Key_Right:
                                    root.setP(str.base + 1, Math.min(50, str.detune + 1))
                                    event.accepted = true; break
                                case Qt.Key_Left:
                                    root.setP(str.base + 1, Math.max(-50, str.detune - 1))
                                    event.accepted = true; break
                                case Qt.Key_PageUp:
                                    root.setP(str.base, Math.min(24, str.interval + (fine ? 12 : 1)))
                                    event.accepted = true; break
                                case Qt.Key_PageDown:
                                    root.setP(str.base, Math.max(-24, str.interval - (fine ? 12 : 1)))
                                    event.accepted = true; break
                                case Qt.Key_Home:
                                    root.setP(str.base + 1, 0)
                                    event.accepted = true; break
                                }
                            }
                        }
                    }
                }
            }

            // --- the swell and the weather ------------------------------------------
            // A layout fills by default; this one is told not to, or it takes
            // the strings' room.
            ColumnLayout {
                Layout.fillWidth: false
                Layout.preferredWidth: Px.px(300)
                Layout.maximumWidth: Px.px(300)
                Layout.fillHeight: true
                spacing: Skin.spacing

                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: Skin.spacing

                    ParamBar {
                        Layout.preferredWidth: Px.px(84)
                        Layout.fillHeight: true
                        big: true
                        label: qsTr("SWELL")
                        hue: root.pitchHue(0)
                        value: root.p(root.pSwell)
                        marker: root.swellPos
                        readout: Math.round(root.p(root.pSwell) * 100)
                        mapped: root.isMapped(root.pSwell)
                        waiting: root.waitingParam === root.pSwell
                        mapping: root.mapping
                        tip: qsTr("The drone's one pedal. Push it up and the whole set rises over the Rise time; pull it down and it sinks the same way, leaving the room ringing. The light line is where the sound has actually got to.")
                        onEdited: v => root.setP(root.pSwell, v)
                        onArmed: root.armParam(root.pSwell, 0, 1)
                    }

                    // The sky: cutoff across, resonance up, and a dot for
                    // where the filter is breathing right now.
                    Item {
                        id: sky
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        readonly property real cutoff: root.p(root.pCutoff)
                        readonly property real resonance: root.p(root.pResonance)
                        readonly property real motion: root.p(root.pMotion)
                        readonly property real breathing: Math.max(0, Math.min(1,
                            sky.cutoff + sky.motion * root.breath * 2.5 / 8.64))
                        readonly property bool waiting: root.waitingParam === root.pCutoff
                                                        || root.waitingParam === root.pResonance
                        readonly property color hue: root.pitchHue(4)

                        Rectangle {
                            anchors.fill: parent
                            radius: Skin.radius
                            color: Skin.slotEmpty
                            border.width: sky.waiting ? Px.px(2) : 1
                            border.color: sky.waiting ? Skin.solo
                                        : root.mapping ? Skin.focus
                                        : sky.activeFocus ? Skin.focus
                                        : Qt.rgba(sky.hue.r, sky.hue.g, sky.hue.b, 0.45)
                            clip: true

                            // Brighter to the right, the way an open filter is.
                            Rectangle {
                                anchors.fill: parent
                                gradient: Gradient {
                                    orientation: Gradient.Horizontal
                                    GradientStop { position: 0.0; color: "transparent" }
                                    GradientStop { position: 1.0; color: Qt.rgba(sky.hue.r, sky.hue.g, sky.hue.b, 0.22) }
                                }
                            }

                            // The range the breath covers, as a band.
                            Rectangle {
                                x: Math.max(0, Math.min(1, sky.cutoff - sky.motion * 2.5 / 8.64)) * parent.width
                                width: Math.max(0, Math.min(parent.width - x,
                                    (sky.motion * 2 * 2.5 / 8.64) * parent.width))
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                color: Qt.rgba(sky.hue.r, sky.hue.g, sky.hue.b, 0.10)
                            }

                            // Crosshair at the setting.
                            Rectangle {
                                x: sky.cutoff * parent.width - width / 2
                                width: 1
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                color: Qt.rgba(sky.hue.r, sky.hue.g, sky.hue.b, 0.6)
                            }
                            Rectangle {
                                y: (1 - sky.resonance) * parent.height - height / 2
                                height: 1
                                anchors.left: parent.left
                                anchors.right: parent.right
                                color: Qt.rgba(sky.hue.r, sky.hue.g, sky.hue.b, 0.6)
                            }

                            // The breathing dot.
                            Rectangle {
                                x: sky.breathing * parent.width - width / 2
                                y: (1 - sky.resonance) * parent.height - height / 2
                                width: Px.px(12)
                                height: Px.px(12)
                                radius: width / 2
                                color: Qt.lighter(sky.hue, 1.4)
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: parent.width * 2.2
                                    height: width
                                    radius: width / 2
                                    color: "transparent"
                                    border.width: Px.px(2)
                                    border.color: sky.hue
                                    opacity: 0.45
                                }
                            }

                            Rectangle {
                                anchors.top: parent.top
                                anchors.left: parent.left
                                anchors.margins: Skin.spacingXS
                                radius: Skin.radiusS
                                color: Qt.rgba(0, 0, 0, 0.4)
                                width: skyLabel.implicitWidth + Skin.spacingS * 2
                                height: skyLabel.implicitHeight + Skin.spacingXS * 1.5
                                Text {
                                    id: skyLabel
                                    anchors.centerIn: parent
                                    text: qsTr("SKY")
                                    color: Skin.text
                                    font.pixelSize: Skin.fontXS
                                    font.bold: true
                                    font.letterSpacing: Px.px(1)
                                }
                            }
                            Text {
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.margins: Skin.spacingS
                                text: root.fmtHz(root.cutoffHz(sky.cutoff))
                                color: Skin.text
                                font.pixelSize: Skin.fontXS
                                font.family: Skin.monoFamily
                                style: Text.Outline
                                styleColor: Qt.rgba(0, 0, 0, 0.55)
                            }
                            Text {
                                anchors.bottom: parent.bottom
                                anchors.right: parent.right
                                anchors.margins: Skin.spacingS
                                text: "Q " + Math.round(sky.resonance * 100)
                                color: Skin.text
                                font.pixelSize: Skin.fontXS
                                font.family: Skin.monoFamily
                                style: Text.Outline
                                styleColor: Qt.rgba(0, 0, 0, 0.55)
                            }
                            Rectangle {
                                visible: root.isMapped(root.pCutoff) || root.isMapped(root.pResonance)
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: Skin.spacingXS
                                width: Px.px(8)
                                height: Px.px(8)
                                radius: width / 2
                                color: "transparent"
                                border.width: Px.px(2)
                                border.color: sky.waiting ? Skin.solo : sky.hue
                            }
                        }

                        activeFocusOnTab: true
                        Accessible.role: Accessible.Slider
                        Accessible.name: qsTr("Sky")

                        HoverHandler { id: skyHover; cursorShape: Qt.CrossCursor }
                        Tip {
                            text: qsTr("The filter. Across is how open it is, up is how much it rings. The dot is where it is breathing to right now; Motion sets how far it wanders from the mark, Tide how slowly. In MAP, tap left of the mark to bind the cutoff, right of it to bind the resonance.")
                            visible: skyHover.hovered && !skyDrag.active
                        }
                        TapHandler {
                            enabled: root.mapping
                            onTapped: eventPoint => {
                                if (eventPoint.position.x < sky.width / 2)
                                    root.armParam(root.pCutoff, 0, 1)
                                else
                                    root.armParam(root.pResonance, 0, 1)
                            }
                        }
                        DragHandler {
                            id: skyDrag
                            enabled: !root.mapping
                            target: null
                            dragThreshold: 0
                            grabPermissions: PointerHandler.CanTakeOverFromAnything
                                             | PointerHandler.ApprovesTakeOverByNothing
                            function apply() {
                                sky.forceActiveFocus(Qt.MouseFocusReason)
                                root.setP(root.pCutoff, Math.max(0, Math.min(1, centroid.position.x / Math.max(1, sky.width))))
                                root.setP(root.pResonance, 1 - Math.max(0, Math.min(1, centroid.position.y / Math.max(1, sky.height))))
                            }
                            onActiveChanged: if (active) apply()
                            onCentroidChanged: if (active) apply()
                        }
                        WheelHandler {
                            acceptedModifiers: Qt.NoModifier
                            onWheel: event => root.setP(root.pCutoff,
                                Math.max(0, Math.min(1, sky.cutoff + (event.angleDelta.y > 0 ? 0.02 : -0.02))))
                        }
                        WheelHandler {
                            acceptedModifiers: Qt.ShiftModifier
                            onWheel: event => root.setP(root.pResonance,
                                Math.max(0, Math.min(1, sky.resonance + (event.angleDelta.y > 0 ? 0.02 : -0.02))))
                        }
                        Keys.onPressed: event => {
                            switch (event.key) {
                            case Qt.Key_Right: root.setP(root.pCutoff, Math.min(1, sky.cutoff + 0.02)); event.accepted = true; break
                            case Qt.Key_Left: root.setP(root.pCutoff, Math.max(0, sky.cutoff - 0.02)); event.accepted = true; break
                            case Qt.Key_Up: root.setP(root.pResonance, Math.min(1, sky.resonance + 0.02)); event.accepted = true; break
                            case Qt.Key_Down: root.setP(root.pResonance, Math.max(0, sky.resonance - 0.02)); event.accepted = true; break
                            }
                        }
                    }
                }

                // The weather: two rows of four.
                GridLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Px.px(150)
                    columns: 4
                    columnSpacing: Skin.spacingS
                    rowSpacing: Skin.spacingS

                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("DRIFT"); hue: root.pitchHue(2)
                        value: root.p(root.pDrift)
                        readout: Math.round(root.p(root.pDrift) * 100)
                        mapped: root.isMapped(root.pDrift)
                        waiting: root.waitingParam === root.pDrift
                        mapping: root.mapping
                        tip: qsTr("How far each string wanders from where you left it, in pitch and in weight. Nothing at the bottom; at the top, a set that never sits still.")
                        onEdited: v => root.setP(root.pDrift, v)
                        onArmed: root.armParam(root.pDrift, 0, 1)
                    }
                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("TIDE"); hue: root.pitchHue(5)
                        value: root.p(root.pTide)
                        readout: root.fmtSeconds(root.tidePeriod(root.p(root.pTide)))
                        mapped: root.isMapped(root.pTide)
                        waiting: root.waitingParam === root.pTide
                        mapping: root.mapping
                        tip: qsTr("How slowly everything that wanders, wanders: the drift, and the filter's breath. Shown as the length of one breath.")
                        onEdited: v => root.setP(root.pTide, v)
                        onArmed: root.armParam(root.pTide, 0, 1)
                    }
                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("MOTION"); hue: root.pitchHue(4)
                        value: root.p(root.pMotion)
                        readout: Math.round(root.p(root.pMotion) * 100)
                        mapped: root.isMapped(root.pMotion)
                        waiting: root.waitingParam === root.pMotion
                        mapping: root.mapping
                        tip: qsTr("How far the filter breathes from its mark on the sky, with the tide.")
                        onEdited: v => root.setP(root.pMotion, v)
                        onArmed: root.armParam(root.pMotion, 0, 1)
                    }
                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("SPACE"); hue: root.pitchHue(9)
                        value: root.p(root.pSpace)
                        readout: Math.round(root.p(root.pSpace) * 100)
                        mapped: root.isMapped(root.pSpace)
                        waiting: root.waitingParam === root.pSpace
                        mapping: root.mapping
                        tip: qsTr("The room. Higher is wetter and longer, up to a tail that outlasts the swell by half a minute. It sits after the swell, so pulling the drone down leaves the room ringing.")
                        onEdited: v => root.setP(root.pSpace, v)
                        onArmed: root.armParam(root.pSpace, 0, 1)
                    }
                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("GRIT"); hue: root.pitchHue(10)
                        value: root.p(root.pGrit)
                        readout: Math.round(root.p(root.pGrit) * 100)
                        mapped: root.isMapped(root.pGrit)
                        waiting: root.waitingParam === root.pGrit
                        mapping: root.mapping
                        tip: qsTr("Saturation before the filter. A little thickens the strings into one; a lot is an organ through a bad amplifier.")
                        onEdited: v => root.setP(root.pGrit, v)
                        onArmed: root.armParam(root.pGrit, 0, 1)
                    }
                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("WIDTH"); hue: root.pitchHue(7)
                        value: root.p(root.pWidth)
                        readout: Math.round(root.p(root.pWidth) * 100)
                        mapped: root.isMapped(root.pWidth)
                        waiting: root.waitingParam === root.pWidth
                        mapping: root.mapping
                        tip: qsTr("Left and right tuned a few cents apart, in opposite directions on alternate strings, and the strings leaned off centre. The root stays in the middle.")
                        onEdited: v => root.setP(root.pWidth, v)
                        onArmed: root.armParam(root.pWidth, 0, 1)
                    }
                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("GLIDE"); hue: root.pitchHue(11)
                        value: root.p(root.pGlide)
                        readout: root.fmtSeconds(root.glideSeconds(root.p(root.pGlide)))
                        mapped: root.isMapped(root.pGlide)
                        waiting: root.waitingParam === root.pGlide
                        mapping: root.mapping
                        tip: qsTr("How long a change of root or interval takes to arrive. Short is a keyboard; long is a set of strings being retuned while they sound.")
                        onEdited: v => root.setP(root.pGlide, v)
                        onArmed: root.armParam(root.pGlide, 0, 1)
                    }
                    ParamBar {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        label: qsTr("RISE"); hue: root.pitchHue(0)
                        value: root.riseToBar(root.p(root.pRise))
                        readout: root.fmtSeconds(root.p(root.pRise))
                        mapped: root.isMapped(root.pRise)
                        waiting: root.waitingParam === root.pRise
                        mapping: root.mapping
                        tip: qsTr("How long the swell takes to go all the way up, or all the way down. Fifty milliseconds to half a minute.")
                        onEdited: v => root.setP(root.pRise, root.barToRise(v))
                        onArmed: root.armParam(root.pRise, 0.05, 30)
                    }
                }
            }
        }

        // --- status line -------------------------------------------------------------
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: root.waitingParam >= 0
                  ? qsTr("Turn a knob on this strip's MIDI input… Esc cancels.")
                  : root.mapping
                    ? qsTr("Tap the control you want, then turn a knob.")
                    : qsTr("Touch a string where it should sound, slide to bend it, roll to move it. Push the swell.")
            color: root.mapping || Mixer.learning ? Skin.solo : Skin.textDim
            font.pixelSize: Skin.fontXS
            wrapMode: Text.WordWrap
        }
    }
}
