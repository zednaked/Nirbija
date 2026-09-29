pragma ComponentBehavior: Bound
// SPDX-License-Identifier: GPL-3.0-only

import QtQuick
import QtQuick.Layouts
import Nirbija

// The scenes, across the top of the mixer: the song's parts in the order they
// play, each as wide as it is long, so the ribbon reads as the shape of the
// music before anything sounds. The one playing fills as its bars go by; the
// one armed waits, blinking, for the next bar line. See design/scenes.md.
Rectangle {
    id: root

    signal sceneMenuRequested(int scene, var item)

    readonly property var scenes: Mixer.scenes
    readonly property int beatsPerBar: Math.max(1, Mixer.timeNumerator())
    // An open-ended scene takes the room of eight bars on the ribbon.
    readonly property int openBars: 8

    implicitHeight: Px.px(104)
    height: implicitHeight
    color: Skin.bar

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Skin.line
    }

    // No translation is loaded, so a qsTr plural form would reach the
    // screen as written; the English plural is spelled out here instead.
    function barsText(count) {
        return count === 1 ? qsTr("1 bar") : qsTr("%1 bars").arg(count)
    }

    function hueOf(scene) {
        return Qt.hsla(scene.hue, 0.55, 0.64, 1.0)
    }

    function escaped(text) {
        return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;")
    }

    function statusText() {
        const list = root.scenes
        if (list.length === 0)
            return qsTr("Add a scene, press Record, then move what should change in it.")
        const current = Mixer.currentScene
        const armed = Mixer.armedScene
        let text = ""
        if (current < 0) {
            text = armed >= 0
                   ? qsTr("<b>%1</b> starts on the next bar").arg(root.escaped(list[armed].name))
                   : Mixer.playing ? qsTr("Pick a scene to start it on the next bar")
                                   : qsTr("Stopped · play starts from the first scene")
        } else {
            const scene = list[current]
            const bar = Math.max(0, Mixer.sceneBar)
            text = "<b>" + root.escaped(scene.name) + "</b> · "
                 + (scene.bars > 0 ? qsTr("bar %1/%2").arg(Math.min(bar, scene.bars - 1) + 1).arg(scene.bars)
                                   : qsTr("bar %1").arg(bar + 1))
            if (armed >= 0)
                text += " · " + qsTr("<b>%1</b> on the next bar").arg(root.escaped(list[armed].name))
            else if (Mixer.sceneHold)
                text += " · " + qsTr("holding")
            else if (scene.bars === 0)
                text += " · " + qsTr("stays until you change it")
            else if (Mixer.sceneAuto && current + 1 < list.length)
                text += " · " + qsTr("%1 in %2").arg(root.escaped(list[current + 1].name))
                                                   .arg(root.barsText(Math.max(1, scene.bars - bar)))
        }
        if (Mixer.sceneRecording && Mixer.recordScene >= 0)
            text += " · <font color=\"" + Skin.arm + "\">"
                  + qsTr("recording into %1").arg(root.escaped(list[Mixer.recordScene].name))
                  + "</font>"
        return text
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: Skin.spacing
        anchors.rightMargin: Skin.spacing
        anchors.topMargin: Skin.spacingS
        anchors.bottomMargin: Skin.spacingS + 1
        spacing: Skin.spacingS

        // --- header ---------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: Px.px(24)
            spacing: Skin.spacingS

            Text {
                text: qsTr("SCENES")
                color: Skin.text
                font.pixelSize: Skin.fontS
                font.bold: true
                font.letterSpacing: Px.px(2)
                Layout.rightMargin: Skin.spacingS
            }

            StripButton {
                Layout.preferredHeight: Px.px(22)
                label: qsTr("Queue")
                tip: qsTr("The list walks on by itself: when a scene has played its bars, the next one starts.")
                active: Mixer.sceneAuto
                onClicked: Mixer.sceneAuto = !Mixer.sceneAuto
            }

            StripButton {
                Layout.preferredHeight: Px.px(22)
                label: qsTr("Hold")
                tip: qsTr("Keep playing the scene you are in, over and over, until you let go (Alt+H).")
                active: Mixer.sceneHold
                activeColor: Skin.solo
                onClicked: Mixer.sceneHold = !Mixer.sceneHold
            }

            StripButton {
                id: recButton
                Layout.preferredHeight: Px.px(22)
                label: qsTr("● Record scene")
                tip: qsTr("While this is lit, every fader, on/off and pattern you touch on a strip that follows scenes goes into the scene outlined in red (Alt+R).")
                active: Mixer.sceneRecording
                activeColor: Skin.arm
                onClicked: Mixer.sceneRecording = !Mixer.sceneRecording

                // Recording is a mode you can forget you are in; it breathes.
                SequentialAnimation on opacity {
                    running: Mixer.sceneRecording
                    loops: Animation.Infinite
                    onRunningChanged: if (!running) recButton.opacity = 1
                    NumberAnimation { to: 0.6; duration: 550; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 1.0; duration: 550; easing.type: Easing.InOutSine }
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.leftMargin: Skin.spacing
                text: root.statusText()
                textFormat: Text.StyledText
                color: Skin.textDim
                font.pixelSize: Skin.fontS
                font.family: Skin.monoFamily
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideLeft
            }
        }

        // --- the song -------------------------------------------------------
        Flickable {
            id: lane
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: tiles.width
            contentHeight: height
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds
            clip: true

            readonly property real addWidth: Px.px(44)
            readonly property real minTile: Px.px(108)
            readonly property int totalBars: {
                let total = 0
                for (const scene of root.scenes)
                    total += scene.bars > 0 ? scene.bars : root.openBars
                return Math.max(1, total)
            }
            // Width per bar: the whole song across the ribbon when it fits,
            // never so thin a short scene cannot show its name.
            readonly property real unit: Math.max(
                Px.px(6),
                (lane.width - lane.addWidth - tiles.spacing * root.scenes.length)
                / lane.totalBars)

            Row {
                id: tiles
                height: lane.height
                spacing: Px.px(4)

                Repeater {
                    model: root.scenes

                    Rectangle {
                        id: tile

                        required property int index
                        required property var modelData

                        readonly property color hue: root.hueOf(tile.modelData)
                        readonly property int bars: tile.modelData.bars
                        readonly property int lengthBars: tile.bars > 0 ? tile.bars : root.openBars
                        readonly property bool current: tile.index === Mixer.currentScene
                        readonly property bool armed: tile.index === Mixer.armedScene
                        readonly property bool recording: Mixer.sceneRecording
                                                          && tile.index === Mixer.recordScene
                        readonly property bool past: Mixer.currentScene >= 0
                                                     && tile.index < Mixer.currentScene
                                                     && !tile.armed
                        readonly property real progress: {
                            if (!tile.current || Mixer.sceneBar < 0) return 0
                            const into = Mixer.sceneBar + (Mixer.playing ? Mixer.sceneBarPhase : 0)
                            if (tile.bars > 0) return Math.min(1, into / tile.bars)
                            return (into % root.openBars) / root.openBars
                        }

                        width: Math.max(lane.minTile, tile.lengthBars * lane.unit)
                        height: tiles.height
                        radius: Skin.radius
                        clip: true
                        color: tile.current ? Qt.tint(Skin.strip, Qt.rgba(tile.hue.r, tile.hue.g, tile.hue.b, 0.07))
                             : tileHover.hovered ? Skin.stripAlt : Skin.strip
                        border.width: 1
                        border.color: tile.current ? tile.hue
                                    : tileHover.hovered ? Qt.tint(Skin.border, Qt.rgba(tile.hue.r, tile.hue.g, tile.hue.b, 0.45))
                                    : Skin.border
                        opacity: tile.past ? 0.55 : 1.0

                        Behavior on opacity { NumberAnimation { duration: Skin.medium } }
                        Behavior on color { ColorAnimation { duration: Skin.medium } }

                        Accessible.role: Accessible.Button
                        Accessible.name: tile.modelData.name

                        // The fade, drawn as the ramp it is: from nothing at
                        // the scene's first bar line up to full where it ends.
                        Canvas {
                            id: wedge
                            anchors.fill: parent
                            readonly property real fadeWidth:
                                tile.modelData.fade <= 0 ? 0
                                : Math.min(1, tile.modelData.fade / tile.lengthBars) * width
                            onFadeWidthChanged: requestPaint()
                            onHeightChanged: requestPaint()
                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.reset()
                                if (fadeWidth <= 0) return
                                const gradient = ctx.createLinearGradient(0, 0, fadeWidth, 0)
                                gradient.addColorStop(0, Qt.rgba(tile.hue.r, tile.hue.g, tile.hue.b, 0.0))
                                gradient.addColorStop(1, Qt.rgba(tile.hue.r, tile.hue.g, tile.hue.b, 0.16))
                                ctx.fillStyle = gradient
                                ctx.beginPath()
                                ctx.moveTo(0, height)
                                ctx.lineTo(fadeWidth, 0)
                                ctx.lineTo(fadeWidth, height)
                                ctx.closePath()
                                ctx.fill()
                            }
                        }

                        // How far through the scene the song is.
                        Rectangle {
                            width: tile.progress * tile.width
                            height: parent.height
                            color: Qt.rgba(tile.hue.r, tile.hue.g, tile.hue.b, 0.18)
                            visible: tile.current
                        }

                        // The playhead, with a glow so it is the first thing
                        // the eye finds from across the room.
                        Rectangle {
                            x: tile.progress * tile.width - width / 2
                            width: Px.px(8)
                            height: parent.height
                            visible: tile.current && Mixer.sceneBar >= 0
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0.0; color: "transparent" }
                                GradientStop { position: 0.5; color: Qt.rgba(tile.hue.r, tile.hue.g, tile.hue.b, 0.45) }
                                GradientStop { position: 1.0; color: "transparent" }
                            }
                        }
                        Rectangle {
                            x: tile.progress * tile.width - width / 2
                            width: Px.px(2)
                            height: parent.height
                            color: tile.hue
                            visible: tile.current && Mixer.sceneBar >= 0
                        }

                        Rectangle {
                            anchors.top: parent.top
                            anchors.left: parent.left
                            anchors.right: parent.right
                            height: Px.px(3)
                            color: tile.hue
                            opacity: tile.current || tile.armed ? 1.0 : 0.35
                        }

                        // Armed: an outline that blinks until the bar line
                        // takes it.
                        Rectangle {
                            anchors.fill: parent
                            radius: Skin.radius
                            color: "transparent"
                            border.width: Px.px(2)
                            border.color: tile.hue
                            visible: tile.armed
                            SequentialAnimation on opacity {
                                running: tile.armed
                                loops: Animation.Infinite
                                NumberAnimation { to: 0.25; duration: 260 }
                                NumberAnimation { to: 1.0; duration: 260 }
                            }
                        }

                        // Recording into this one.
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            radius: Skin.radius
                            color: "transparent"
                            border.width: Px.px(2)
                            border.color: Skin.arm
                            visible: tile.recording
                        }

                        Column {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.leftMargin: Skin.spacing
                            anchors.rightMargin: Px.px(30)
                            anchors.topMargin: Skin.spacingS + Px.px(3)
                            spacing: Px.px(2)

                            Text {
                                width: parent.width
                                text: tile.modelData.name
                                color: Skin.text
                                font.pixelSize: Skin.fontL
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                text: (tile.bars > 0 ? root.barsText(tile.bars) : qsTr("until changed"))
                                      + " · "
                                      + (tile.modelData.fade > 0 ? qsTr("fade %1").arg(tile.modelData.fade)
                                                                 : qsTr("no fade"))
                                color: Skin.textDim
                                font.pixelSize: Skin.fontXS
                                font.family: Skin.monoFamily
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                text: tile.modelData.count === 0
                                      ? qsTr("empty · record to fill")
                                      : (tile.modelData.count === 1
                                         ? qsTr("1 control")
                                         : qsTr("%1 controls").arg(tile.modelData.count))
                                        + (tile.modelData.lost > 0
                                           ? " · " + qsTr("%1 gone").arg(tile.modelData.lost) : "")
                                color: tile.modelData.lost > 0 ? Skin.mute : Skin.disabled
                                font.pixelSize: Skin.fontXS
                                font.family: Skin.monoFamily
                                elide: Text.ElideRight
                            }
                        }

                        Text {
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.topMargin: Skin.spacingS + Px.px(3)
                            anchors.rightMargin: Skin.spacingS
                            visible: tile.index < 9
                            text: "⌥" + (tile.index + 1)
                            color: Skin.disabled
                            font.pixelSize: Skin.fontXS
                            font.family: Skin.monoFamily
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.leftMargin: Skin.spacing
                            anchors.bottomMargin: Skin.spacingS
                            text: tile.armed ? qsTr("NEXT")
                                : tile.recording ? qsTr("REC")
                                : tile.current ? (Mixer.sceneHold ? qsTr("HOLD") : qsTr("PLAYING"))
                                : ""
                            color: tile.recording && !tile.armed ? Skin.arm : tile.hue
                            font.pixelSize: Skin.fontXS
                            font.bold: true
                            font.letterSpacing: Px.px(1.5)
                        }

                        // The beat inside the bar.
                        Row {
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.rightMargin: Skin.spacingS
                            anchors.bottomMargin: Skin.spacingS + Px.px(3)
                            spacing: Px.px(3)
                            visible: tile.current && Mixer.playing

                            Repeater {
                                model: root.beatsPerBar
                                Rectangle {
                                    required property int index
                                    readonly property bool lit:
                                        index === Math.floor(Mixer.sceneBarPhase * root.beatsPerBar)
                                    width: Px.px(5)
                                    height: width
                                    radius: width / 2
                                    color: lit ? tile.hue : Skin.disabled
                                }
                            }
                        }

                        HoverHandler {
                            id: tileHover
                        }

                        Tip {
                            text: qsTr("%1. Click to play it from the next bar; click again to take that back. Right-click to rename, change its length or fade, or remove it.")
                                  .arg(tile.modelData.name)
                            visible: tileHover.hovered
                        }

                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: mouse => {
                                if (mouse.button === Qt.RightButton)
                                    root.sceneMenuRequested(tile.index, tile)
                                else
                                    Mixer.armScene(tile.index)
                            }
                            onPressAndHold: root.sceneMenuRequested(tile.index, tile)
                        }
                    }
                }

                // A new scene at the end of the song.
                Rectangle {
                    id: addTile
                    width: lane.addWidth
                    height: tiles.height
                    radius: Skin.radius
                    color: addHover.hovered ? Skin.stripAlt : Skin.strip
                    opacity: addHover.hovered ? 1.0 : 0.7
                    border.width: 1
                    border.color: Skin.border

                    Behavior on opacity { NumberAnimation { duration: Skin.fast } }

                    Text {
                        anchors.centerIn: parent
                        text: "+"
                        color: addHover.hovered ? Skin.text : Skin.textDim
                        font.pixelSize: Px.px(24)
                    }

                    HoverHandler {
                        id: addHover
                    }

                    Tip {
                        text: qsTr("Add a scene at the end of the song.")
                        visible: addHover.hovered
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: Mixer.addScene()
                    }
                }
            }
        }
    }
}
