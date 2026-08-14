// Drives MixerModel the way the QML does — add a channel, move a fader, load an
// insert — without opening a window. Catches the wiring breaking between the UI
// and the engine, which a compile check cannot.

#include <QGuiApplication>

#include <cstdio>
#include <string>

#include "mixer_model.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

QVariant field(nirbija::MixerModel& mixer, int row, int role) {
  return mixer.data(mixer.index(row), role);
}

}  // namespace

int main(int argc, char* argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);

  nirbija::MixerModel mixer;
  if (!mixer.running()) {
    std::printf("no audio server available, skipping\n");
    return 0;
  }

  mixer.addChannel(QStringLiteral("Guitar"), 2);
  if (mixer.rowCount() != 1) {
    fail("addChannel did not add a row");
    return 1;
  }
  if (field(mixer, 0, nirbija::MixerModel::NameRole).toString() != "Guitar")
    fail("channel name did not survive addChannel");

  // The fader is in decibels, so a half-travel position is nowhere near half
  // the gain — this is the round-trip the QML relies on.
  const qreal gain = nirbija::MixerModel::faderToGain(0.5);
  mixer.setGain(0, gain);
  if (field(mixer, 0, nirbija::MixerModel::GainRole).toReal() != gain)
    fail("setGain did not reach the model");
  if (std::abs(nirbija::MixerModel::gainToFader(gain) - 0.5) > 1e-9)
    fail("fader position did not survive the gain round-trip");

  mixer.toggleMute(0);
  if (!field(mixer, 0, nirbija::MixerModel::MutedRole).toBool())
    fail("toggleMute did not reach the model");
  mixer.toggleMute(0);

  if (mixer.plugins()->rowCount() == 0) {
    std::printf("no plugins installed, skipping the insert check\n");
  } else {
    // Pick the first plugin that takes audio in, so the insert is a legitimate
    // one rather than an instrument.
    int chosen = -1;
    for (int i = 0; i < mixer.plugins()->rowCount(); ++i) {
      const nirbija::PluginDescriptor* desc = mixer.plugins()->descriptor(i);
      if (desc != nullptr && desc->audio_inputs >= 2) {
        chosen = i;
        break;
      }
    }
    if (chosen < 0) {
      std::printf("no stereo effect found, skipping the insert check\n");
    } else if (!mixer.addInsert(0, chosen)) {
      fail("addInsert failed for " + mixer.plugins()->descriptor(chosen)->name);
    } else {
      const QStringList inserts =
          field(mixer, 0, nirbija::MixerModel::InsertsRole).toStringList();
      if (inserts.size() != 1) fail("insert did not show up in the model");
      std::printf("  loaded insert: %s\n", inserts.value(0).toUtf8().constData());

      mixer.removeInsert(0, 0);
      const QStringList after =
          field(mixer, 0, nirbija::MixerModel::InsertsRole).toStringList();
      // Removal leaves the slot in place so the surviving indices stay stable.
      if (after.size() != 1 || !after.value(0).isEmpty())
        fail("removeInsert did not clear the slot label");
    }
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
