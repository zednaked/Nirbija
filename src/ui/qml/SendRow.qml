import QtQuick
import Nirbija

// One send: the bus it feeds and how much of the strip goes there. The bar is
// the level, so the amount is visible without a number — and the number is
// there anyway, because "about a third" is not a mix decision.
ValueTrack {
    id: root

    property string busName: ""
    property real level: 0

    signal levelRequested(real level)

    value: root.level
    label: "→ " + root.busName
    valueText: Math.round(Math.max(0, Math.min(1, root.level)) * 100) + "%"
    // Nothing scrolls under a send, so it can answer the first pixel of a drag.
    pressThreshold: 0

    tip: qsTr("Send to %1, at %2%. A copy of this strip's output goes there while the strip still feeds its own destination. Drag or scroll to set the amount; right-click to remove.")
             .arg(root.busName)
             .arg(Math.round(Math.max(0, Math.min(1, root.level)) * 100))

    onMoved: value => root.levelRequested(value)
}
