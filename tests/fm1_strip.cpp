// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// The FM-1 put in a strip the way a person does it, through the mixer
// (design/fm1.md, step 3). A JACK client stands for the FM-1 on USB: MIDI in
// and out and two capture ports, all with "Jangada" in their names, as the
// device shows them. Picking FM-1 for a strip wires the engine's ports to it
// and the strip's input to its audio; removing it unwires everything before
// the plugin is freed; a strip set to "no input" on purpose stays that way.

#include <jack/jack.h>
#include <jack/midiport.h>

#include <QCoreApplication>
#include <QElapsedTimer>
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

// The FM-1's ports on the bus, doing nothing: only who connects to them counts.
class FakeUsb {
 public:
  bool start() {
    jack_status_t status;
    client_ = jack_client_open("nirbija-fake-jangada", JackNoStartServer, &status);
    if (client_ == nullptr) return false;
    midi_in_ = jack_port_register(client_, "Jangada MIDI in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    midi_out_ = jack_port_register(client_, "Jangada MIDI out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
    audio_[0] = jack_port_register(client_, "Jangada capture_FL", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    audio_[1] = jack_port_register(client_, "Jangada capture_FR", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    jack_set_process_callback(client_, &FakeUsb::silence, this);
    return midi_in_ && midi_out_ && audio_[0] && audio_[1] && jack_activate(client_) == 0;
  }
  void stop() {
    if (client_ == nullptr) return;
    jack_deactivate(client_);
    jack_client_close(client_);
    client_ = nullptr;
  }
  int midi_links() const { return jack_port_connected(midi_in_) + jack_port_connected(midi_out_); }
  int audio_links() const { return jack_port_connected(audio_[0]) + jack_port_connected(audio_[1]); }

 private:
  static int silence(jack_nframes_t frames, void* arg) {
    auto* self = static_cast<FakeUsb*>(arg);
    for (jack_port_t* port : self->audio_)
      std::fill_n(static_cast<float*>(jack_port_get_buffer(port, frames)), frames, 0.0f);
    jack_midi_clear_buffer(jack_port_get_buffer(self->midi_out_, frames));
    return 0;
  }
  jack_client_t* client_ = nullptr;
  jack_port_t* midi_in_ = nullptr;
  jack_port_t* midi_out_ = nullptr;
  jack_port_t* audio_[2] = {nullptr, nullptr};
};

// Lets the mixer's timers run (the level poll is where devices are synced).
template <typename Done>
bool settle(int ms, Done done) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (done()) return true;
  }
  return done();
}

int fm1_row(nirbija::MixerModel& mixer) {
  for (int i = 0; i < mixer.plugins()->rowCount(); ++i) {
    const nirbija::PluginDescriptor* d = mixer.plugins()->descriptor(i);
    if (d != nullptr && d->uid == "nirbija.fm1") return i;
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
  // A real FM-1 on the bus would be a second "Jangada", and which of the two
  // a strip takes depends on the order the server lists them in.
  if (!mixer.engineForTests().sources_matching("Jangada", true).empty()) {
    std::printf("a real FM-1 is plugged in; this test needs the bus to itself, skipping\n");
    return 77;
  }
  FakeUsb usb;
  if (!usb.start()) {
    std::printf("could not open a second JACK client, skipping\n");
    return 77;
  }

  const int plugin = fm1_row(mixer);
  expect(plugin >= 0, "picker: FM-1 is in the list");
  mixer.addChannel(QStringLiteral("FM-1"), 2);
  expect(plugin >= 0 && mixer.addInsert(0, plugin), "strip: FM-1 placed");
  nirbija::Engine& engine = mixer.engineForTests();
  expect(settle(500, [&] { return usb.midi_links() >= 3; }),
         "strip: the engine's ports wired to the FM-1's MIDI both ways, the clock along");
  const std::string source = engine.current_source(0, false);
  expect(usb.audio_links() == 2 && source.find("Jangada capture_FL") != std::string::npos,
         "strip: its input is the FM-1's USB audio, both sides");

  mixer.removeInsert(0, 0);
  expect(settle(500, [&] { return usb.midi_links() == 0; }), "remove: unwired at the next poll");
  // Polls past the reclaim: the plugin is freed after it was detached, and the
  // audio thread is still running happily (a use after free would be ASan's).
  settle(300, [] { return false; });

  // Back in, then "no input" by hand: two rewiring rounds later, still none.
  mixer.addInsert(0, plugin);
  settle(500, [&] { return usb.midi_links() >= 3; });
  mixer.connectSource(0, QString(), false);
  settle(4500, [] { return false; });
  expect(engine.current_source(0, false).empty() && usb.midi_links() >= 3,
         "no input chosen by hand stays chosen; the MIDI stays wired");

  mixer.removeChannel(0);
  expect(settle(500, [&] { return usb.midi_links() == 0; }), "remove the strip: unwired too");
  settle(300, [] { return false; });

  usb.stop();
  std::printf("%s\n", failures ? "FAILED" : "all ok");
  return failures ? 1 : 0;
}
