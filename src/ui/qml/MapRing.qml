// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import Nirbija

// A ring in the corner of anything bound to a knob - the same ring the bars
// draw for themselves. Yellow while the binding is still being learned.
Rectangle {
    id: root

    property bool mapped: false
    property bool waiting: false

    anchors.right: parent.right
    anchors.top: parent.top
    anchors.margins: Skin.spacingXS
    width: Px.px(8)
    height: Px.px(8)
    radius: width / 2
    z: 2
    visible: root.mapped
    color: "transparent"
    border.width: Px.px(2)
    border.color: root.waiting ? Skin.solo : Skin.focus
}
