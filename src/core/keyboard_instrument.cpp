// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "core/keyboard_instrument.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

namespace nirbija {
namespace {

enum Params : uint32_t {
  kChannel = 0,
};

constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kNoteOff = 0x80;

int clamp_int(double value, int low, int high) {
  const int rounded = static_cast<int>(value + (value >= 0.0 ? 0.5 : -0.5));
  return std::clamp(rounded, low, high);
}

}  // namespace

PluginDescriptor KeyboardInstrumentInstance::make_descriptor() {
  PluginDescriptor descriptor;
  descriptor.format = PluginFormat::Internal;
  descriptor.uid = "nirbija.keyboard";
  descriptor.name = "Computer Keyboard";
  descriptor.vendor = "Nirbija";
  // Same shape as the step sequencer: nothing but notes, so the picker sorts
  // it as MIDI without being told to.
  descriptor.audio_inputs = 0;
  descriptor.audio_outputs = 0;
  descriptor.has_midi_input = true;
  descriptor.category = "Keyboard";
  descriptor.kind = PluginKind::MidiEffect;
  return descriptor;
}

KeyboardInstrumentInstance::KeyboardInstrumentInstance()
    : descriptor_(make_descriptor()) {}

bool KeyboardInstrumentInstance::activate(double sample_rate, uint32_t) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  held_.fill(false);
  out_.clear();
  release_all_.store(false, std::memory_order_relaxed);
  // Drop anything a previous activation left queued rather than let it fire
  // the moment this one starts processing.
  KeyEvent discard;
  while (incoming_.pop(discard)) {
  }
  return true;
}

void KeyboardInstrumentInstance::deactivate() {
  held_.fill(false);
  out_.clear();
}

void KeyboardInstrumentInstance::key_down(int note, int velocity) {
  KeyEvent event;
  event.note = static_cast<uint8_t>(std::clamp(note, 0, 127));
  event.velocity = static_cast<uint8_t>(std::clamp(velocity, 1, 127));
  event.down = true;
  // A key-down the queue cannot take is a missed note, and nothing more.
  incoming_.push(event);
}

void KeyboardInstrumentInstance::key_up(int note) {
  KeyEvent event;
  event.note = static_cast<uint8_t>(std::clamp(note, 0, 127));
  event.down = false;
  if (!incoming_.push(event))
    release_all_.store(true, std::memory_order_release);
}

void KeyboardInstrumentInstance::emit_event(uint32_t frame, uint8_t status,
                                            uint8_t data1, uint8_t data2) {
  out_.emit(frame, status, data1, data2);
}

void KeyboardInstrumentInstance::queue_midi(const MidiEvent& event) {
  // Passed along untouched, so a step sequencer or another instrument can sit
  // ahead of this in the same strip and still be heard.
  out_.push(event);
}

void KeyboardInstrumentInstance::process(const float* const*, float* const*,
                                         uint32_t frames) {
  const uint8_t channel =
      static_cast<uint8_t>(std::clamp(channel_.load(std::memory_order_relaxed), 0, 15));

  // Every frame in the block is close enough to the same instant a physical
  // key press already was by the time it got off the UI thread - there is no
  // finer timestamp worth chasing here. The one exception: a key that went
  // down and up inside this same block. Its off goes one frame later than
  // its on, because the output sorts a note-off before a note-on on the same
  // frame, and the other way round would leave the note ringing.
  const uint32_t off_after_on = frames > 1 ? 1 : 0;
  std::array<bool, 128> struck{};
  KeyEvent key;
  while (incoming_.pop(key)) {
    if (key.down) {
      if (held_[key.note]) continue;  // OS key-repeat, or a duplicate press
      held_[key.note] = true;
      struck[key.note] = true;
      emit_event(0, static_cast<uint8_t>(kNoteOn | channel), key.note,
                key.velocity);
    } else {
      if (!held_[key.note]) continue;
      held_[key.note] = false;
      emit_event(struck[key.note] ? off_after_on : 0,
                 static_cast<uint8_t>(kNoteOff | channel), key.note, 0);
    }
  }

  if (release_all_.exchange(false, std::memory_order_acq_rel)) {
    for (size_t note = 0; note < held_.size(); ++note) {
      if (!held_[note]) continue;
      held_[note] = false;
      emit_event(struck[note] ? off_after_on : 0,
                 static_cast<uint8_t>(kNoteOff | channel),
                 static_cast<uint8_t>(note), 0);
    }
  }
}

size_t KeyboardInstrumentInstance::take_midi_output(MidiEvent* out,
                                                    size_t capacity) {
  return out_.take(out, capacity);
}

std::vector<ParameterInfo> KeyboardInstrumentInstance::parameters() const {
  return {{kChannel, "MIDI channel", 1.0, 16.0, 1.0}};
}

double KeyboardInstrumentInstance::parameter_value(uint32_t id) const {
  if (id == kChannel) return channel_.load(std::memory_order_relaxed) + 1;
  return 0.0;
}

void KeyboardInstrumentInstance::set_parameter(uint32_t id, double value) {
  if (id == kChannel)
    channel_.store(clamp_int(value, 1, 16) - 1, std::memory_order_relaxed);
}

std::vector<uint8_t> KeyboardInstrumentInstance::save_state() const {
  char line[32];
  std::snprintf(line, sizeof(line), "channel %d\n",
               channel_.load(std::memory_order_relaxed));
  const std::string text(line);
  return {text.begin(), text.end()};
}

bool KeyboardInstrumentInstance::load_state(const std::vector<uint8_t>& blob) {
  const std::string text(blob.begin(), blob.end());
  size_t position = 0;
  while (position < text.size()) {
    const size_t end = text.find('\n', position);
    const std::string_view line(
        text.data() + position,
        (end == std::string::npos ? text.size() : end) - position);
    position = (end == std::string::npos) ? text.size() : end + 1;
    if (line.empty()) continue;

    const size_t space = line.find(' ');
    if (space == std::string_view::npos) continue;
    const std::string_view key = line.substr(0, space);
    const std::string_view rest = line.substr(space + 1);

    double value = 0.0;
    // The stored value is the 0-based channel_ field itself; set_parameter
    // expects the 1-based number the user sees.
    if (key == "channel" && parse_number(rest, &value))
      set_parameter(kChannel, value + 1);
  }
  return true;
}

}  // namespace nirbija
