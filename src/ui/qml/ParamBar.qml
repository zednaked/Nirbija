import QtQuick
import Nirbija

// A vertical amount, filled from the bottom, touched where you want it.
// The drone's swell and weather are these; so are the looper's feedback,
// gain, pitch, speed and tone. One control, one look, in every editor.
//
// `bipolar` fills from the middle instead, for a value that has a rest at
// its centre - a pitch, a speed - so zero reads as nothing rather than as
// half. `absolute` off makes a drag move from where the value already was,
// for a control where a tap that jumps would do damage: overdub feedback
// slammed to 40% in the middle of a take is the take gone.
Item {
    id: bar
    property string label: ""
    property real value: 0
    property string readout: ""
    property color hue: Skin.accent
    property bool mapped: false
    property bool waiting: false
    property bool mapping: false
    property string tip: ""
    // Where the value is actually sounding, when it lags the control:
    // -1 draws nothing.
    property real marker: -1
    property bool big: false
    property bool bipolar: false
    property bool absolute: true
    // How much one notch of the wheel is worth, and with Shift held.
    property real step: 0.03
    property real fineStep: 0.005
    signal edited(real value)
    signal armed()
    property real pulse: 0.35

    readonly property real clamped: Math.max(0, Math.min(1, bar.value))

    implicitWidth: Px.px(56)
    implicitHeight: Px.px(64)

    SequentialAnimation on pulse {
        running: bar.waiting
        loops: Animation.Infinite
        NumberAnimation { from: 0.25; to: 0.85; duration: 480; easing.type: Easing.InOutSine }
        NumberAnimation { from: 0.85; to: 0.25; duration: 480; easing.type: Easing.InOutSine }
    }

    Rectangle {
        anchors.fill: face
        anchors.margins: -Px.px(4)
        radius: face.radius + Px.px(4)
        color: "transparent"
        border.width: Px.px(4)
        border.color: bar.hue
        opacity: bar.waiting ? bar.pulse
               : (bar.big && !bar.bipolar && bar.value > 0.02 ? 0.22 * bar.value : 0)
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: Skin.medium } }
    }

    Rectangle {
        id: face
        anchors.fill: parent
        radius: Skin.radius
        color: Skin.slotEmpty
        clip: true
        border.width: bar.waiting ? Px.px(2) : 1
        border.color: bar.waiting ? Skin.solo
                    : bar.mapping ? Skin.focus
                    : bar.activeFocus ? Skin.focus
                    : Qt.rgba(bar.hue.r, bar.hue.g, bar.hue.b, 0.45)

        // The fill: from the bottom, or from the middle out.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            y: bar.bipolar
               ? parent.height * (1 - Math.max(0.5, bar.clamped))
               : parent.height - bar.clamped * parent.height
            height: bar.bipolar
                    ? Math.abs(bar.clamped - 0.5) * parent.height
                    : bar.clamped * parent.height
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.lighter(bar.hue, 1.3) }
                GradientStop { position: 1.0; color: Qt.darker(bar.hue, 1.15) }
            }
            opacity: 0.85
        }
        // The rest line of a bipolar bar: where nothing is.
        Rectangle {
            visible: bar.bipolar
            anchors.left: parent.left
            anchors.right: parent.right
            y: parent.height / 2 - height / 2
            height: 1
            color: Qt.rgba(bar.hue.r, bar.hue.g, bar.hue.b, 0.5)
        }
        Rectangle {
            visible: bar.bipolar ? Math.abs(bar.clamped - 0.5) > 0.005 : bar.value > 0.01
            anchors.left: parent.left
            anchors.right: parent.right
            y: parent.height - bar.clamped * parent.height
            height: Px.px(2)
            color: Qt.lighter(bar.hue, 1.6)
        }
        // The marker: a thin light line where the sound actually is.
        Rectangle {
            visible: bar.marker >= 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Px.px(6)
            anchors.rightMargin: Px.px(6)
            y: parent.height - Math.max(0, Math.min(1, bar.marker)) * parent.height - height / 2
            height: Px.px(2)
            color: Skin.text
            opacity: 0.8
        }

        Rectangle {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: Skin.spacingXS
            radius: Skin.radiusS
            color: Qt.rgba(0, 0, 0, 0.4)
            width: labelText.implicitWidth + Skin.spacingS * 2
            height: labelText.implicitHeight + Skin.spacingXS * 1.5
            Text {
                id: labelText
                anchors.centerIn: parent
                text: bar.label
                color: Skin.text
                font.pixelSize: bar.big ? Skin.fontS : Skin.fontXS
                font.bold: true
                font.letterSpacing: Px.px(1)
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Skin.spacingXS
            text: bar.readout
            color: Skin.text
            font.pixelSize: bar.big ? Skin.fontL : Skin.fontXS
            font.bold: true
            font.family: Skin.monoFamily
            style: Text.Outline
            styleColor: Qt.rgba(0, 0, 0, 0.55)
        }

        Rectangle {
            visible: bar.mapped
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Skin.spacingXS
            width: Px.px(8)
            height: Px.px(8)
            radius: width / 2
            color: "transparent"
            border.width: Px.px(2)
            border.color: bar.waiting ? Skin.solo : bar.hue
        }
    }

    function valueAt(y) {
        return 1 - Math.max(0, Math.min(1, y / Math.max(1, bar.height)))
    }
    function nudge(delta) {
        bar.edited(Math.max(0, Math.min(1, bar.value + delta)))
    }

    HoverHandler {
        id: barHover
        cursorShape: Qt.SizeVerCursor
    }
    Tip {
        text: bar.tip
        visible: bar.tip.length > 0 && barHover.hovered
    }

    activeFocusOnTab: true
    Accessible.role: Accessible.Slider
    Accessible.name: bar.label
    Accessible.description: bar.tip

    TapHandler {
        enabled: bar.mapping
        acceptedButtons: Qt.LeftButton
        onTapped: bar.armed()
    }
    DragHandler {
        enabled: !bar.mapping
        target: null
        dragThreshold: 0
        grabPermissions: PointerHandler.CanTakeOverFromAnything
                         | PointerHandler.ApprovesTakeOverByNothing
        property real startValue: 0
        onActiveChanged: if (active) {
            bar.forceActiveFocus(Qt.MouseFocusReason)
            startValue = bar.value
            if (bar.absolute) bar.edited(bar.valueAt(centroid.position.y))
        }
        onCentroidChanged: if (active) {
            if (bar.absolute) {
                bar.edited(bar.valueAt(centroid.position.y))
            } else {
                const travelled = (centroid.pressPosition.y - centroid.position.y)
                                  / Math.max(1, bar.height)
                bar.edited(Math.max(0, Math.min(1, startValue + travelled)))
            }
        }
    }
    WheelHandler {
        acceptedModifiers: Qt.NoModifier
        onWheel: event => bar.nudge(event.angleDelta.y > 0 ? bar.step : -bar.step)
    }
    WheelHandler {
        acceptedModifiers: Qt.ShiftModifier
        onWheel: event => bar.nudge(event.angleDelta.y > 0 ? bar.fineStep : -bar.fineStep)
    }
    Keys.onPressed: event => {
        // An arrow is a bigger notch than the wheel: 0.05 and 0.01 at the
        // default steps, scaled with them when a caller sets its own.
        const fine = (event.modifiers & Qt.ShiftModifier) !== 0
        const amount = fine ? bar.fineStep * 2 : bar.step * 5 / 3
        switch (event.key) {
        case Qt.Key_Up: bar.nudge(amount); event.accepted = true; break
        case Qt.Key_Down: bar.nudge(-amount); event.accepted = true; break
        case Qt.Key_Home: bar.edited(1); event.accepted = true; break
        case Qt.Key_End: bar.edited(0); event.accepted = true; break
        }
    }
}
