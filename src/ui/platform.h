#pragma once

#include <QByteArray>
#include <QDebug>

namespace nirbija {

// Plugin editors on Linux are X11 windows, and embedding one means handing the
// plugin an X11 window id. A native Wayland surface has none: the id QWindow
// returns is a pointer, and the plugin's first X call fails with BadWindow. So
// the host asks for xcb and runs under XWayland.
//
// Must be called before QGuiApplication is constructed. Set
// NIRBIJA_ALLOW_WAYLAND=1 to keep the session's own choice and lose embedded
// editors.
inline void force_x11_platform() {
  if (!qEnvironmentVariableIsEmpty("NIRBIJA_ALLOW_WAYLAND")) return;
  if (qEnvironmentVariableIsEmpty("DISPLAY")) return;  // no XWayland to fall back on

  const QByteArray current = qgetenv("QT_QPA_PLATFORM");
  if (current == "xcb") return;
  if (!current.isEmpty() && !current.contains("wayland")) return;  // deliberate choice

  qputenv("QT_QPA_PLATFORM", "xcb");
}

}  // namespace nirbija
