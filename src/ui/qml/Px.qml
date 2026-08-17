pragma Singleton
import QtQuick
import Nirbija

// A reactive twin of Skin::px(). That one is a plain C++ invokable method:
// calling it from a binding never establishes a dependency on `scale`: the
// engine only tracks property reads that JavaScript performs directly, and a
// C++ method reading another C++ getter does not go through that path. Every
// binding that called Px.px(N) computed it once, at whatever scale was
// current when the item was created, and then sat frozen through every zoom
// afterwards — a button sized at 100% keeping its old width while the label
// inside it grew with the rest of the interface, eliding into "MU…".
//
// This is JavaScript, so `Skin.scale` read here is captured like any other
// property read inside a binding, and every Px.px(N) call re-evaluates when
// the scale changes.
QtObject {
    function px(value) {
        return Math.round(value * Skin.scale)
    }
}
