import QtQuick

// One octave of notes for the selected strip. Velocity is fixed; this is a
// "does it make sound" keyboard, not a piano.
Rectangle {
    id: root

    property int targetRow: 0
    property int octave: 4

    width: 280
    height: 72
    color: Skin.strip
    border.width: 1
    border.color: Skin.line
    radius: Skin.radius

    readonly property var whites: [0, 2, 4, 5, 7, 9, 11]
    readonly property var blacks: [1, 3, -1, 6, 8, 10]

    function noteOn(semitone) {
        mixer.sendNote(targetRow, octave * 12 + semitone, 100)
    }
    function noteOff(semitone) {
        mixer.sendNote(targetRow, octave * 12 + semitone, 0)
    }

    Row {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 2

        Repeater {
            model: 7
            Rectangle {
                width: 28
                height: parent.height
                radius: 2
                color: whiteArea.pressed ? Skin.accent : Skin.slot
                Text {
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: ["C", "D", "E", "F", "G", "A", "B"][index]
                    color: Skin.textDim
                    font.pixelSize: 9
                }
                MouseArea {
                    id: whiteArea
                    anchors.fill: parent
                    onPressed: root.noteOn(root.whites[index])
                    onReleased: root.noteOff(root.whites[index])
                    onCanceled: root.noteOff(root.whites[index])
                }
            }
        }

        Column {
            width: 36
            spacing: 2
            StripButton {
                width: parent.width
                height: 22
                label: "oct-"
                onClicked: root.octave = Math.max(1, root.octave - 1)
            }
            StripButton {
                width: parent.width
                height: 22
                label: "oct+"
                onClicked: root.octave = Math.min(7, root.octave + 1)
            }
        }
    }
}
