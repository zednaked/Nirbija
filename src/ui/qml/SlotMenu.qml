import QtQuick
import QtQuick.Controls.Basic

// The menu that opens on an insert slot or a channel title. Its entries are
// handed in, so one component serves both.
Popup {
    id: root

    // [{ label: "...", enabled: true, danger: false, action: function }]
    property var entries: []
    property string heading: ""

    width: 190
    padding: 4
    modal: true
    dim: false

    background: Rectangle {
        color: Skin.strip
        border.width: 1
        border.color: Skin.line
        radius: Skin.radius
    }

    function openAt(item, entries, heading) {
        root.entries = entries
        root.heading = heading

        // The height is computed rather than read: implicitHeight is not
        // settled the instant the entries change, and clamping against a stale
        // value is how a menu ends up half off the screen.
        const estimated = 8 + (heading.length > 0 ? 22 : 0) + entries.length * 31
        const below = item.mapToItem(Overlay.overlay, 0, item.height + 2)
        const overlayWidth = Overlay.overlay.width
        const overlayHeight = Overlay.overlay.height

        root.x = Math.max(4, Math.min(below.x, overlayWidth - root.width - 4))

        if (below.y + estimated <= overlayHeight - 4) {
            root.y = below.y
        } else {
            // No room below: open upwards from the item's top edge.
            const above = item.mapToItem(Overlay.overlay, 0, -2)
            root.y = Math.max(4, above.y - estimated)
        }
        root.open()
    }

    Column {
        width: parent.width
        spacing: 1

        Text {
            visible: root.heading.length > 0
            width: parent.width
            leftPadding: 8
            topPadding: 4
            bottomPadding: 4
            text: root.heading
            color: Skin.textDim
            font.pixelSize: 10
            elide: Text.ElideRight
        }

        Repeater {
            model: root.entries

            Rectangle {
                width: parent.width
                height: 30
                radius: Skin.radius
                color: hover.hovered && entryEnabled ? Skin.slot : "transparent"

                // Not called `enabled`: Item already has one, and shadowing it
                // makes the whole row stop taking input.
                readonly property bool entryEnabled: modelData.enabled !== false

                HoverHandler {
                    id: hover
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    text: modelData.label
                    color: !parent.entryEnabled ? Skin.line
                                           : (modelData.danger === true ? Skin.mute
                                                                        : Skin.text)
                    font.pixelSize: 12
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: parent.entryEnabled
                    onClicked: {
                        root.close()
                        modelData.action()
                    }
                }
            }
        }
    }
}
