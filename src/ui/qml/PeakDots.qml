import QtQuick

// The three-dot peak indicator AUM puts on every node slot: a compact level
// readout that fits inside a row of text.
Row {
    id: root

    property real level: 0
    property int dots: 3

    spacing: 2

    Repeater {
        model: root.dots

        Rectangle {
            width: 4
            height: 4
            radius: 2

            // Each dot covers a third of the range, so the last one only lights
            // when the signal is genuinely close to clipping.
            readonly property real threshold: (index + 1) / root.dots
            readonly property bool lit: root.level >= threshold - (1 / root.dots)

            color: lit ? Skin.meterColor(threshold) : Skin.line
            opacity: lit ? 1.0 : 0.6
        }
    }
}
