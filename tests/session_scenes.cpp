// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// Scenes through the model: recorded by touching the strips, kept by the
// session across a restart, brought back by undo, and played by the audio
// thread when one is armed with the transport stopped.

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QThread>

#include <cmath>
#include <cstdio>
#include <string>

#include "mixer_model.h"

namespace {

using nirbija::MixerModel;

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

QVariant field(MixerModel& mixer, int row, int role) {
  return mixer.data(mixer.index(row), role);
}

QVariantMap scene(MixerModel& mixer, int index) {
  const QVariantList list = mixer.scenes();
  return index < list.size() ? list[index].toMap() : QVariantMap{};
}

int plugin_row(MixerModel& mixer, const char* uid) {
  for (int i = 0; i < mixer.plugins()->rowCount(); ++i) {
    const nirbija::PluginDescriptor* descriptor = mixer.plugins()->descriptor(i);
    if (descriptor != nullptr && descriptor->uid == uid) return i;
  }
  return -1;
}

// Lets the audio thread take the commands and the poll see what it did.
void settle(int milliseconds = 300) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < milliseconds) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(10);
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);

  QTemporaryDir dir;
  if (!dir.isValid()) {
    fail("could not make a temporary directory");
    return 1;
  }
  qputenv("NIRBIJA_SESSION", (dir.path() + "/session.json").toLocal8Bit());

  {
    MixerModel mixer;
    mixer.waitForScan();
    if (!mixer.running()) {
      std::printf("no audio server available, skipping\n");
      return 77;
    }

    mixer.addChannel(QStringLiteral("Beat"), 2);
    mixer.addChannel(QStringLiteral("Pad"), 2);
    mixer.addChannel(QStringLiteral("Drone"), 2);
    const int seq = plugin_row(mixer, "nirbija.stepseq");
    if (seq < 0 || !mixer.addInsert(0, seq)) fail("could not load a Step Sequencer");
    const int fx = plugin_row(mixer, "nirbija.fxpad");
    if (fx < 0 || !mixer.addInsert(1, fx)) fail("could not load an FX Pad");

    // Recording with no scene makes one, and each touch lands in it once.
    mixer.setSceneRecording(true);
    if (mixer.scenes().size() != 1) fail("recording did not make a first scene");
    mixer.setGain(0, 0.5);
    mixer.setGain(0, 0.4);  // the same fader again: still one control
    mixer.toggleSceneOn(1);
    mixer.setInsertParameter(0, 0, 197, 3.0);  // queue pattern 3 (index 2)
    mixer.setFxPadAmount(1, 0, 7, 0.3);        // the filter pad, then again:
    mixer.setFxPadAmount(1, 0, 7, 0.6);        // one control, the last value
    mixer.setInsertParameter(0, 0, 1, 0.5);    // a sequencer knob: not a scene's
    mixer.setFollowScenes(2, false);
    mixer.setGain(2, 0.1);  // ignores scenes: not recorded
    mixer.setSceneRecording(false);
    mixer.setGain(1, 0.9);  // not recording: not recorded

    if (scene(mixer, 0)["count"].toInt() != 4)
      fail("the first scene holds " + std::to_string(scene(mixer, 0)["count"].toInt()) +
           " controls, not 4");
    // Marks follow the scene in view; with nothing playing and not
    // recording there is none, so nothing is marked.
    if (mixer.currentScene() < 0 && field(mixer, 0, MixerModel::SceneMarksRole).toInt() != 0)
      fail("a strip is marked with no scene in view");

    mixer.renameScene(0, QStringLiteral("Intro"));
    const int second = mixer.addScene();
    mixer.setSceneBars(second, 0);
    mixer.setSceneFade(second, 4);
    mixer.setSceneAuto(false);
    mixer.saveSession();
  }

  {
    MixerModel mixer;
    // A strip is drawn when it is made, before the session says it ignores
    // scenes or is switched off; the model has to say so afterwards.
    bool follow_announced = false;
    QObject::connect(&mixer, &QAbstractItemModel::dataChanged,
                     [&](const QModelIndex& top, const QModelIndex& bottom,
                         const QList<int>& roles) {
                       if (roles.contains(MixerModel::FollowScenesRole) &&
                           top.row() <= 2 && bottom.row() >= 2)
                         follow_announced = true;
                     });
    mixer.waitForScan();
    if (!follow_announced)
      fail("a restored strip's follow switch was never announced to the view");

    if (mixer.scenes().size() != 2) {
      fail("the session came back with " + std::to_string(mixer.scenes().size()) +
           " scenes, not 2");
    } else {
      if (scene(mixer, 0)["name"].toString() != QStringLiteral("Intro"))
        fail("a scene's name was not kept");
      if (scene(mixer, 0)["count"].toInt() != 4) fail("a scene lost what it holds");
      if (scene(mixer, 0)["lost"].toInt() != 0)
        fail("a scene's controls no longer find their strips: " +
             std::to_string(scene(mixer, 0)["lost"].toInt()));
      if (scene(mixer, 1)["bars"].toInt() != 0 || scene(mixer, 1)["fade"].toInt() != 4)
        fail("a scene's length or fade was not kept");
    }
    if (mixer.sceneAuto()) fail("the queue switch was not kept");
    if (field(mixer, 2, MixerModel::FollowScenesRole).toBool())
      fail("a strip that ignores scenes follows them again");
    if (field(mixer, 1, MixerModel::SceneOnRole).toBool())
      fail("a strip switched off came back on");

    // Undo brings a removed scene back whole.
    mixer.removeScene(0);
    if (mixer.scenes().size() != 1) fail("remove did not remove the scene");
    mixer.undo();
    if (mixer.scenes().size() != 2 || scene(mixer, 0)["count"].toInt() != 4)
      fail("undo did not bring the scene back with its controls");

    // Stopped: an armed scene lands at once, and the strips follow it.
    mixer.setGain(0, 1.0);
    mixer.toggleSceneOn(1);  // on again, by hand
    mixer.setFxPadAmount(1, 0, 7, 0.0);
    mixer.armScene(0);
    settle();
    if (std::abs(mixer.fxPadAmount(1, 0, 7) - 0.6) > 1e-4)
      fail("the FX Pad knob did not follow the scene: " +
           std::to_string(mixer.fxPadAmount(1, 0, 7)));
    if (mixer.currentScene() != 0) fail("an armed scene did not start with the transport stopped");
    if (std::abs(field(mixer, 0, MixerModel::GainRole).toDouble() - 0.4) > 1e-4)
      fail("the fader did not follow the scene: " +
           std::to_string(field(mixer, 0, MixerModel::GainRole).toDouble()));
    if (field(mixer, 1, MixerModel::SceneOnRole).toBool())
      fail("the strip the scene switches off is still on");
    if ((field(mixer, 0, MixerModel::SceneMarksRole).toInt() & 2) == 0)
      fail("the playing scene's fader is not marked on its strip");

    // A hand takes the fader; the next scene clears it.
    mixer.setGain(0, 0.7);
    if ((field(mixer, 0, MixerModel::SceneHandsRole).toInt() & 2) == 0)
      fail("taking a fader the scene holds did not mark the hand");
    mixer.armScene(1);
    settle();
    if (field(mixer, 0, MixerModel::SceneHandsRole).toInt() != 0)
      fail("a new scene did not clear the hand");
  }

  if (failures == 0) std::puts("session_scenes: ok");
  return failures == 0 ? 0 : 1;
}
