#pragma once

namespace nirbija {

// Plugin editors are X11 windows of their own, carrying the class
// `nirbija-plugin`, and a tiling compositor stretches them to fill a tile: the
// plugin keeps drawing at its own size and the rest of the tile is dead space.
// The fix is a compositor rule that floats them, which until now every user had
// to paste into their config by hand.
//
// This asks the compositor for that rule at startup instead. Hyprland is the
// only one that can be told at runtime, so it is the only one handled; nothing
// happens under any other compositor, or when there is none.
//
// Safe to call before or after the QApplication exists, and safe to call when
// no compositor is listening - it fails quietly. Set NIRBIJA_NO_WM_RULES=1 to
// skip it and manage the rule yourself.
void apply_compositor_rules();

}  // namespace nirbija
