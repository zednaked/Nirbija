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

    width: Px.px(460)
    // A plugin with enough parameters to want the 600 px cap can still not
    // fit a window resized down toward its own 520 px minimum height.
    height: Math.min(Px.px(600), Px.px(80) + parameters.length * Px.px(34),
                     Overlay.overlay ? Overlay.overlay.height - Px.px(24)
                                      : Px.px(600))
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
                height: Px.px(26)

                readonly property real span: modelData.max - modelData.min
                // Kept locally while dragging so the bar follows the finger
                // without re-fetching the whole list. Assigning to it below
                // breaks this binding for good, which is fine while the row
                // stays put - but the list reuses delegates, so scrolling can
                // hand this same Item a different row's modelData without
                // its dead binding ever picking up the new value. Explicitly
                // resyncing on that change is what makes a value dragged
                // earlier stop bleeding into whatever parameter scrolls into
                // this slot next.
                property real liveValue: modelData.value
                onModelDataChanged: liveValue = modelData.value

                Text {
                    id: label
                    width: Px.px(150)
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
                    height: Px.px(18)

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
