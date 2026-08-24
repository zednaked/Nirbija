#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "core/plugin.h"
#include "core/rt_queue.h"

namespace nirbija {

// Bluetooth LE MIDI, which PipeWire advertises as a JACK port but never
// actually AcquireNotify's on the GATT characteristic — so the port exists
// and stays silent. We take the notify FD ourselves, decode BLE MIDI
// packets, and hand 1–3 byte messages to the audio thread.
class BleMidi {
 public:
  BleMidi();
  ~BleMidi();

  void start();
  void stop();

  // Audio thread. Drains what the BLE thread pushed.
  size_t pop(MidiEvent* out, size_t capacity);

 private:
  void thread_main();
  bool run_one_device();
  void feed(const uint8_t* data, size_t n);
  void emit(uint8_t status, uint8_t d1, uint8_t d2, uint8_t size);

  RtQueue<MidiEvent, 256> queue_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  uint8_t running_status_{0};
};

}  // namespace nirbija
