import QtQuick
import QtQuick.Controls.Basic
import Nirbija

// One effect slot in a strip's insert chain. Empty slots read as a thin outline
// and fill on tap; right-click or a long press opens what can be done to a
// loaded one, matching AUM's gesture for acting on a slot without a menu.
AbstractButton {
    id: root

    property string pluginName: ""
    property bool bypassed: false
    property bool postFader: false
    // A Looper insert's own state, so a live set reads at a glance whether
    // something is armed without opening its editor to find out. Recording
    // is Rec being down; writing is the head actually on the tape - they
    // differ for up to a bar either side of a quantised press.
    property bool looperRecording: false
    property bool looperWriting: false
    property bool looperPlaying: false
    property bool looperHasAudio: false
    property bool samplerRecording: false
    readonly property bool empty: pluginName.length === 0

    signal menuRequested

    implicitHeight: Skin.slotHeight
    padding: Skin.spacingS
    // Clears the bypass mark down the left edge.
    leftPadding: Skin.spacing + Skin.spacingXS
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.role: Accessible.Button
    Accessible.name: root.empty ? qsTr("Empty insert slot") : root.pluginName
    Accessible.onPressAction: root.clicked()

    // An empty slot and a filled one answer different questions: one is "what
    // goes here", the other "what is this and how do I get at it".
    Tip {
        text: root.empty
              ? qsTr("Empty insert slot. Click to load a plugin here.")
              : qsTr("%1 — click to open its editor. Right-click or hold for bypass, reorder and remove.").arg(root.pluginName)
        visible: root.hovered
    }

    background: Rectangle {
        radius: Skin.radius
        color: root.down || root.hovered
               ? Skin.slotHover
               : (root.empty ? Skin.slotEmpty : Skin.slot)
        border.width: 1
        border.color: root.visualFocus ? Skin.focus
                    : root.empty ? Skin.line
                    : Skin.border

        Behavior on color {
            ColorAnimation { duration: Skin.fast }
        }

        // A bypassed insert is still in the chain and still costs the CPU it
        // costs; it just is not heard. Saying so on the slot is cheaper than
        // opening the menu to find out.
        Rectangle {
            visible: !root.empty && root.bypassed
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 1
            width: Px.px(3)
            radius: Skin.radiusS
            color: Skin.mute
        }

        // A Looper's or Sampler's own Rec, at a glance: yellow while a
        // press waits for the bar, red while the head is writing (the thing
        // you must not miss walking into a room full of channels), green
        // while a looper is audibly looping. Quiet when there is nothing
        // to say.
        Rectangle {
            id: stateDot
            // Rec down or the head still writing after a release: both are
            // moments the player has to watch.
            readonly property bool waiting: root.looperRecording !== root.looperWriting
            readonly property bool hot: root.looperWriting || root.samplerRecording
            visible: stateDot.waiting || stateDot.hot ||
                     (root.looperPlaying && root.looperHasAudio)
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 4
            width: Px.px(7)
            height: Px.px(7)
            radius: width / 2
            color: stateDot.waiting ? Skin.solo
                 : stateDot.hot ? Skin.arm : Skin.meterLow
            z: 2

            // Pulses while armed - the one state worth catching out of the
            // corner of an eye. A plain opacity binding stays untouched by
            // this; only `pulse` is a value source, so the dot settles
            // back to a steady 0.85 the moment recording stops.
            property real pulse: 1.0
            opacity: stateDot.waiting || stateDot.hot ? stateDot.pulse : 0.85
            SequentialAnimation on pulse {
                running: stateDot.waiting || stateDot.hot
                loops: Animation.Infinite
                NumberAnimation { from: 1.0; to: 0.35; duration: Skin.fast * 3 }
                NumberAnimation { from: 0.35; to: 1.0; duration: Skin.fast * 3 }
            }
        }
    }

    contentItem: Item {
        Text {
            id: nameText
            anchors.left: parent.left
            anchors.right: tag.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: Skin.spacingXS
            text: root.empty ? "+" : root.pluginName
            color: root.empty ? Skin.textDim
                 : root.bypassed ? Skin.disabled
                 : Skin.text
            font.pixelSize: root.empty ? Skin.fontXL : Skin.font
            horizontalAlignment: root.empty ? Text.AlignHCenter : Text.AlignLeft
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            wrapMode: Text.NoWrap
        }

        // Where the insert sits against the fader. Pre is the common case and
        // says nothing; post is worth a mark, because it is the one that makes
        // an insert follow the fader instead of feeding it.
        Text {
            id: tag
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            visible: !root.empty && root.postFader
            width: visible ? implicitWidth : 0
            text: qsTr("post")
            color: Skin.textDim
            font.pixelSize: Skin.fontXS
        }
    }

    // A MouseArea rather than a TapHandler, and it has to take the button
    // rather than merely watch for it: a handler runs alongside AbstractButton,
    // which answered the right press with `clicked` as well, so the editor
    // opened over the menu that had just been asked for. Accepting only the
    // right button here consumes it before the button sees it, and leaves the
    // left one, hover and the wheel untouched.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: root.menuRequested()
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onLongPressed: root.menuRequested()
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Menu) {
            root.menuRequested()
            event.accepted = true
        }
    }
}
