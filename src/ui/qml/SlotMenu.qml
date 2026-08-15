pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import Nirbija

// The menu that opens on an insert slot, a channel title or the session button.
// Its entries are handed in, so one component serves all of them.
//
// The height is the content's height now, not an estimate. It used to be
// `8 + heading * 22 + entries * 31`, three numbers that had to be kept in step
// with the delegate by hand, and a menu that outgrew the guess opened half off
// the bottom of the screen.
Popup {
    id: root

    // [{ label: "...", enabled: true, danger: false, action: function }]
    property var entries: []
    property string heading: ""

    implicitWidth: Skin.px(210)
    padding: Skin.spacingS
    modal: true
    dim: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // Never taller than the window it opens in; past that the list scrolls.
    readonly property int maximumHeight: Overlay.overlay
                                         ? Overlay.overlay.height - Skin.px(16)
                                         : Skin.px(400)

    background: Rectangle {
        color: Skin.popup
        border.width: 1
        border.color: Skin.border
        radius: Skin.radius
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Skin.fast }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Skin.fast }
    }

    function openAt(item, entries, heading) {
        root.entries = entries
        root.heading = heading
        list.currentIndex = -1

        // Reading `height` here settles the binding above against the entries
        // just assigned, so the clamping below works on the real size.
        const size = root.height
        const below = item.mapToItem(Overlay.overlay, 0, item.height + 2)
        const overlayWidth = Overlay.overlay.width
        const overlayHeight = Overlay.overlay.height

        root.x = Math.max(4, Math.min(below.x, overlayWidth - root.width - 4))

        if (below.y + size <= overlayHeight - 4) {
            root.y = below.y
        } else {
            // No room below: open upwards from the item's top edge.
            const above = item.mapToItem(Overlay.overlay, 0, -2)
            root.y = Math.max(4, Math.min(above.y - size, overlayHeight - size - 4))
        }
        root.open()
    }

    contentItem: Column {
        spacing: 1

        Text {
            visible: root.heading.length > 0
            width: parent.width
            height: visible ? implicitHeight + Skin.spacingS : 0
            leftPadding: Skin.spacing
            topPadding: Skin.spacingXS
            text: root.heading
            color: Skin.textDim
            font.pixelSize: Skin.fontS
            elide: Text.ElideRight
        }

        ListView {
            id: list
            width: parent.width
            height: Math.min(contentHeight,
                             root.maximumHeight - (root.heading.length > 0
                                                   ? Skin.px(26) : 0))
            model: root.entries
            interactive: contentHeight > height
            clip: true
            currentIndex: -1
            keyNavigationEnabled: true
            focus: true

            // Arrow keys walk the menu and Return takes the highlighted entry,
            // so a menu opened from a keyboard can be finished from one.
            Keys.onReturnPressed: list.activateCurrent()
            Keys.onEnterPressed: list.activateCurrent()

            function activateCurrent() {
                if (currentIndex < 0 || currentIndex >= root.entries.length) return
                const entry = root.entries[currentIndex]
                if (entry.enabled === false) return
                root.close()
                entry.action()
            }

            delegate: Rectangle {
                id: entry
                required property int index
                required property var modelData

                // Not called `enabled`: Item already has one, and shadowing it
                // makes the whole row stop taking input.
                readonly property bool entryEnabled: modelData.enabled !== false
                readonly property bool current: list.currentIndex === entry.index

                width: list.width
                height: Skin.rowHeight
                radius: Skin.radius
                color: (hover.hovered || current) && entryEnabled
                       ? Skin.slot : "transparent"

                HoverHandler {
                    id: hover
                    onHoveredChanged: if (hovered) list.currentIndex = entry.index
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Skin.spacing
                    anchors.rightMargin: Skin.spacing
                    text: entry.modelData.label
                    color: !entry.entryEnabled ? Skin.disabled
                         : (entry.modelData.danger === true ? Skin.mute : Skin.text)
                    font.pixelSize: Skin.font
                    elide: Text.ElideRight
                }

                TapHandler {
                    enabled: entry.entryEnabled
                    onSingleTapped: {
                        root.close()
                        entry.modelData.action()
                    }
                }
            }
        }
    }
}
