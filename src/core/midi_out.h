// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

// One block's worth of outgoing MIDI from a built-in plugin, kept in a fixed
// array on the audio thread. The sequencer, the arpeggiator, the chord
// plugin, the keyboard and the script plugin each used to carry their own
// copy of the same three things: an emit() that fills a MidiEvent, a reserve
// of slots that only note-offs may take, and a sort at take time that puts a
// note-off before a note-on landing on the same frame. Here they are once.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "core/plugin.h"

namespace nirbija {

// Beats per step, in quarter notes, indexed by a `division` parameter. The
// same table for every plugin that has one, so they read alike in one strip.
constexpr double kStepDivisions[] = {
    1.0,        // 1/4
    0.5,        // 1/8
    0.25,       // 1/16
    0.125,      // 1/32
    1.0 / 3.0,  // 1/4 triplet
    1.0 / 6.0,  // 1/8 triplet
};
constexpr int kStepDivisionCount = static_cast<int>(std::size(kStepDivisions));

template <size_t Capacity>
class MidiOutBlock {
 public:
  static constexpr size_t kCapacity = Capacity;

  // Three-byte message. Returns false when the block is full - a note-on
  // dropped is a missed note; the last eighth of the block is kept for
  // note-offs so a dropped note-off, which is a stuck note, does not happen
  // until the block is completely full of note-offs.
  bool emit(uint32_t frame, uint8_t status, uint8_t data1, uint8_t data2) {
    MidiEvent event;
    event.frame = frame;
    event.size = 3;
    event.data[0] = status;
    event.data[1] = data1;
    event.data[2] = data2;
    return push(event);
  }

  bool push(const MidiEvent& event) {
    if (!midi_queue_admits(count_, Capacity, event)) return false;
    events_[count_++] = event;
    return true;
  }

  size_t count() const { return count_; }
  bool empty() const { return count_ == 0; }
  void clear() { count_ = 0; }
  MidiEvent* begin() { return events_.data(); }
  MidiEvent* end() { return events_.data() + count_; }

  // Stable by frame, and on the same frame a note-off before a note-on, so a
  // repeated pitch that turns over on one frame closes before it reopens.
  // Everything else keeps the order it was emitted in.
  void sort() {
    std::stable_sort(events_.begin(), events_.begin() + count_,
                     [](const MidiEvent& a, const MidiEvent& b) {
                       if (a.frame != b.frame) return a.frame < b.frame;
                       const bool a_off = midi_is_note_off(a);
                       const bool b_off = midi_is_note_off(b);
                       return a_off && !b_off;
                     });
  }

  // The usual take_midi_output(): sort, copy out up to `capacity`, reset.
  size_t take(MidiEvent* out, size_t capacity) {
    sort();
    const size_t n = std::min(count_, capacity);
    std::copy_n(events_.begin(), n, out);
    count_ = 0;
    return n;
  }

 private:
  std::array<MidiEvent, Capacity> events_{};
  size_t count_ = 0;
};

// Orders a run of events already in a buffer by frame, note-offs first on a
// tie, without allocating. For the strip's chain buffer, where injected
// events and each insert's output are appended out of order and LV2 and CLAP
// both require non-decreasing time.
inline void sort_midi_by_frame(MidiEvent* events, size_t count) {
  // Insertion sort: n is a few dozen at most and the input is nearly sorted.
  for (size_t i = 1; i < count; ++i) {
    MidiEvent key = events[i];
    const bool key_off = midi_is_note_off(key);
    size_t j = i;
    while (j > 0) {
      const MidiEvent& prev = events[j - 1];
      const bool after = prev.frame > key.frame ||
                         (prev.frame == key.frame && key_off && !midi_is_note_off(prev));
      if (!after) break;
      events[j] = events[j - 1];
      --j;
    }
    events[j] = key;
  }
}

}  // namespace nirbija
