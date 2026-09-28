// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// Undo and redo used to rebuild the whole session from disk: the graph
// parked, every plugin re-instantiated, a hole in the master for "put that
// channel back". Now the mixer is brought to the snapshot by difference: a
// strip the snapshot also has keeps its very same plugin objects, nothing is
// parked, and only what actually differs is made or removed.
//
// Needs an audio server for the graph to render at all; skips without one,
// the same as the other session tests.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
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

void pump(int milliseconds) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < milliseconds) QCoreApplication::processEvents();
}

QStringList chain(nirbija::MixerModel& mixer, int row) {
  return mixer.data(mixer.index(row), nirbija::MixerModel::InsertsRole)
      .toStringList();
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

  nirbija::MixerModel mixer;
  mixer.waitForScan();
  if (!mixer.running()) {
    std::printf("no audio server available, skipping\n");
    return 77;  // CTest marks it Skipped rather than Passed
  }
  nirbija::AudioGraph& graph = mixer.engineForTests().graph();

  const int sequencer = mixer.plugins()->rowFor(nirbija::PluginFormat::Internal,
                                                "nirbija.stepseq");
  const int drone = mixer.plugins()->rowFor(nirbija::PluginFormat::Internal,
                                            "nirbija.drone");
  if (sequencer < 0 || drone < 0) {
    fail("the built-in sequencer or drone is missing from the picker");
    return 1;
  }

  // A strip that is playing something: the one an undo must not touch.
  mixer.addChannel(QStringLiteral("Keep"), 2);
  mixer.addInsert(0, sequencer);
  mixer.addInsert(0, drone);
  mixer.setGain(0, 0.7);
  mixer.setSequencerCell(0, 0, 0, 0, 0, 60, 100, true, 1.0, false, false);
  nirbija::PluginInstance* seq_before = graph.channel(0).insert_at(0);
  nirbija::PluginInstance* drone_before = graph.channel(0).insert_at(1);
  if (seq_before == nullptr || drone_before == nullptr) {
    fail("the inserts did not land");
    return 1;
  }
  if (!mixer.playing()) mixer.togglePlay();
  pump(200);

  // --- add a channel, take it back, put it back again ----------------------
  const uint64_t quiet_before = graph.quiet_generation();
  const uint64_t rendered_before = graph.render_generation();
  mixer.addChannel(QStringLiteral("Extra"), 2);
  mixer.addInsert(1, drone);
  if (mixer.rowCount() != 2) fail("the second channel was not added");
  if (!mixer.canUndo()) fail("adding a channel did not become an undo step");

  mixer.undo();
  pump(100);
  if (mixer.rowCount() != 1)
    fail("undo did not take the added channel away, rows: " +
         std::to_string(mixer.rowCount()));
  if (graph.channel(0).insert_at(0) != seq_before)
    fail("undo re-instantiated the sequencer on the untouched strip");
  if (graph.channel(0).insert_at(1) != drone_before)
    fail("undo re-instantiated the drone on the untouched strip");
  if (!mixer.canRedo()) fail("undo did not leave a redo step");

  mixer.redo();
  pump(100);
  if (mixer.rowCount() != 2) fail("redo did not bring the channel back");
  if (mixer.data(mixer.index(1), nirbija::MixerModel::NameRole).toString() !=
      QStringLiteral("Extra"))
    fail("redo brought a channel back under another name");
  if (chain(mixer, 1) != QStringList{QStringLiteral("Drone")})
    fail("redo brought the channel back without its insert");
  if (graph.channel(0).insert_at(0) != seq_before ||
      graph.channel(0).insert_at(1) != drone_before)
    fail("redo re-instantiated a plugin on the untouched strip");

  // The pattern edit made before the add is not an undo step and must not
  // have snapped back with the channel.
  const QVariantMap snap = mixer.insertSequencerSnapshot(0, 0);
  if (snap.value(QStringLiteral("on")).toList().value(0).toInt() != 1)
    fail("undo/redo reverted a sequencer edit that was never an undo step");

  // --- remove an insert and take that back ---------------------------------
  mixer.pushUndoForTests();
  mixer.removeInsert(0, 1);
  if (graph.channel(0).insert_at(1) != nullptr) fail("removeInsert left the drone");
  mixer.undo();
  pump(100);
  if (chain(mixer, 0) != QStringList{QStringLiteral("Step Sequencer"), QStringLiteral("Drone")} &&
      chain(mixer, 0).size() != 2)
    fail("undo did not put the removed insert back: " +
         chain(mixer, 0).join(", ").toStdString());
  if (graph.channel(0).insert_at(0) != seq_before)
    fail("putting one insert back re-instantiated its neighbour");
  if (graph.channel(0).insert_at(1) == nullptr)
    fail("the removed insert did not come back");

  // Through all of it the graph kept rendering and never went quiet.
  if (graph.render_generation() == rendered_before)
    fail("the graph did not render at all");
  if (graph.quiet_generation() != quiet_before)
    fail("undo or redo parked the graph");
  if (graph.parked()) fail("the graph was left parked");

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
