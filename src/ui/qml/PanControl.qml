import QtQuick
import Nirbija

// Balance on a stereo strip, constant-power pan on a mono one — the same
// control, two different laws underneath. Centre is rest, and the fill grows
// out of the centre rather than out of the left edge, so "how far off centre"
// is the thing you see.
Item {
    id: root

    // -1 hard left, 0 centre, +1 hard right.
    property real pan: 0

    signal panRequested(real pan)

    implicitHeight: Px.px(18)
    activeFocusOnTab: true

    readonly property real clamped: Math.max(-1, Math.min(1, pan))
    readonly property string readout: Math.abs(clamped) < 0.02
        ? qsTr("C")
        : (clamped < 0 ? qsTr("L%1") : qsTr("R%1")).arg(Math.round(Math.abs(clamped) * 100))

    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Pan")
    Accessible.description: root.readout

    function applyAt(x) {
        root.panRequested(Math.max(-1, Math.min(1, x / root.width * 2 - 1)))
    }
    function nudge(delta) {
        root.panRequested(Math.max(-1, Math.min(1, root.clamped + delta)))
    }

    Rectangle {
        anchors.fill: parent
        radius: Skin.radius
        color: Skin.slotEmpty
        border.width: 1
        border.color: root.activeFocus ? Skin.focus
                    : hover.hovered ? Skin.border
                    : Skin.line
        clip: true

        // The fill runs from the centre to where the control is set, which is
        // what a pan actually says.
        Rectangle {
            height: parent.height - 2
            anchors.verticalCenter: parent.verticalCenter
            x: root.clamped < 0
               ? parent.width / 2 + root.clamped * (parent.width / 2 - 1)
               : parent.width / 2
            width: Math.abs(root.clamped) * (parent.width / 2 - 1)
            color: Skin.accent
            opacity: 0.35
        }

        // Centre mark, so the resting place is visible when nothing is set.
        Rectangle {
            width: 1
            height: parent.height - Px.px(8)
            anchors.centerIn: parent
            color: Skin.border
        }

        Rectangle {
            width: Px.px(3)
            height: parent.height - Px.px(4)
            radius: Skin.radiusS
            color: Skin.accent
            anchors.verticalCenter: parent.verticalCenter
            x: (parent.width - width) * (root.clamped + 1) * 0.5
        }

        Text {
            anchors.centerIn: parent
            text: root.readout
            color: Skin.textDim
            font.pixelSize: Skin.fontXS
            font.family: Skin.monoFamily
            visible: hover.hovered || root.activeFocus
        }
    }

    HoverHandler {
        id: hover
        cursorShape: Qt.SizeHorCursor
    }

    Tip {
        text: qsTr("Pan. Drag or scroll to move it, double-click to centre.")
        visible: hover.hovered
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onSingleTapped: eventPoint => {
            root.forceActiveFocus(Qt.MouseFocusReason)
            root.applyAt(eventPoint.position.x)
        }
        onDoubleTapped: root.panRequested(0)
    }

    DragHandler {
        target: null
        xAxis.enabled: true
        yAxis.enabled: false
        dragThreshold: 0
        onActiveChanged: if (active) root.forceActiveFocus(Qt.MouseFocusReason)
        onCentroidChanged: if (active) root.applyAt(centroid.position.x)
    }

    WheelHandler {
        acceptedModifiers: Qt.NoModifier
        onWheel: event => root.nudge(event.angleDelta.y > 0 ? 0.05 : -0.05)
    }
    WheelHandler {
        acceptedModifiers: Qt.ShiftModifier
        onWheel: event => root.nudge(event.angleDelta.y > 0 ? 0.01 : -0.01)
    }

    Keys.onPressed: event => {
        const amount = (event.modifiers & Qt.ShiftModifier) ? 0.01 : 0.05
        switch (event.key) {
        case Qt.Key_Left:
            root.nudge(-amount); event.accepted = true; break
        case Qt.Key_Right:
            root.nudge(amount); event.accepted = true; break
        case Qt.Key_Home:
            root.panRequested(0); event.accepted = true; break
        }
    }
}
