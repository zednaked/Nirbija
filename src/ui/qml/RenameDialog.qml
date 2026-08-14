import QtQuick
import QtQuick.Controls.Basic

// Renaming a channel. Small on purpose: it is a text field and two ways out.
Popup {
    id: root

    property int targetRow: -1

    signal accepted(string name)

    width: 300
    height: 96
    modal: true
    anchors.centerIn: Overlay.overlay
    padding: 10

    background: Rectangle {
        color: Skin.strip
        border.width: 1
        border.color: Skin.line
        radius: Skin.radius
    }

    function openFor(row, current) {
        root.targetRow = row
        field.text = current
        root.open()
        field.forceActiveFocus()
        field.selectAll()
    }

    Column {
        anchors.fill: parent
        spacing: 8

        Text {
            text: qsTr("Channel name")
            color: Skin.textDim
            font.pixelSize: 11
        }

        TextField {
            id: field
            width: parent.width
            color: Skin.text
            font.pixelSize: 13

            background: Rectangle {
                color: Skin.slotEmpty
                radius: Skin.radius
                border.width: 1
                border.color: Skin.line
            }

            // Enter commits and Escape backs out, so the mouse is optional.
            Keys.onReturnPressed: root.commit()
            Keys.onEnterPressed: root.commit()
            Keys.onEscapePressed: root.close()
        }
    }

    function commit() {
        if (field.text.length > 0)
            root.accepted(field.text)
        root.close()
    }
}
