#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

#include <cstdio>
#include <cstring>

#include "platform.h"

namespace {

// Which backends this binary actually carries. A build with -DNIRBIJA_VST3=OFF
// looks identical from the outside otherwise, and that is exactly the question
// a bug report needs answered first.
const char* compiled_backends() {
  return
#if NIRBIJA_HAVE_LV2
      "LV2 "
#endif
#if NIRBIJA_HAVE_CLAP
      "CLAP "
#endif
#if NIRBIJA_HAVE_VST3
      "VST3 "
#endif
      "";
}

void print_version() {
  std::printf("nirbija %s\nbackends: %s\n", NIRBIJA_VERSION, compiled_backends());
}

void print_help(const char* argv0) {
  print_version();
  std::printf(R"(
usage: %s [options]

  -h, --help       this text
  -v, --version    version and compiled-in plugin backends

environment:
  NIRBIJA_SESSION        session file to open instead of the autosave
  NIRBIJA_UI_SCALE       size of the whole interface, 1.0 is the default
  NIRBIJA_SKIN_COLOR     accent colour, as #rrggbb
  NIRBIJA_ALLOW_WAYLAND  keep the session's platform; loses embedded editors
  NIRBIJA_DEBUG_EMBED    trace plugin editor window embedding
  NIRBIJA_DEBUG_TRANSPORT  trace transport and tempo

Editors are X11 windows, so the app puts itself on xcb/XWayland by default.
Plugin editors carry the window class `nirbija-plugin` so a compositor rule
can float them.
)",
              argv0);
}

}  // namespace

int main(int argc, char* argv[]) {
  // Answered before anything touches a display, so `--version` works over ssh
  // and in a container with no X around.
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--version") == 0) {
      print_version();
      return 0;
    }
    if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
      print_help(argv[0]);
      return 0;
    }
  }

  nirbija::force_x11_platform();

  QGuiApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("Nirbija"));
  app.setOrganizationName(QStringLiteral("Nirbija"));
  app.setApplicationVersion(QStringLiteral(NIRBIJA_VERSION));
  app.setDesktopFileName(QStringLiteral("Nirbija"));
  // Set explicitly rather than left to the .desktop file: on X11 the icon is a
  // property of the window, and an uninstalled build has no .desktop to read.
  app.setWindowIcon(QIcon(QStringLiteral(":/nirbija.png")));

  // Basic draws nothing of its own, which is what a hand-styled mixer wants.
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  // The mixer and the skin register themselves as singletons of the Nirbija
  // module, so there is nothing to push into the root context here: the engine
  // builds them on first use and QML refers to them by type.
  QQmlApplicationEngine engine;
  engine.loadFromModule("Nirbija", "Main");
  if (engine.rootObjects().isEmpty()) return 1;

  return app.exec();
}
