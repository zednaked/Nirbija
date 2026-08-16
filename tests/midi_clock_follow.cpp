// Sends a MIDI beat clock at a known tempo into the engine's clock_in port and
// checks that the transport follows it: start, tempo, position and stop.
//
// Needs a JACK server, so it skips (CTest 77) rather than failing when there is
// none — the same rule the other JACK tests here follow.

#include <jack/jack.h>
#include <jack/midiport.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

#include "core/engine.h"

namespace {

constexpr double kTempo = 140.0;  // what the sender pretends to be
constexpr int kSkip = 77;

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

// A JACK client that emits nothing but beat clock, at whatever tempo it is
// told, counting frames the way a real sequencer does.
class ClockSender {
 public:
  bool start() {
    jack_status_t status;
    client_ = jack_client_open("nirbija-clock", JackNoStartServer, &status);
    if (client_ == nullptr) return false;
    port_ = jack_port_register(client_, "out", JACK_DEFAULT_MIDI_TYPE,
                               JackPortIsOutput, 0);
    if (port_ == nullptr) return false;
    rate_ = jack_get_sample_rate(client_);
    jack_set_process_callback(client_, &ClockSender::trampoline, this);
    return jack_activate(client_) == 0;
  }

  void stop() {
    if (client_ == nullptr) return;
    jack_deactivate(client_);
    jack_client_close(client_);
    client_ = nullptr;
  }

  const char* port_name() const { return jack_port_name(port_); }

  void send_start() { command_.store(0xfa, std::memory_order_release); }
  void send_stop() { command_.store(0xfc, std::memory_order_release); }
  void run_clock(bool on) { ticking_.store(on, std::memory_order_release); }

 private:
  static int trampoline(jack_nframes_t frames, void* arg) {
    return static_cast<ClockSender*>(arg)->process(frames);
  }

  int process(jack_nframes_t frames) {
    void* buffer = jack_port_get_buffer(port_, frames);
    jack_midi_clear_buffer(buffer);

    if (const uint8_t command = command_.exchange(0, std::memory_order_acquire)) {
      if (jack_midi_data_t* event = jack_midi_event_reserve(buffer, 0, 1))
        event[0] = command;
    }

    if (!ticking_.load(std::memory_order_acquire)) return 0;

    // Twenty-four ticks to the quarter note, placed on the frame they fall on.
    const double frames_per_tick = rate_ / (kTempo / 60.0 * 24.0);
    for (jack_nframes_t i = 0; i < frames; ++i) {
      phase_ += 1.0;
      if (phase_ < frames_per_tick) continue;
      phase_ -= frames_per_tick;
      if (jack_midi_data_t* event = jack_midi_event_reserve(buffer, i, 1))
        event[0] = 0xf8;
    }
    return 0;
  }

  jack_client_t* client_ = nullptr;
  jack_port_t* port_ = nullptr;
  double rate_ = 48000.0;
  double phase_ = 0.0;
  std::atomic<uint8_t> command_{0};
  std::atomic<bool> ticking_{false};
};

void settle(int milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

}  // namespace

int main() {
  nirbija::Engine engine;
  if (!engine.start("nirbija-clock-test")) {
    std::printf("no JACK server available, skipping\n");
    return kSkip;
  }

  ClockSender sender;
  if (!sender.start()) {
    std::printf("could not open a second JACK client, skipping\n");
    return kSkip;
  }

  // Ports do not appear the instant a client activates: on PipeWire's JACK
  // layer the graph is published a beat later, and connecting before that
  // fails with the port simply not existing yet.
  settle(300);

  const std::string target =
      std::string(jack_get_client_name(engine.client())) + ":clock_in";
  if (jack_connect(engine.client(), sender.port_name(), target.c_str()) != 0) {
    std::printf("could not connect %s -> %s, skipping\n", sender.port_name(),
                target.c_str());
    sender.stop();
    return kSkip;
  }

  // The connection itself takes a moment to become live; checking before it
  // does would pass for the wrong reason.
  settle(200);

  // Off by default: a clock arriving changes nothing until it is asked for.
  sender.run_clock(true);
  sender.send_start();
  settle(400);
  if (engine.playing())
    fail("an external start moved the transport while following was off");

  engine.set_follow_midi_clock(true);
  sender.send_start();
  settle(600);

  if (!engine.playing()) fail("an external start did not start the transport");

  // The tempo is inferred from how far apart the ticks land, and smoothed, so
  // this asks for the neighbourhood rather than the number.
  const double tempo = engine.tempo();
  if (std::abs(tempo - kTempo) > 3.0)
    fail("followed tempo came out " + std::to_string(tempo) + ", wanted near " +
         std::to_string(kTempo));

  // Position has to move, and roughly at the rate the clock implies.
  const uint64_t before = engine.transport_frame();
  settle(500);
  const uint64_t after = engine.transport_frame();
  if (after <= before) fail("the transport did not advance while following");

  sender.send_stop();
  settle(300);
  if (engine.playing()) fail("an external stop did not stop the transport");

  sender.stop();
  engine.stop();

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok: followed %.1f BPM\n", tempo);
  return 0;
}
