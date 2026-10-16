// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include <atomic>
#include <string>

#include "core/midi_out.h"
#include "core/rt_byte_ring.h"

namespace nirbija {

// A hardware instrument on the other side of a pair of the engine's MIDI ports
// (design/fm1.md). The plugin that stands for it owns one of these and hands
// it to Engine::attach_external(); the engine registers `extN_out` and
// `extN_in`, wires them to the device's ports, and every block:
//
//   - sends `notes` (the instrument's own MIDI, filled during the render) and
//     then as many whole frames of `to_device` as fit, each as one event;
//   - copies every event arriving on `extN_in` into `from_device`, SysEx and
//     all, as it arrives. A long SysEx may come in several events: whoever
//     reads `from_device` treats it as a byte stream and finds F0 .. F7 itself.
//
// The audio thread only moves bytes; building and reading messages is the
// worker's (Fm1Link).
struct ExternalMidi {
  static constexpr size_t kRingBytes = 8192;

  // Part of the device's JACK port names ("Felucca"), matched without case.
  // UI thread, set before attaching.
  std::string match;

  RtByteRing<kRingBytes> to_device;    // worker -> audio thread
  RtByteRing<kRingBytes> from_device;  // audio thread -> worker

  // Audio thread only: the instrument emits into it while the graph renders,
  // the engine sends and clears it right after.
  MidiOutBlock<128> notes;

  // Whether the engine services this device: set by attach_external() once
  // `notes` is empty, cleared by detach_external(). The instrument emits only
  // while it is set, or an unattached instrument would fill `notes` and the
  // next attach would send that stale block at once.
  std::atomic<bool> serviced{false};
};

}  // namespace nirbija
