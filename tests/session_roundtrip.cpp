// Saves a session, throws the mixer away, and builds a new one to check it comes
// back the same. This is what closing and reopening the app does.

#include <QGuiApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdio>
#include <string>
#include <vector>

#include "mixer_model.h"
#include "core/looper.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

QVariant field(nirbija::MixerModel& mixer, int row, int role) {
  return mixer.data(mixer.index(row), role);
}

// The first stereo effect installed here, so the test exercises a real plugin's
// own state rather than a stand-in.
int pick_effect(nirbija::MixerModel& mixer) {
  for (int i = 0; i < mixer.plugins()->rowCount(); ++i) {
    const nirbija::PluginDescriptor* descriptor = mixer.plugins()->descriptor(i);
    if (descriptor != nullptr && descriptor->audio_inputs >= 2) return i;
  }
  return -1;
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

  QString effect_name;
  {
    nirbija::MixerModel mixer;
    if (!mixer.running()) {
      std::printf("no audio server available, skipping\n");
      return 0;
    }

    mixer.addChannel(QStringLiteral("Bass"), 2);
    mixer.addChannel(QStringLiteral("Drums"), 2);
    mixer.setGain(0, 0.25);
    mixer.setPan(1, -0.5);
    mixer.toggleMute(1);

    const int effect = pick_effect(mixer);
    if (effect >= 0) {
      effect_name = mixer.plugins()->descriptor(effect)->name.c_str();
      if (!mixer.addInsert(0, effect)) fail("could not load an insert");
    }

    mixer.saveSession();
  }

  if (!QFile::exists(dir.path() + "/session.json")) {
    fail("no session file was written");
    return 1;
  }

  // A fresh model loads the session in its constructor, the same as a restart.
  nirbija::MixerModel restored;
  if (restored.rowCount() != 2) {
    fail("expected 2 channels back, got " + std::to_string(restored.rowCount()));
    return 1;
  }

  if (field(restored, 0, nirbija::MixerModel::NameRole).toString() != "Bass")
    fail("channel name did not survive the round trip");
  if (std::abs(field(restored, 0, nirbija::MixerModel::GainRole).toReal() - 0.25) > 1e-9)
    fail("fader did not survive the round trip");
  if (std::abs(field(restored, 1, nirbija::MixerModel::PanRole).toReal() + 0.5) > 1e-9)
    fail("pan did not survive the round trip");
  if (!field(restored, 1, nirbija::MixerModel::MutedRole).toBool())
    fail("mute did not survive the round trip");

  if (!effect_name.isEmpty()) {
    const QStringList inserts =
        field(restored, 0, nirbija::MixerModel::InsertsRole).toStringList();
    if (inserts.size() != 1 || inserts.value(0) != effect_name)
      fail("the insert did not come back");
    else
      std::printf("  insert restored: %s\n", effect_name.toUtf8().constData());
  }

  // The looper's tape is megabytes of floats inside the insert blob. Saving
  // the mixer and constructing a new one is what closing the app does, and
  // that path used to come back with a silent looper.
  {
    qputenv("NIRBIJA_SESSION", (dir.path() + "/looper.json").toLocal8Bit());
    const int looper_row =
        restored.plugins()->rowFor(nirbija::PluginFormat::Internal,
                                   "nirbija.looper");
    if (looper_row < 0) {
      fail("the built-in looper is not in the plugin list");
    } else {
      nirbija::MixerModel with_loop;
      if (!with_loop.running()) {
        fail("audio server went away before the looper round trip");
      } else {
        with_loop.addChannel(QStringLiteral("Loop"), 2);
        if (!with_loop.addInsert(0, looper_row)) {
          fail("could not add a looper");
        } else {
          with_loop.engineForTests().park_graph();
          nirbija::LooperInstance* looper = dynamic_cast<nirbija::LooperInstance*>(
              with_loop.engineForTests().graph().channel(0).insert_at(0));
          if (looper == nullptr) {
            fail("the insert was not a looper");
          } else {
            looper->set_parameter(3, 0.0);
            looper->set_parameter(0, 1.0);
            std::vector<float> in_l(256, 0.6f), in_r(256, 0.6f);
            std::vector<float> out_l(256), out_r(256);
            const float* ins[2] = {in_l.data(), in_r.data()};
            float* outs[2] = {out_l.data(), out_r.data()};
            nirbija::TransportInfo transport;
            transport.playing = true;
            looper->set_transport(transport);
            for (int i = 0; i < 8; ++i) looper->process(ins, outs, 256);
            looper->set_parameter(0, 0.0);
            looper->process(ins, outs, 256);
            if (!looper->loop_closed())
              fail("the take did not close before the session save");
          }
          with_loop.engineForTests().unpark_graph();
          with_loop.saveSession();
        }
      }
    }

    nirbija::MixerModel looped;
    if (!looped.looperLoopClosed(0, 0))
      fail("the looper came back without a closed loop");
    else if (!looped.looperHasAudio(0, 0))
      fail("the looper came back silent");
    else {
      bool heard = false;
      for (const QVariant& peak : looped.looperWaveform(0, 0, 8))
        heard |= peak.toFloat() > 0.1f;
      if (!heard) fail("the restored looper waveform was empty");
    }
    qputenv("NIRBIJA_SESSION", (dir.path() + "/session.json").toLocal8Bit());
  }

  // Loading a file into a mixer that already has channels walks the removal
  // path first; restoring a bus insert's state through the channel list used
  // to dereference the removed channel's null strip right here.
  {
    restored.addBus(QStringLiteral("FX"));
    const int effect = pick_effect(restored);
    if (effect >= 0) restored.addInsert(restored.rowCount() - 1, effect);
    restored.saveSessionAs(QUrl::fromLocalFile(dir.path() + "/named.json"));

    if (!restored.loadSessionFrom(QUrl::fromLocalFile(dir.path() + "/named.json")))
      fail("loading a named session over a live mixer failed");
    if (restored.rowCount() < 3)
      fail("the named session did not bring its rows back");
  }

  // Saving over a session that already exists. The first save always worked —
  // the file was not there yet — so every check above passed while the second
  // one silently left the new state in session.json.tmp and the old file in
  // place. Anything that changed after the very first save was lost.
  {
    const QString path = dir.path() + "/session.json";
    restored.renameChannel(0, QStringLiteral("Renamed"));
    restored.saveSession();

    if (QFile::exists(path + ".tmp"))
      fail("the temporary file outlived the save; the rename did not happen");

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
      fail("the session file went missing across a resave");
    } else if (!file.readAll().contains("Renamed")) {
      fail("a second save did not reach the session file");
    }
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
