import QtQuick
import Nirbija

// A horizontal bar that is its own handle: the fill is the value and the whole
// width is the grab area, so there is no small knob to hunt for. Sends and
// plugin parameters are both this control.
//
// Built on pointer handlers rather than a MouseArea because a MouseArea has to
// choose between taking every press — which stops the list underneath from ever
// scrolling — and using `preventStealing`, which was the old answer. Handlers
// negotiate: whichever of the drag and the flick crosses its threshold first
// gets the gesture.
Item {
    id: root

    // Always 0..1. A caller with real units maps in and out.
    property real value: 0
    property string label: ""
    property string valueText: ""
    property color fillColor: Skin.accent
    property real fillOpacity: 0.45
    property string tip: ""
    // How far the pointer must travel before a drag starts. Zero is instant,
    // which suits a control with nothing scrollable underneath it.
    property int pressThreshold: -1
    // How much one notch of the wheel or one arrow key is worth.
    property real step: 0.05
    property real fineStep: 0.01

    signal moved(real value)
    signal menuRequested

    implicitHeight: Skin.px(20)
    activeFocusOnTab: true

    Accessible.role: Accessible.Slider
    Accessible.name: root.label
    Accessible.description: Math.round(root.value * 100) + "%. " + root.tip

    function applyAt(x) {
        root.moved(Math.max(0, Math.min(1, x / root.width)))
    }
    function nudge(delta) {
        root.moved(Math.max(0, Math.min(1, root.value + delta)))
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

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 1
            width: Math.max(0, Math.min(1, root.value)) * (parent.width - 2)
            radius: Skin.radiusS
            color: root.fillColor
            opacity: root.fillOpacity
        }

        Text {
            anchors.left: parent.left
            anchors.right: readout.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: Skin.spacingS + 1
            anchors.rightMargin: Skin.spacingS
            text: root.label
            color: Skin.text
            font.pixelSize: Skin.fontS
            elide: Text.ElideRight
        }

        Text {
            id: readout
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: Skin.spacingS + 1
            visible: root.valueText.length > 0
            width: visible ? implicitWidth : 0
            text: root.valueText
            color: Skin.textDim
            font.pixelSize: Skin.fontS
            font.family: Skin.monoFamily
        }
    }

    HoverHandler {
        id: hover
        cursorShape: Qt.SizeHorCursor
    }

    Tip {
        text: root.tip
        visible: root.tip.length > 0 && hover.hovered
    }

    // A tap lands the value where it was tapped; a drag keeps it under the
    // finger.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onSingleTapped: eventPoint => {
            root.forceActiveFocus(Qt.MouseFocusReason)
            root.applyAt(eventPoint.position.x)
        }
        onLongPressed: root.menuRequested()
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onSingleTapped: root.menuRequested()
    }

    DragHandler {
        target: null
        xAxis.enabled: true
        yAxis.enabled: false
        dragThreshold: root.pressThreshold
        onActiveChanged: if (active) root.forceActiveFocus(Qt.MouseFocusReason)
        onCentroidChanged: if (active) root.applyAt(centroid.position.x)
    }

    // The wheel is what a mouse has instead of a fine drag, and Shift makes it
    // finer still.
    WheelHandler {
        acceptedModifiers: Qt.NoModifier
        onWheel: event => root.nudge(event.angleDelta.y > 0 ? root.step : -root.step)
    }
    WheelHandler {
        acceptedModifiers: Qt.ShiftModifier
        onWheel: event => root.nudge(event.angleDelta.y > 0 ? root.fineStep
                                                            : -root.fineStep)
    }

    Keys.onPressed: event => {
        const amount = (event.modifiers & Qt.ShiftModifier) ? root.fineStep
                                                            : root.step
        switch (event.key) {
        case Qt.Key_Left:
        case Qt.Key_Down:
            root.nudge(-amount); event.accepted = true; break
        case Qt.Key_Right:
        case Qt.Key_Up:
            root.nudge(amount); event.accepted = true; break
        case Qt.Key_Home:
            root.moved(0); event.accepted = true; break
        case Qt.Key_End:
            root.moved(1); event.accepted = true; break
        case Qt.Key_Menu:
            root.menuRequested(); event.accepted = true; break
        }
    }
}
