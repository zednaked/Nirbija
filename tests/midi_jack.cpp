// Proves the MIDI path through JACK itself: a second client sends a note into
// the engine's channel MIDI port, a synth on that channel plays it, and the
// master bus meter shows the sound. midi_synth covers the same chain with the
// events handed over directly; this one covers the wiring around it.

#include <jack/jack.h>
#include <jack/midiport.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "core/engine.h"
#include "core/plugin.h"

namespace {

constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kKey = 60;
constexpr uint8_t kVelocity = 100;

// A minimal JACK client that emits one note and then keeps quiet, standing in
// for the sequencer.
class NoteSender {
 public:
  bool start() {
    jack_status_t status;
    client_ = jack_client_open("nirbija-note", JackNoStartServer, &status);
    if (client_ == nullptr) return false;

    port_ = jack_port_register(client_, "out", JACK_DEFAULT_MIDI_TYPE,
                               JackPortIsOutput, 0);
    if (port_ == nullptr) return false;

    jack_set_process_callback(client_, &NoteSender::trampoline, this);
    return jack_activate(client_) == 0;
  }

  void stop() {
    if (client_ == nullptr) return;
    jack_deactivate(client_);
    jack_client_close(client_);
    client_ = nullptr;
  }

  const char* port_name() const { return jack_port_name(port_); }
  void send_note() { pending_.store(true, std::memory_order_release); }

 private:
  static int trampoline(jack_nframes_t frames, void* arg) {
    return static_cast<NoteSender*>(arg)->process(frames);
  }

  int process(jack_nframes_t frames) {
    void* buffer = jack_port_get_buffer(port_, frames);
    jack_midi_clear_buffer(buffer);

    if (!pending_.exchange(false, std::memory_order_acquire)) return 0;

    jack_midi_data_t* event = jack_midi_event_reserve(buffer, 0, 3);
    if (event == nullptr) return 0;
    event[0] = kNoteOn;
    event[1] = kKey;
    event[2] = kVelocity;
    return 0;
  }

  jack_client_t* client_ = nullptr;
  jack_port_t* port_ = nullptr;
  std::atomic<bool> pending_{false};
};

std::unique_ptr<nirbija::PluginInstance> find_synth(const std::string& name) {
  for (auto& backend : nirbija::make_all_backends()) {
    for (const auto& descriptor : backend->scan()) {
      if (descriptor.name != name || !descriptor.has_midi_input) continue;
      auto instance = backend->instantiate(descriptor);
      if (instance != nullptr) return instance;
    }
  }
  return nullptr;
}

// Runs the engine for a while and reports the loudest master peak seen.
float loudest_master(nirbija::Engine& engine, int milliseconds) {
  float loudest = 0.0f;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    loudest = std::max(loudest, engine.graph().read_master_peak(0));
  }
  return loudest;
}

}  // namespace

int main(int argc, char* argv[]) {
  const std::string wanted = argc > 1 ? argv[1] : "Odin2";

  nirbija::Engine engine;
  if (!engine.start("nirbija-miditest")) {
    std::printf("no JACK server available, skipping\n");
    return 0;
  }

  auto synth = find_synth(wanted);
  if (synth == nullptr) {
    std::printf("%s not installed or has no MIDI input, skipping\n", wanted.c_str());
    return 0;
  }

  const size_t channel = engine.add_channel("synth", 2);
  if (channel == nirbija::kMaxChannels) {
    std::fprintf(stderr, "FAIL could not add a channel\n");
    return 1;
  }
  if (!engine.graph().channel(channel).add_insert(std::move(synth))) {
    std::fprintf(stderr, "FAIL could not load the synth as an insert\n");
    return 1;
  }

  NoteSender sender;
  if (!sender.start()) {
    std::printf("could not open a second JACK client, skipping\n");
    return 0;
  }

  // The server owns the final port names — it may rename a client to keep names
  // unique — so they are looked up rather than assumed.
  const char** inputs = jack_get_ports(engine.client(), nullptr,
                                       JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
  std::string target;
  for (const char** port = inputs; port != nullptr && *port != nullptr; ++port) {
    const std::string name = *port;
    if (name.find("midi_in") != std::string::npos) {
      target = name;
      break;
    }
  }
  if (inputs != nullptr) jack_free(inputs);

  if (target.empty()) {
    std::fprintf(stderr, "FAIL the engine registered no MIDI input port\n");
    sender.stop();
    return 1;
  }

  // A port the server has only just learned about is not connectable yet, so
  // this retries rather than failing on the first attempt.
  bool connected = false;
  for (int attempt = 0; attempt < 20 && !connected; ++attempt) {
    connected = jack_connect(engine.client(), sender.port_name(), target.c_str()) == 0;
    if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!connected) {
    std::fprintf(stderr, "FAIL could not connect %s to %s\n", sender.port_name(),
                 target.c_str());
    sender.stop();
    return 1;
  }

  // Silence first, so a synth droning on its own would not pass as a success.
  const float before = loudest_master(engine, 150);
  sender.send_note();
  const float after = loudest_master(engine, 400);

  sender.stop();
  engine.stop();

  if (before > 1e-4f) {
    std::fprintf(stderr, "FAIL master was not silent before the note: %.6f\n", before);
    return 1;
  }
  if (after <= 1e-4f) {
    std::fprintf(stderr, "FAIL note sent through JACK produced no audio\n");
    return 1;
  }

  std::printf("ok: note through JACK reached %s, master peak %.6f\n",
              wanted.c_str(), after);
  return 0;
}
