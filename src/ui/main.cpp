#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include "mixer_model.h"
#include "skin.h"

int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("Nirbija"));
  app.setOrganizationName(QStringLiteral("Nirbija"));

  // Basic draws nothing of its own, which is what a hand-styled mixer wants.
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  nirbija::MixerModel mixer;
  nirbija::Skin skin;

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(QStringLiteral("mixer"), &mixer);
  engine.rootContext()->setContextProperty(QStringLiteral("Skin"), &skin);
  engine.loadFromModule("Nirbija", "Main");
  if (engine.rootObjects().isEmpty()) return 1;

  return app.exec();
}
