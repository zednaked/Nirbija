import QtQuick
import QtQuick.Controls.Basic
import Nirbija

// Renaming a channel. Small on purpose: it is a text field and two ways out.
Popup {
    id: root

    property int targetRow: -1

    signal accepted(string name)

    width: Px.px(320)
    modal: true
    anchors.centerIn: Overlay.overlay
    padding: Skin.spacingL
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Skin.popup
        border.width: 1
        border.color: Skin.border
        radius: Skin.radiusL
    }

    function openFor(row, current) {
        root.targetRow = row
        field.text = current
        root.open()
        field.forceActiveFocus()
        field.selectAll()
    }

    function commit() {
        if (field.text.trim().length > 0)
            root.accepted(field.text.trim())
        root.close()
    }

    contentItem: Column {
        spacing: Skin.spacing

        Text {
            width: parent.width
            text: qsTr("Channel name")
            color: Skin.textDim
            font.pixelSize: Skin.font
        }

        TextField {
            id: field
            width: parent.width
            color: Skin.text
            font.pixelSize: Skin.fontL
            selectByMouse: true

            background: Rectangle {
                color: Skin.slotEmpty
                radius: Skin.radius
                border.width: 1
                border.color: field.activeFocus ? Skin.focus : Skin.border
            }

            // Enter commits and Escape backs out, so the mouse is optional.
            Keys.onReturnPressed: root.commit()
            Keys.onEnterPressed: root.commit()
            Keys.onEscapePressed: root.close()
        }

        Row {
            anchors.right: parent.right
            spacing: Skin.spacingS

            StripButton {
                width: Px.px(76)
                label: qsTr("Cancel")
                onClicked: root.close()
            }

            StripButton {
                width: Px.px(76)
                label: qsTr("Rename")
                active: field.text.trim().length > 0
                enabled: field.text.trim().length > 0
                onClicked: root.commit()
            }
        }
    }
}
