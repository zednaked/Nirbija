// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import Nirbija

// A tape's trim handle: full-height, dragged to wherever the audio should
// start or stop, with a tab at each end that is easier to find than a
// hairline. Shared by the looper's loop and the sampler's pad; the owner
// hears where it went through moved() and writes it to its plugin.
Item {
    id: handle

    required property bool isStart
    // Where the handle sits, in the tape's pixels.
    property int atX: 0
    // The tape's width, which the drag is measured against.
    property real span: 1
    // The other edge, so the two can never cross.
    property real start: 0
    property real end: 1
    property string tip: ""

    // The new pair of edges, as fractions of the tape.
    signal moved(real start, real end)

    readonly property bool lit: dragHover.hovered || drag.active

    x: handle.atX - width / 2
    width: Px.px(14)

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Px.px(4)
        radius: Skin.radiusS
        color: handle.lit ? Skin.focus : Skin.accent
    }
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        width: parent.width
        height: Px.px(10)
        radius: Skin.radiusS
        color: handle.lit ? Skin.focus : Skin.accent
    }
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        width: parent.width
        height: Px.px(10)
        radius: Skin.radiusS
        color: handle.lit ? Skin.focus : Skin.accent
    }

    HoverHandler { id: dragHover; cursorShape: Qt.SizeHorCursor }
    Tip {
        text: handle.tip
        visible: handle.tip.length > 0 && dragHover.hovered && !drag.active
    }

    DragHandler {
        id: drag
        target: null
        // Both axes, or a slightly vertical drag is a better match for the
        // fader sitting under this popup and steals the grab mid-trim.
        xAxis.enabled: true
        yAxis.enabled: true
        // No Approves bit: once this handler has the grab nobody may take it.
        grabPermissions: PointerHandler.CanTakeOverFromAnything
        onCentroidChanged: if (drag.active) {
            const fraction = Math.max(0, Math.min(1,
                (handle.x + handle.width / 2 + drag.centroid.position.x
                 - drag.centroid.pressPosition.x) / Math.max(1, handle.span)))
            if (handle.isStart)
                handle.moved(Math.min(fraction, handle.end - 0.02), handle.end)
            else
                handle.moved(handle.start, Math.max(fraction, handle.start + 0.02))
        }
    }
}
