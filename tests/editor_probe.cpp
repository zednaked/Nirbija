// Manual tool: brings the whole mixer up, stands one built-in plugin on a
// fresh strip, opens its editor and writes a picture of the window. For
// looking at an editor without clicking through to it. Not a ctest target -
// it wants a real display, and it runs the audio engine (with the master
// muted, so nothing is heard).
//
//   nirbija_editor_probe nirbija.drone /tmp/drone.png [seconds] [id=value ...]
//
// The trailing pairs set parameters on the insert before the picture is
// taken: `24=1` pushes a drone's swell up so the strings are moving. A
// `drag=x1,y1,x2,y2` argument drags the mouse across the open editor before
// the picture, and `show=24,30` prints those parameters afterwards - the
// way to check that a gesture in the real QML lands where it should.
// `play=1` starts the transport first, for an editor whose state waits on
// the beat grid: a looper armed for the next bar, say. `at=800` delays the
// pairs after it by that many milliseconds, so a press can land mid-bar
// instead of on the transport's own first beat. `editor=0` leaves the
// editor closed, for a picture of the strip behind it. `below=<uid>` stands
// a second built-in plugin in the slot under the first, and `remove_below=800`
// takes it out again that many milliseconds in - the way to see whether an
// editor open over the strip notices the chain under it changing.
// `pack=<file.pack.json>` loads a sampler pack onto the insert, and
// `hit=<pad>` taps that pad shortly before the picture, so a sampler can be
// photographed with a kit on its pads and one of them sounding.

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPointF>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "mixer_model.h"
#include "platform.h"

int main(int argc, char* argv[]) {
  nirbija::force_x11_platform();
  QApplication app(argc, argv);
  // Not the user's own settings or session: the scale here is whatever the
  // environment says, and the session goes wherever NIRBIJA_SESSION points -
  // which the caller had better set to something disposable.
  QCoreApplication::setOrganizationName(QStringLiteral("nirbija-probe"));
  QCoreApplication::setApplicationName(QStringLiteral("editor-probe"));
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  const QStringList args = app.arguments();
  if (args.size() < 3) {
    std::fprintf(stderr, "usage: %s <uid> <out.png> [seconds] [id=value ...]\n",
                 argv[0]);
    return 2;
  }
  const std::string uid = args[1].toStdString();
  const QString out = args[2];
  const int seconds = args.size() > 3 ? args[3].toInt() : 3;

  QQmlApplicationEngine engine;
  engine.loadFromModule("Nirbija", "Main");
  if (engine.rootObjects().isEmpty()) return 1;
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
  if (window == nullptr) {
    std::fprintf(stderr, "Main.qml did not produce a window\n");
    return 1;
  }

  auto* mixer = engine.singletonInstance<nirbija::MixerModel*>("Nirbija", "Mixer");
  if (mixer == nullptr) {
    std::fprintf(stderr, "no Mixer singleton\n");
    return 1;
  }
  if (!mixer->masterMute()) mixer->toggleMasterMute();

  const int row = mixer->rowCount() > 0 ? 0 : mixer->addChannel(QString(), 2);
  const int plugin = mixer->plugins()->rowFor(nirbija::PluginFormat::Internal, uid);
  if (plugin < 0) {
    std::fprintf(stderr, "no internal plugin %s\n", uid.c_str());
    return 1;
  }
  int slot = -1;
  const QVariantList inserts =
      mixer->data(mixer->index(row), nirbija::MixerModel::InsertsRole).toList();
  slot = static_cast<int>(inserts.size());
  if (!mixer->addInsert(row, plugin)) {
    std::fprintf(stderr, "could not add %s\n", uid.c_str());
    return 1;
  }
  QList<QPointF> drag;
  QList<int> show;
  int delay = 0;
  // `editor=0` leaves the editor closed, for a picture of the strip itself:
  // the state dot on the insert slot, say.
  bool openEditor = true;
  for (int i = 4; i < args.size(); ++i) {
    const QStringList pair = args[i].split('=');
    if (pair.size() != 2) continue;
    if (pair[0] == QStringLiteral("drag")) {
      const QStringList xy = pair[1].split(',');
      if (xy.size() == 4)
        drag = {QPointF(xy[0].toDouble(), xy[1].toDouble()),
                QPointF(xy[2].toDouble(), xy[3].toDouble())};
      continue;
    }
    if (pair[0] == QStringLiteral("show")) {
      for (const QString& id : pair[1].split(',')) show.append(id.toInt());
      continue;
    }
    if (pair[0] == QStringLiteral("play")) {
      if ((pair[1].toInt() != 0) != mixer->playing()) mixer->togglePlay();
      continue;
    }
    if (pair[0] == QStringLiteral("at")) {
      delay = pair[1].toInt();
      continue;
    }
    if (pair[0] == QStringLiteral("editor")) {
      openEditor = pair[1].toInt() != 0;
      continue;
    }
    if (pair[0] == QStringLiteral("below")) {
      const int other = mixer->plugins()->rowFor(nirbija::PluginFormat::Internal,
                                                 pair[1].toStdString());
      if (other < 0 || !mixer->addInsertAt(row, other, slot + 1)) {
        std::fprintf(stderr, "could not add %s below\n", qUtf8Printable(pair[1]));
        return 1;
      }
      continue;
    }
    if (pair[0] == QStringLiteral("pack")) {
      if (!mixer->loadSamplerPackFrom(row, slot, QUrl::fromLocalFile(pair[1]))) {
        std::fprintf(stderr, "could not load pack %s\n", qUtf8Printable(pair[1]));
        return 1;
      }
      continue;
    }
    if (pair[0] == QStringLiteral("hit")) {
      const int pad = pair[1].toInt();
      // Late enough that a short one-shot is still sounding when the
      // picture is taken, unless `at=` asked for a moment of its own.
      const int when = delay > 0 ? delay : std::max(0, seconds * 1000 - 150);
      QTimer::singleShot(when, [=] {
        mixer->previewSamplerPad(row, slot, pad, 110);
      });
      continue;
    }
    if (pair[0] == QStringLiteral("remove_below")) {
      QTimer::singleShot(pair[1].toInt(), [=] {
        mixer->removeInsert(row, slot + 1);
        std::printf("removed slot %d\n", slot + 1);
      });
      continue;
    }
    const int id = pair[0].toInt();
    const double value = pair[1].toDouble();
    if (delay <= 0) {
      mixer->setInsertParameter(row, slot, id, value);
    } else {
      QTimer::singleShot(delay, [=] {
        mixer->setInsertParameter(row, slot, id, value);
      });
    }
  }
  std::printf("engine %s, row %d slot %d\n",
              mixer->running() ? "running" : "not running", row, slot);

  // After the window has been shown and laid out: an editor that centres
  // itself over the overlay wants the overlay to have a size first.
  QTimer::singleShot(400, [&] {
    if (!openEditor) return;
    QMetaObject::invokeMethod(window, "openEditor", Q_ARG(QVariant, row),
                              Q_ARG(QVariant, slot));
  });

  // The drag goes in as ordinary mouse events on the window, so it travels
  // the same path a real pointer does: through the overlay, into the popup,
  // onto whichever handler claims it.
  if (drag.size() == 2) {
    QTimer::singleShot(1200, [&] {
      ulong stamp = 1000;
      auto send = [&](QEvent::Type type, QPointF pos, Qt::MouseButton button,
                      Qt::MouseButtons buttons) {
        QMouseEvent event(type, pos, pos, window->mapToGlobal(pos.toPoint()),
                          button, buttons, Qt::NoModifier);
        event.setTimestamp(stamp += 16);
        QCoreApplication::sendEvent(window, &event);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      };
      send(QEvent::MouseButtonPress, drag[0], Qt::LeftButton, Qt::LeftButton);
      for (int step = 1; step <= 12; ++step) {
        const QPointF pos = drag[0] + (drag[1] - drag[0]) * (step / 12.0);
        send(QEvent::MouseMove, pos, Qt::NoButton, Qt::LeftButton);
      }
      send(QEvent::MouseButtonRelease, drag[1], Qt::LeftButton, Qt::NoButton);
      std::printf("dragged (%g,%g) -> (%g,%g)\n", drag[0].x(), drag[0].y(),
                  drag[1].x(), drag[1].y());
    });
  }

  QTimer::singleShot(seconds * 1000, [&] {
    for (const QVariant& entry : mixer->insertParameters(row, slot)) {
      const QVariantMap map = entry.toMap();
      if (show.contains(map[QStringLiteral("id")].toInt()))
        std::printf("param %d %s = %g\n", map[QStringLiteral("id")].toInt(),
                    qUtf8Printable(map[QStringLiteral("name")].toString()),
                    map[QStringLiteral("value")].toDouble());
    }
    const QImage image = window->grabWindow();
    if (!image.save(out))
      std::fprintf(stderr, "could not write %s\n", qUtf8Printable(out));
    else
      std::printf("wrote %s (%dx%d)\n", qUtf8Printable(out), image.width(),
                  image.height());
    app.quit();
  });
  return app.exec();
}
