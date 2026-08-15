pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nirbija

// The editor a plugin gets when it ships none of its own: every parameter as a
// horizontal track. Plain, but it makes UI-less plugins fully usable — and with
// the wheel and the arrow keys on each row, usable precisely.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property string pluginName: ""
    property var parameters: []

    width: Skin.px(460)
    height: Math.min(Skin.px(600), Skin.px(80) + parameters.length * Skin.px(34))
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

    function openFor(row, slot) {
        root.targetRow = row
        root.targetSlot = slot
        root.pluginName = Mixer.insertName(row, slot)
        root.parameters = Mixer.insertParameters(row, slot)
        root.open()
    }

    contentItem: ColumnLayout {
        spacing: Skin.spacingS

        Text {
            Layout.fillWidth: true
            text: root.pluginName
            color: Skin.text
            font.pixelSize: Skin.fontL
            font.bold: true
            elide: Text.ElideRight
        }

        Text {
            Layout.fillWidth: true
            visible: root.parameters.length > 0
            text: qsTr("Drag or scroll a row; Shift for fine. Right-click a row to bind it to a controller.")
            color: Skin.textDim
            font.pixelSize: Skin.fontS
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            visible: root.parameters.length === 0
            text: qsTr("This plugin exposes no parameters to the host.")
            color: Skin.textDim
            font.pixelSize: Skin.font
            wrapMode: Text.WordWrap
        }

        RowLayout {
            visible: Mixer.insertIsLooper(root.targetRow, root.targetSlot)
            spacing: Skin.spacingS

            StripButton {
                Layout.preferredWidth: Skin.px(76)
                label: qsTr("Rec")
                activeColor: Skin.arm
                tip: qsTr("Arm the loop for recording; it starts at the next cycle.")
                onClicked: Mixer.setLooperRecord(root.targetRow, root.targetSlot, true)
            }
            StripButton {
                Layout.preferredWidth: Skin.px(76)
                label: qsTr("Play")
                activeColor: Skin.meterLow
                tip: qsTr("Play the recorded loop.")
                onClicked: Mixer.setLooperPlay(root.targetRow, root.targetSlot, true)
            }
            StripButton {
                Layout.preferredWidth: Skin.px(76)
                label: qsTr("Clear")
                danger: true
                tip: qsTr("Throw the loop away.")
                onClicked: Mixer.clearLooper(root.targetRow, root.targetSlot)
            }

            Item {
                Layout.fillWidth: true
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Skin.spacingS
            model: root.parameters
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true

            ScrollBar.vertical: ScrollBar {
                policy: list.contentHeight > list.height ? ScrollBar.AsNeeded
                                                         : ScrollBar.AlwaysOff
            }

            delegate: Item {
                id: parameter
                required property var modelData

                width: list.width
                height: Skin.px(26)

                readonly property real span: modelData.max - modelData.min
                // Kept locally while dragging so the bar follows the finger
                // without re-fetching the whole list.
                property real liveValue: modelData.value

                Text {
                    id: label
                    width: Skin.px(150)
                    anchors.verticalCenter: parent.verticalCenter
                    text: parameter.modelData.name
                    color: Skin.textDim
                    font.pixelSize: Skin.font
                    elide: Text.ElideRight
                }

                ValueTrack {
                    anchors.left: label.right
                    anchors.right: parent.right
                    anchors.leftMargin: Skin.spacing
                    anchors.rightMargin: Skin.spacingL
                    anchors.verticalCenter: parent.verticalCenter
                    height: Skin.px(18)

                    // The rows scroll, so a drag has to prove itself before it
                    // wins the gesture from the list.
                    pressThreshold: -1
                    value: parameter.span > 0
                           ? (parameter.liveValue - parameter.modelData.min)
                             / parameter.span
                           : 0
                    valueText: Math.abs(parameter.liveValue) >= 100
                               ? parameter.liveValue.toFixed(0)
                               : parameter.liveValue.toFixed(2)
                    tip: qsTr("%1, from %2 to %3.")
                             .arg(parameter.modelData.name)
                             .arg(parameter.modelData.min)
                             .arg(parameter.modelData.max)

                    onMoved: value => {
                        parameter.liveValue = parameter.modelData.min
                                              + value * parameter.span
                        Mixer.setInsertParameter(root.targetRow, root.targetSlot,
                                                 parameter.modelData.id,
                                                 parameter.liveValue)
                    }

                    // Right-click binds a hardware control to this parameter
                    // instead of moving it.
                    onMenuRequested: Mixer.learnInsertParam(
                        root.targetRow, root.targetSlot, parameter.modelData.id,
                        parameter.modelData.min, parameter.modelData.max)
                }
            }
        }
    }
}
