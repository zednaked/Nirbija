pragma ComponentBehavior: Bound

import QtQuick
import Nirbija

// The three-dot peak indicator on every node slot: a compact level readout that
// fits inside a row of text. Fed the position on the fader's travel, already
// computed by the model, rather than an amplitude it would have to convert on
// every frame.
Row {
    id: root

    property real position: 0
    property int dots: 3

    spacing: Skin.spacingXS

    Repeater {
        model: root.dots

        Rectangle {
            id: dot
            required property int index

            // Each dot covers a third of the travel, so the last one only lights
            // when the signal is genuinely close to clipping.
            readonly property real threshold: (index + 1) / root.dots
            readonly property bool lit: index === 0
                ? root.position > 0.02
                : root.position >= threshold - (1 / root.dots)

            width: Skin.px(4)
            height: width
            radius: width / 2
            color: lit ? Skin.meterColor(threshold) : Skin.meterTrack
            opacity: lit ? 1.0 : 0.7
        }
    }
}
