// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// MIDI reaches every insert in time order. The strip's chain buffer is
// appended to out of order - the port's events, then injected ones, then
// whatever each insert upstream produced - and LV2 atom sequences and CLAP
// both require non-decreasing time. A note-off on the same frame as a
// note-on goes first, so a repeated pitch closes before it reopens.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/channel_strip.h"

namespace {

int failures = 0;

void expect(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what.c_str());
    ++failures;
  }
}

nirbija::MidiEvent note(uint32_t frame, bool on, uint8_t key = 60) {
  nirbija::MidiEvent e;
  e.frame = frame;
  e.size = 3;
  e.data[0] = on ? 0x90 : 0x80;
  e.data[1] = key;
  e.data[2] = on ? 100 : 0;
  return e;
}

// Records what it was handed, in the order it was handed, and emits what it
// was told to on the next take.
class MidiWitness : public nirbija::PluginInstance {
 public:
  void set_channel_layout(int) override {}
  bool activate(double, uint32_t) override { return true; }
  void deactivate() override {}
  void process(const float* const*, float* const*, uint32_t) override {}
  void queue_midi(const nirbija::MidiEvent& event) override { received.push_back(event); }
  size_t take_midi_output(nirbija::MidiEvent* out, size_t capacity) override {
    const size_t n = std::min(capacity, to_emit.size());
    for (size_t i = 0; i < n; ++i) out[i] = to_emit[i];
    to_emit.clear();
    return n;
  }
  std::vector<nirbija::ParameterInfo> parameters() const override { return {}; }
  double parameter_value(uint32_t) const override { return 0.0; }
  void set_parameter(uint32_t, double) override {}
  std::vector<uint8_t> save_state() const override { return {}; }
  bool load_state(const std::vector<uint8_t>&) override { return true; }
  const nirbija::PluginDescriptor& descriptor() const override { return desc_; }

  std::vector<nirbija::MidiEvent> received;
  std::vector<nirbija::MidiEvent> to_emit;

 private:
  nirbija::PluginDescriptor desc_{.format = nirbija::PluginFormat::Internal,
                                  .uid = "test.witness",
                                  .name = "Witness",
                                  .kind = nirbija::PluginKind::MidiEffect,
                                  .has_midi_input = true};
};

bool ordered(const std::vector<nirbija::MidiEvent>& events) {
  for (size_t i = 1; i < events.size(); ++i) {
    if (events[i].frame < events[i - 1].frame) return false;
    if (events[i].frame == events[i - 1].frame &&
        nirbija::midi_is_note_off(events[i]) && !nirbija::midi_is_note_off(events[i - 1]))
      return false;
  }
  return true;
}

std::string frames_of(const std::vector<nirbija::MidiEvent>& events) {
  std::string s;
  for (const auto& e : events)
    s += std::to_string(e.frame) + (nirbija::midi_is_note_off(e) ? "off " : "on ");
  return s;
}

}  // namespace

int main() {
  nirbija::ChannelStrip strip("s", 2);
  strip.prepare(48000.0, 256);

  auto first = std::make_unique<MidiWitness>();
  auto second = std::make_unique<MidiWitness>();
  MidiWitness* a = first.get();
  MidiWitness* b = second.get();
  expect(strip.add_insert(std::move(first)), "could not add a");
  expect(strip.add_insert(std::move(second)), "could not add b");

  std::vector<float> left(256, 0.0f), right(256, 0.0f);
  float* buffers[2] = {left.data(), right.data()};

  // What the graph hands over: the port's events in order, then injected ones
  // appended after them, out of order.
  std::vector<nirbija::MidiEvent> in = {note(100, true), note(200, false, 61),
                                        note(50, true, 62), note(50, false, 62),
                                        note(0, true, 63)};
  // The first insert answers with events of its own, out of order too.
  a->to_emit = {note(150, true, 70), note(10, true, 71), note(150, false, 70)};

  strip.process(buffers, 256, in.data(), in.size());

  expect(a->received.size() == 5, "a did not get every event");
  expect(ordered(a->received), "a got them out of order: " + frames_of(a->received));
  // The tie at frame 50: off before on.
  expect(a->received.size() >= 3 && a->received[1].frame == 50 &&
             nirbija::midi_is_note_off(a->received[1]) && a->received[2].frame == 50 &&
             !nirbija::midi_is_note_off(a->received[2]),
         "note-off on the same frame was not put first: " + frames_of(a->received));

  expect(b->received.size() == 8, "b did not get the port's events plus a's");
  expect(ordered(b->received), "b got the merged run out of order: " + frames_of(b->received));
  const std::string want = "0on 10on 50off 50on 100on 150off 150on 200off ";
  expect(frames_of(b->received) == want,
         "b's run is not the merged order: got " + frames_of(b->received) + " want " + want);

  // A bypassed insert still gets them in order.
  a->received.clear();
  b->received.clear();
  strip.set_insert_bypassed(0, true);
  for (int i = 0; i < 16; ++i) strip.process(buffers, 256);  // settle the crossfade
  a->received.clear();
  b->received.clear();
  strip.process(buffers, 256, in.data(), in.size());
  expect(a->received.size() == 5 && ordered(a->received),
         "a bypassed insert got them out of order: " + frames_of(a->received));
  expect(b->received.size() == 5 && ordered(b->received),
         "after a bypassed insert the order was lost: " + frames_of(b->received));

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
