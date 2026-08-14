import QtQuick
import QtQuick.Controls.Basic

// The editor a plugin gets when it ships none of its own: every parameter as a
// horizontal slider. Plain, but it makes UI-less plugins fully usable.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property string pluginName: ""
    property var parameters: []

    width: 420
    height: Math.min(560, 64 + parameters.length * 34)
    modal: true
    anchors.centerIn: Overlay.overlay
    padding: 10

    background: Rectangle {
        color: Skin.strip
        border.width: 1
        border.color: Skin.line
        radius: Skin.radius
    }

    function openFor(row, slot) {
        targetRow = row
        targetSlot = slot
        pluginName = mixer.insertName(row, slot)
        parameters = mixer.insertParameters(row, slot)
        open()
    }

    Column {
        anchors.fill: parent
        spacing: 6

        Text {
            width: parent.width
            text: root.pluginName
            color: Skin.text
            font.pixelSize: 13
            font.bold: true
            elide: Text.ElideRight
        }

        ListView {
            id: list
            width: parent.width
            height: parent.height - 30
            clip: true
            spacing: 4
            model: root.parameters
            boundsBehavior: Flickable.StopAtBounds

            delegate: Item {
                width: list.width
                height: 30

                Text {
                    id: label
                    width: 150
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.name
                    color: Skin.textDim
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }

                Rectangle {
                    id: track
                    anchors.left: label.right
                    anchors.right: readout.left
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    height: 16
                    radius: Skin.radius
                    color: Skin.slotEmpty
                    border.width: 1
                    border.color: Skin.line

                    // The filled part is the value; the whole track is a drag
                    // surface, so there is no tiny handle to hunt for.
                    Rectangle {
                        readonly property real span: modelData.max - modelData.min
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.margins: 1
                        radius: Skin.radius
                        color: Skin.accent
                        opacity: 0.55
                        width: span > 0
                               ? Math.max(0, Math.min(1,
                                     (dragArea.liveValue - modelData.min) / span))
                                 * (parent.width - 2)
                               : 0
                    }

                    MouseArea {
                        id: dragArea
                        anchors.fill: parent

                        // Kept locally while dragging so the bar follows the
                        // finger without re-fetching the whole list.
                        property real liveValue: modelData.value

                        function apply(x) {
                            const span = modelData.max - modelData.min
                            const position = Math.max(0, Math.min(1, x / width))
                            liveValue = modelData.min + position * span
                            mixer.setInsertParameter(root.targetRow,
                                                     root.targetSlot,
                                                     modelData.id, liveValue)
                        }

                        onPressed: mouse => apply(mouse.x)
                        onPositionChanged: mouse => {
                            if (pressed)
                                apply(mouse.x)
                        }
                    }
                }

                Text {
                    id: readout
                    width: 52
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: Math.abs(dragArea.liveValue) >= 100
                          ? dragArea.liveValue.toFixed(0)
                          : dragArea.liveValue.toFixed(2)
                    color: Skin.text
                    font.pixelSize: 10
                    horizontalAlignment: Text.AlignRight
                }
            }
        }
    }
}
