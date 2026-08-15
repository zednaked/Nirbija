pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import Nirbija

// The list that opens when an empty insert slot is tapped.
//
// The search is done by a proxy model rather than by hiding delegates. Giving
// every non-matching row `visible: false` still built a delegate for each of
// the hundreds of installed plugins and re-laid them out on every keystroke.
Popup {
    id: root

    property int targetRow: -1
    property int targetSlot: -1
    property bool replace: false

    width: Skin.px(420)
    height: Skin.px(480)
    modal: true
    anchors.centerIn: Overlay.overlay
    padding: 0
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

    onOpened: {
        search.text = ""
        list.currentIndex = 0
        search.forceActiveFocus()
    }

    PluginFilterModel {
        id: filter
        sourceModel: Mixer.plugins
        query: search.text
    }

    function choose(proxyRow) {
        const pluginIndex = filter.sourceRow(proxyRow)
        if (pluginIndex < 0) return
        if (root.replace && root.targetSlot >= 0)
            Mixer.removeInsert(root.targetRow, root.targetSlot)
        if (root.targetSlot >= 0)
            Mixer.addInsertAt(root.targetRow, pluginIndex, root.targetSlot)
        else
            Mixer.addInsert(root.targetRow, pluginIndex)
        root.replace = false
        root.close()
    }

    Column {
        anchors.fill: parent
        anchors.margins: Skin.spacing
        spacing: Skin.spacing

        TextField {
            id: search
            width: parent.width
            placeholderText: qsTr("Search %1 plugins by name, maker or format")
                                 .arg(Mixer.plugins.count)
            color: Skin.text
            placeholderTextColor: Skin.disabled
            font.pixelSize: Skin.fontL
            selectByMouse: true

            background: Rectangle {
                color: Skin.slotEmpty
                radius: Skin.radius
                border.width: 1
                border.color: search.activeFocus ? Skin.focus : Skin.border
            }

            // Down walks into the list without taking the hands off the search,
            // and Return takes whatever is highlighted.
            Keys.onDownPressed: {
                list.forceActiveFocus()
                list.currentIndex = Math.max(0, list.currentIndex)
            }
            Keys.onReturnPressed: root.choose(list.currentIndex)
            Keys.onEnterPressed: root.choose(list.currentIndex)
        }

        Text {
            width: parent.width
            visible: filter.count === 0
            text: Mixer.plugins.count === 0
                  ? qsTr("No plugins found. The session menu can rescan.")
                  : qsTr("Nothing matches “%1”.").arg(search.text)
            color: Skin.textDim
            font.pixelSize: Skin.font
            wrapMode: Text.WordWrap
        }

        ListView {
            id: list
            width: parent.width
            height: parent.height - search.height - Skin.spacing
            clip: true
            model: filter
            currentIndex: 0
            keyNavigationEnabled: true
            reuseItems: true
            boundsBehavior: Flickable.StopAtBounds

            Keys.onReturnPressed: root.choose(list.currentIndex)
            Keys.onEnterPressed: root.choose(list.currentIndex)

            ScrollBar.vertical: ScrollBar {
                policy: list.contentHeight > list.height ? ScrollBar.AsNeeded
                                                         : ScrollBar.AlwaysOff
            }

            delegate: Rectangle {
                id: entry
                required property int index
                required property string name
                required property string vendor
                required property string format

                width: list.width - (list.ScrollBar.vertical.visible
                                     ? Skin.spacingL : 0)
                height: Skin.px(40)
                radius: Skin.radius
                color: list.currentIndex === entry.index ? Skin.slot
                     : hover.hovered ? Skin.stripAlt
                     : "transparent"

                HoverHandler {
                    id: hover
                    onHoveredChanged: if (hovered) list.currentIndex = entry.index
                }

                Column {
                    anchors.left: parent.left
                    anchors.right: formatBadge.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Skin.spacing
                    anchors.rightMargin: Skin.spacing
                    spacing: 1

                    Text {
                        width: parent.width
                        text: entry.name
                        color: Skin.text
                        font.pixelSize: Skin.font
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        text: entry.vendor.length > 0 ? entry.vendor
                                                      : qsTr("unknown vendor")
                        color: Skin.textDim
                        font.pixelSize: Skin.fontS
                        elide: Text.ElideRight
                    }
                }

                // The format is what tells two copies of the same plugin apart,
                // so it is a badge rather than a word in the margin.
                Rectangle {
                    id: formatBadge
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: Skin.spacing
                    width: formatText.implicitWidth + Skin.spacing
                    height: Skin.px(16)
                    radius: Skin.radiusS
                    color: Skin.slotEmpty
                    border.width: 1
                    border.color: Skin.border

                    Text {
                        id: formatText
                        anchors.centerIn: parent
                        text: entry.format
                        color: Skin.textDim
                        font.pixelSize: Skin.fontXS
                    }
                }

                TapHandler {
                    onSingleTapped: root.choose(entry.index)
                }
            }
        }
    }
}
