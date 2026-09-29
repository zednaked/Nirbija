// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// A plugin that crashes while the session is restored must not turn the app
// into one that opens and closes forever. The launch that died leaves a marker
// naming the insert it was loading; this plays the launch after it: the insert
// comes back held off with its state, stays held off across saves, and the
// slot menu's "Try again" makes it a live insert again.

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

#include "mixer_model.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

QVariantMap detail(nirbija::MixerModel& mixer, int row, int slot) {
  const QVariantList details =
      mixer.data(mixer.index(row), nirbija::MixerModel::InsertDetailsRole).toList();
  if (slot >= details.size()) return {};
  return details.at(slot).toMap();
}

int saved_quarantines(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return -1;
  const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
  int count = 0;
  for (const QJsonValue& channel : root[QStringLiteral("channels")].toArray())
    for (const QJsonValue& insert :
         channel.toObject()[QStringLiteral("inserts")].toArray())
      if (insert.toObject()[QStringLiteral("quarantined")].toBool()) ++count;
  return count;
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
  const QString session = dir.path() + "/session.json";
  const QString marker = session + ".loading";
  qputenv("NIRBIJA_SESSION", session.toLocal8Bit());

  const std::string uid = "nirbija.drone";
  {
    nirbija::MixerModel mixer;
    mixer.waitForScan();
    if (!mixer.running()) {
      std::printf("no audio server available, skipping\n");
      return 77;
    }
    mixer.addChannel(QStringLiteral("Pad"), 2);
    const int drone =
        mixer.plugins()->rowFor(nirbija::PluginFormat::Internal, uid);
    if (drone < 0 || !mixer.addInsert(0, drone)) {
      fail("could not load the drone");
      return 1;
    }
    mixer.saveSession();
  }

  // What a launch killed inside the drone's load would have left behind.
  {
    QFile left(marker);
    if (!left.open(QIODevice::WriteOnly)) {
      fail("could not write the marker");
      return 1;
    }
    left.write(QByteArray("0/0/") + uid.c_str());
  }

  {
    nirbija::MixerModel mixer;
    mixer.waitForScan();
    const QVariantMap held = detail(mixer, 0, 0);
    if (!held.value(QStringLiteral("missing")).toBool() ||
        !held.value(QStringLiteral("quarantined")).toBool())
      fail("the insert named by the marker was loaded anyway");
    if (held.value(QStringLiteral("name")).toString() != QStringLiteral("Drone"))
      std::printf("note: held insert is named %s\n",
                  qPrintable(held.value(QStringLiteral("name")).toString()));
    if (QFile::exists(marker)) fail("the marker outlived the restore");
    mixer.saveSession();
    if (saved_quarantines(session) != 1) fail("the save forgot the insert is held off");
  }

  // No marker this time: the file alone keeps it off.
  {
    nirbija::MixerModel mixer;
    mixer.waitForScan();
    if (!detail(mixer, 0, 0).value(QStringLiteral("quarantined")).toBool())
      fail("a held-off insert came back on the next launch by itself");

    mixer.retryInsert(0, 0);
    const QVariantMap live = detail(mixer, 0, 0);
    if (live.value(QStringLiteral("missing")).toBool())
      fail("Try again left the insert held off");
    if (mixer.data(mixer.index(0), nirbija::MixerModel::InsertDetailsRole)
            .toList().size() != 1)
      fail("Try again left the ghost behind as well");
    mixer.saveSession();
    if (saved_quarantines(session) != 0) fail("the save still marks it held off");
  }

  if (failures == 0) std::printf("session quarantine: ok\n");
  return failures == 0 ? 0 : 1;
}
