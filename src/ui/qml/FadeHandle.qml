// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import Nirbija

// A tape's fade handle: a small mark that only moves between its own trim
// edge and the trim window's midpoint. Shared by the looper's loop and the
// sampler's pad; the owner hears the new fade through moved().
Item {
    id: fadeHandle

    required property bool isIn
    // Where the handle sits, in the tape's pixels.
    property int atX: 0
    // The trim edges in pixels, and half the window between them: a fade
    // fraction of 1.0 walks its handle exactly to the midpoint.
    property real trimStartX: 0
    property real trimEndX: 1
    readonly property real half: Math.max(1, (fadeHandle.trimEndX - fadeHandle.trimStartX) / 2)
    property string tip: ""

    // The new fade, as a fraction of half the trim window.
    signal moved(real fraction)

    readonly property bool lit: fadeHover.hovered || fadeDrag.active

    x: fadeHandle.atX - width / 2
    width: Px.px(22)
    height: Px.px(22)

    Rectangle {
        anchors.centerIn: parent
        width: Px.px(12)
        height: Px.px(12)
        radius: width / 2
        color: fadeHandle.lit ? Skin.focus : Skin.solo
        Rectangle {
            anchors.centerIn: parent
            width: parent.width * 1.8
            height: width
            radius: width / 2
            color: "transparent"
            border.width: Px.px(2)
            border.color: fadeHandle.lit ? Skin.focus : Skin.solo
            opacity: 0.45
        }
    }

    HoverHandler { id: fadeHover; cursorShape: Qt.SizeHorCursor }
    Tip {
        text: fadeHandle.tip
        visible: fadeHandle.tip.length > 0 && fadeHover.hovered && !fadeDrag.active
    }

    DragHandler {
        id: fadeDrag
        target: null
        xAxis.enabled: true
        yAxis.enabled: true
        // No Approves bit: once this handler has the grab nobody may take it.
        grabPermissions: PointerHandler.CanTakeOverFromAnything
        onCentroidChanged: if (fadeDrag.active) {
            const x = fadeHandle.x + fadeHandle.width / 2
                      + fadeDrag.centroid.position.x
                      - fadeDrag.centroid.pressPosition.x
            if (fadeHandle.isIn)
                fadeHandle.moved(Math.max(0, Math.min(1, (x - fadeHandle.trimStartX) / fadeHandle.half)))
            else
                fadeHandle.moved(Math.max(0, Math.min(1, (fadeHandle.trimEndX - x) / fadeHandle.half)))
        }
    }
}
