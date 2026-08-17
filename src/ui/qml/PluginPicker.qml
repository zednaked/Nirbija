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

    width: Px.px(420)
    height: Px.px(480)
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
        filter.kind = PluginFilterModel.AnyKind
        list.currentIndex = 0
        search.forceActiveFocus()
    }

    PluginFilterModel {
        id: filter
        sourceModel: Mixer.plugins
        query: search.text
        onKindChanged: list.currentIndex = 0
    }

    // The buckets, in the order someone reaches for them. A machine with two
    // hundred plugins installed is unreadable as one list, and the question
    // people arrive with is "a synth" or "a reverb", not a plugin's name.
    readonly property var kinds: [
        { label: qsTr("all"),         value: PluginFilterModel.AnyKind },
        { label: qsTr("instruments"), value: PluginFilterModel.Instrument },
        { label: qsTr("effects"),     value: PluginFilterModel.Effect },
        { label: qsTr("midi"),        value: PluginFilterModel.MidiEffect },
        { label: qsTr("analysers"),   value: PluginFilterModel.Analyzer },
        { label: qsTr("utility"),     value: PluginFilterModel.Utility }
    ]

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
            placeholderText: qsTr("Search %1 plugins by name, maker, kind or format")
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

        Row {
            id: kindRow
            width: parent.width
            spacing: Skin.spacingS

            Repeater {
                model: root.kinds

                Rectangle {
                    id: chip
                    required property var modelData
                    readonly property bool picked: filter.kind === chip.modelData.value

                    height: Px.px(22)
                    width: chipText.implicitWidth + Skin.spacingL
                    radius: Skin.radiusS
                    color: chip.picked ? Skin.accent
                         : chipHover.hovered ? Skin.slotHover
                         : Skin.slotEmpty
                    border.width: 1
                    border.color: chip.picked ? Skin.accent : Skin.border

                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        text: chip.modelData.label
                        color: chip.picked ? Skin.onAccent : Skin.textDim
                        font.pixelSize: Skin.fontS
                    }

                    HoverHandler { id: chipHover }
                    TapHandler {
                        onSingleTapped: filter.kind = chip.modelData.value
                    }
                }
            }
        }

        Text {
            width: parent.width
            visible: filter.count === 0
            text: Mixer.plugins.count === 0
                  ? qsTr("No plugins found. The session menu can rescan.")
                  : search.text.length > 0
                    ? qsTr("Nothing matches “%1” here.").arg(search.text)
                    : qsTr("Nothing of that kind is installed.")
            color: Skin.textDim
            font.pixelSize: Skin.font
            wrapMode: Text.WordWrap
        }

        ListView {
            id: list
            width: parent.width
            height: parent.height - search.height - kindRow.height
                    - 2 * Skin.spacing
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
                required property string category

                width: list.width - (list.ScrollBar.vertical.visible
                                     ? Skin.spacingL : 0)
                height: Px.px(40)
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

                    // Maker and what the plugin calls itself, on one line: with
                    // fifty Calf entries the maker alone tells you nothing, and
                    // "Reverb" beside it is the whole answer.
                    Text {
                        width: parent.width
                        text: {
                            const who = entry.vendor.length > 0
                                        ? entry.vendor : qsTr("unknown vendor")
                            return entry.category.length > 0
                                   ? who + " · " + entry.category : who
                        }
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
                    height: Px.px(16)
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
