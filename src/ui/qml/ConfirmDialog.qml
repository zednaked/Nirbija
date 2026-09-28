// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls.Basic
import Nirbija

// One question, two ways out. For the few actions that throw work away -
// starting a new session over one that has strips in it - where a menu entry
// firing on a slipped click is not something Ctrl+Z should have to catch.
Popup {
    id: root

    property string title: ""
    property string message: ""
    property string acceptLabel: qsTr("OK")
    // The thing to do when the person says yes.
    property var onAccept: null

    signal accepted

    width: Px.px(360)
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

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Skin.fast }
    }

    function ask(heading, text, label, action) {
        root.title = heading
        root.message = text
        root.acceptLabel = label
        root.onAccept = action
        root.open()
        yes.forceActiveFocus()
    }

    function commit() {
        root.close()
        if (root.onAccept) root.onAccept()
        root.accepted()
    }

    contentItem: Column {
        spacing: Skin.spacing

        Text {
            width: parent.width
            text: root.title
            color: Skin.text
            font.pixelSize: Skin.fontL
            font.bold: true
            wrapMode: Text.WordWrap
        }

        Text {
            width: parent.width
            text: root.message
            color: Skin.textDim
            font.pixelSize: Skin.font
            wrapMode: Text.WordWrap
        }

        Row {
            anchors.right: parent.right
            spacing: Skin.spacingS

            StripButton {
                width: Px.px(90)
                label: qsTr("Cancel")
                onClicked: root.close()
            }

            StripButton {
                id: yes
                width: Px.px(120)
                label: root.acceptLabel
                danger: true
                onClicked: root.commit()
                Keys.onReturnPressed: root.commit()
                Keys.onEnterPressed: root.commit()
            }
        }
    }
}
