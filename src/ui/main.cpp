#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

#include "platform.h"

int main(int argc, char* argv[]) {
  nirbija::force_x11_platform();

  QGuiApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("Nirbija"));
  app.setOrganizationName(QStringLiteral("Nirbija"));
  app.setDesktopFileName(QStringLiteral("Nirbija"));

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
