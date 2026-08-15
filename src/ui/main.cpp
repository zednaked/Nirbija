#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include "mixer_model.h"
#include "platform.h"
#include "skin.h"

int main(int argc, char* argv[]) {
  nirbija::force_x11_platform();

  QGuiApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("Nirbija"));
  app.setOrganizationName(QStringLiteral("Nirbija"));

  // Basic draws nothing of its own, which is what a hand-styled mixer wants.
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  nirbija::MixerModel mixer;
  nirbija::Skin skin;

  // Temporary forensics: who ends the event loop, and when.
  QObject::connect(&app, &QGuiApplication::lastWindowClosed,
                   [] { qWarning("forense: lastWindowClosed"); });
  QObject::connect(&app, &QCoreApplication::aboutToQuit,
                   [] { qWarning("forense: aboutToQuit"); });

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(QStringLiteral("mixer"), &mixer);
  engine.rootContext()->setContextProperty(QStringLiteral("Skin"), &skin);
  engine.loadFromModule("Nirbija", "Main");
  if (engine.rootObjects().isEmpty()) return 1;

  return app.exec();
}
