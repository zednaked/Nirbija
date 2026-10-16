// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// A step sequencer above the FM-1 plays one of its tracks (design/fm1.md): the
// lanes' MIDI channel says which, the header names it ("FM-1 · T2"), and
// picking DRUMS puts every lane on channel 10 tuned to the kit's pieces, with
// the rows named after them. One sequencer per track, stacked above the FM-1.

// The core first: Qt defines `emit` as a macro and MidiOutBlock has a method
// of that name.
#include "core/fm1.h"
#include "core/step_sequencer.h"

#include <QGuiApplication>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

#include "mixer_model.h"

namespace {

int failures = 0;

void expect(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

int plugin(nirbija::MixerModel& mixer, const char* uid) {
  for (int i = 0; i < mixer.plugins()->rowCount(); ++i) {
    const nirbija::PluginDescriptor* d = mixer.plugins()->descriptor(i);
    if (d != nullptr && d->uid == uid) return i;
  }
  return -1;
}

}  // namespace

int main(int argc, char* argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  QTemporaryDir dir;
  qputenv("NIRBIJA_SESSION", (dir.path() + "/session.json").toLocal8Bit());

  nirbija::MixerModel mixer;
  mixer.waitForScan();
  if (!mixer.running()) {
    std::printf("no audio server available, skipping\n");
    return 77;
  }
  // A real FM-1 would be attached by the strip and talked to: not in a test,
  // unless asked for (NIRBIJA_TEST_WITH_FM1=1, a few seconds of pings).
  if (!mixer.engineForTests().sources_matching("Jangada", true).empty() &&
      qgetenv("NIRBIJA_TEST_WITH_FM1") != "1") {
    std::printf("a real FM-1 is plugged in; skipping\n");
    return 77;
  }

  mixer.addChannel(QStringLiteral("FM-1"), 2);
  const bool placed = mixer.addInsert(0, plugin(mixer, "nirbija.stepseq")) &&
                      mixer.addInsert(0, plugin(mixer, "nirbija.fm1"));
  expect(placed, "a step sequencer above the FM-1");

  QVariantMap target = mixer.insertSequencerTarget(0, 0);
  expect(target.value("fm1").toBool() && target.value("fm1Track").toInt() == 0 &&
             target.value("name").toString() == QStringLiteral("FM-1 · T1"),
         "fresh lanes are on channel 1: it plays track 1, and says so");

  mixer.setSequencerFm1Track(0, 0, 3);
  auto* seq = dynamic_cast<nirbija::StepSequencerInstance*>(
      mixer.engineForTests().graph().channel(0).insert_at(0));
  bool lanes = seq != nullptr;
  for (int lane = 0; lanes && lane < 8; ++lane)
    lanes = seq->lane_channel(lane) == 9 && seq->lane_note(lane) == nirbija::Fm1Instance::kDrumLaneNotes[lane];
  target = mixer.insertSequencerTarget(0, 0);
  const QVariantList pads = target.value("pads").toList();
  bool kick = false;
  for (const QVariant& pad : pads)
    if (pad.toMap().value("note").toInt() == 36 && pad.toMap().value("name").toString() == QStringLiteral("Kick"))
      kick = true;
  expect(lanes && target.value("fm1Track").toInt() == 3 &&
             target.value("name").toString() == QStringLiteral("FM-1 · DRUMS") && kick,
         "DRUMS: every lane on channel 10, kick snare hats clap tom crash ride, the rows named after the kit");

  mixer.setSequencerFm1Track(0, 0, 1);
  target = mixer.insertSequencerTarget(0, 0);
  expect(seq->lane_channel(4) == 1 && seq->lane_note(0) == 36 && target.value("fm1Track").toInt() == 1 &&
             target.value("pads").toList().isEmpty(),
         "track 2: the lanes on channel 2, their notes kept, no drum names");

  seq->set_lane(5, 60, 16, 2, 0, 4, false, 0.5);  // one lane elsewhere
  expect(mixer.insertSequencerTarget(0, 0).value("fm1Track").toInt() == -1 &&
             mixer.insertSequencerTarget(0, 0).value("name").toString() == QStringLiteral("FM-1"),
         "lanes on mixed channels: just FM-1");

  std::printf("%s\n", failures ? "FAILED" : "all ok");
  return failures ? 1 : 0;
}
