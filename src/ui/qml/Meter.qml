import QtQuick
import Nirbija

// A level meter, vertical or lying down.
//
// The colour is a gradient painted once and then uncovered, rather than a
// colour recomputed from the level. The old meter called into C++ twice per bar
// per frame — once to map the level onto the fader's travel and once to pick a
// colour — for every meter on screen, thirty times a second. Here the gradient
// is a constant and the only thing that moves is the rectangle hiding the part
// that is not lit.
//
// `position` and `hold` arrive already mapped onto the fader's decibel travel,
// with the rise and fall shaped on the model side, so nothing here has to
// animate: every frame is already the right length.
Item {
    id: root

    // 0..1 along the fader's travel, not an amplitude.
    property real position: 0
    // The loudest thing seen recently, held for a moment before it falls.
    property real hold: 0
    property bool vertical: true
    property bool showHold: true

    readonly property real clampedPosition: Math.max(0, Math.min(1, position))
    readonly property real clampedHold: Math.max(0, Math.min(1, hold))

    implicitWidth: vertical ? Px.px(7) : Px.px(120)
    implicitHeight: vertical ? Px.px(120) : Px.px(6)

    Rectangle {
        anchors.fill: parent
        radius: Skin.radiusS
        color: Skin.meterTrack
        clip: true

        // The full scale, always painted; the mask below decides how much of it
        // is seen. Stops are doubled at each boundary so the colours change on
        // a line instead of blending through a muddy middle.
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                orientation: root.vertical ? Gradient.Vertical
                                           : Gradient.Horizontal

                // A vertical meter is loud at the top; a horizontal one is loud
                // at the right, so the same stops run the other way.
                GradientStop {
                    position: root.vertical ? 0.0 : 1.0
                    color: Skin.meterHigh
                }
                GradientStop {
                    position: root.vertical ? 1.0 - Skin.meterRedAt : Skin.meterRedAt
                    color: Skin.meterHigh
                }
                GradientStop {
                    position: root.vertical ? 1.0 - Skin.meterRedAt + 0.001
                                            : Skin.meterRedAt - 0.001
                    color: Skin.meterMid
                }
                GradientStop {
                    position: root.vertical ? 1.0 - Skin.meterYellowAt
                                            : Skin.meterYellowAt
                    color: Skin.meterMid
                }
                GradientStop {
                    position: root.vertical ? 1.0 - Skin.meterYellowAt + 0.001
                                            : Skin.meterYellowAt - 0.001
                    color: Skin.meterLow
                }
                GradientStop {
                    position: root.vertical ? 1.0 : 0.0
                    color: Skin.meterLow
                }
            }
        }

        // What is not lit. One rectangle, one number changing per frame.
        Rectangle {
            color: Skin.meterTrack
            anchors.top: parent.top
            anchors.left: root.vertical ? parent.left : undefined
            anchors.right: root.vertical ? parent.right : undefined
            anchors.bottom: root.vertical ? undefined : parent.bottom
            width: root.vertical ? parent.width
                                 : parent.width * (1.0 - root.clampedPosition)
            height: root.vertical ? parent.height * (1.0 - root.clampedPosition)
                                  : parent.height
            // Anchored to the quiet end, which is the top going up and the
            // right going across.
            x: root.vertical ? 0 : parent.width * root.clampedPosition
        }

        // The held peak: a hairline where the loudest recent moment was, which
        // is the only way to read a transient off a meter that has already
        // fallen back.
        Rectangle {
            visible: root.showHold && root.clampedHold > 0.005
            color: Skin.text
            opacity: 0.85
            width: root.vertical ? parent.width : Math.max(1, Px.px(1.5))
            height: root.vertical ? Math.max(1, Px.px(1.5)) : parent.height
            y: root.vertical
               ? Math.min(parent.height - height,
                          parent.height * (1.0 - root.clampedHold))
               : 0
            x: root.vertical
               ? 0
               : Math.min(parent.width - width,
                          parent.width * root.clampedHold)
        }
    }
}
