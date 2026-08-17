// Feeds the arpeggiator a chord and reads back the figure it makes. Same shape
// as the step sequencer's test: a synthetic transport, no JACK, no display.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/arpeggiator.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

void expect(bool condition, const std::string& what) {
  if (!condition) fail(what);
}

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 6000;  // a quarter of a beat at 120 bpm
constexpr double kTempo = 120.0;
constexpr double kBlockBeats = 0.25;

using Arp = nirbija::ArpeggiatorInstance;

void note_on(Arp& arp, int pitch, int velocity = 100) {
  nirbija::MidiEvent event{};
  event.size = 3;
  event.data[0] = 0x90;
  event.data[1] = static_cast<uint8_t>(pitch);
  event.data[2] = static_cast<uint8_t>(velocity);
  arp.queue_midi(event);
}

void note_off(Arp& arp, int pitch) {
  nirbija::MidiEvent event{};
  event.size = 3;
  event.data[0] = 0x80;
  event.data[1] = static_cast<uint8_t>(pitch);
  arp.queue_midi(event);
}

// One block, returning the pitches that started in it.
std::vector<int> block(Arp& arp, int index, bool playing = true) {
  nirbija::TransportInfo transport;
  transport.playing = playing;
  transport.tempo_bpm = kTempo;
  transport.beats = index * kBlockBeats;
  arp.set_transport(transport);
  arp.process(nullptr, nullptr, kBlock);

  nirbija::MidiEvent buffer[64];
  const size_t count = arp.take_midi_output(buffer, 64);
  std::vector<int> pitches;
  for (size_t e = 0; e < count; ++e)
    if ((buffer[e].data[0] & 0xf0) == 0x90 && buffer[e].data[2] > 0)
      pitches.push_back(buffer[e].data[1]);
  return pitches;
}

// The pitches started over `count` blocks, one block per step.
std::vector<int> run(Arp& arp, int count, int from = 0) {
  std::vector<int> all;
  for (int i = 0; i < count; ++i)
    for (const int pitch : block(arp, from + i)) all.push_back(pitch);
  return all;
}

std::string show(const std::vector<int>& values) {
  std::string text;
  for (const int value : values) text += std::to_string(value) + " ";
  return text;
}

}  // namespace

int main() {
  // C major triad, deliberately played out of order so sorting is doing work.
  const int E = 64, C = 60, G = 67;

  // --- up walks the chord low to high, whatever order it arrived in ----------
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    note_on(arp, E); note_on(arp, C); note_on(arp, G);

    const std::vector<int> got = run(arp, 6);
    const std::vector<int> want{60, 64, 67, 60, 64, 67};
    expect(got == want, "up played " + show(got) + "- wanted " + show(want));
  }

  // --- down is the same set, the other way round -----------------------------
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    arp.set_parameter(1, Arp::Down);
    note_on(arp, C); note_on(arp, E); note_on(arp, G);

    const std::vector<int> got = run(arp, 6);
    const std::vector<int> want{67, 64, 60, 67, 64, 60};
    expect(got == want, "down played " + show(got) + "- wanted " + show(want));
  }

  // --- as-played keeps the order the keys arrived in --------------------------
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    arp.set_parameter(1, Arp::AsPlayed);
    note_on(arp, G); note_on(arp, C); note_on(arp, E);

    const std::vector<int> got = run(arp, 3);
    const std::vector<int> want{67, 60, 64};
    expect(got == want, "as-played played " + show(got) + "- wanted " + show(want));
  }

  // --- up-down turns without striking the ends twice --------------------------
  //
  // The bug this guards is the one that makes a turn sound like a stutter:
  // 60 64 67 67 64 60 instead of 60 64 67 64.
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    arp.set_parameter(1, Arp::UpDown);
    note_on(arp, C); note_on(arp, E); note_on(arp, G);

    const std::vector<int> got = run(arp, 8);
    const std::vector<int> want{60, 64, 67, 64, 60, 64, 67, 64};
    expect(got == want, "up-down played " + show(got) + "- wanted " + show(want));
  }

  // --- octaves stack the whole figure above itself ---------------------------
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    arp.set_parameter(2, 2.0);
    note_on(arp, C); note_on(arp, E); note_on(arp, G);

    const std::vector<int> got = run(arp, 6);
    const std::vector<int> want{60, 64, 67, 72, 76, 79};
    expect(got == want, "two octaves played " + show(got) + "- wanted " + show(want));
  }

  // --- chord mode sounds the handful together --------------------------------
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    arp.set_parameter(1, Arp::Chord);
    note_on(arp, C); note_on(arp, E); note_on(arp, G);

    const std::vector<int> first = block(arp, 0);
    std::vector<int> sorted = first;
    std::sort(sorted.begin(), sorted.end());
    expect(sorted == std::vector<int>({60, 64, 67}),
           "chord mode played " + show(first) + "- wanted all three");
  }

  // --- releasing the keys stops it, and nothing is left ringing ---------------
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    note_on(arp, C); note_on(arp, E);
    run(arp, 2);

    note_off(arp, C);
    note_off(arp, E);

    // The block where the last key came up has to release what was sounding.
    nirbija::TransportInfo transport;
    transport.playing = true;
    transport.tempo_bpm = kTempo;
    transport.beats = 2 * kBlockBeats;
    arp.set_transport(transport);
    arp.process(nullptr, nullptr, kBlock);

    nirbija::MidiEvent buffer[64];
    const size_t count = arp.take_midi_output(buffer, 64);
    int ons = 0;
    bool released = false;
    for (size_t e = 0; e < count; ++e) {
      const uint8_t status = buffer[e].data[0] & 0xf0;
      if (status == 0x90 && buffer[e].data[2] > 0) ++ons;
      if (status == 0x80 || (status == 0x90 && buffer[e].data[2] == 0))
        released = true;
    }
    expect(ons == 0, "the arpeggiator kept playing after the keys came up");
    expect(released, "letting go of the chord left a note ringing");

    // And it does not restart mid-figure: a new chord begins at the top.
    note_on(arp, G);
    const std::vector<int> got = run(arp, 1, 3);
    expect(got == std::vector<int>({67}),
           "a new chord did not start at the top of the figure");
  }

  // --- latch keeps the chord after the hands leave ---------------------------
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    arp.set_parameter(5, 1.0);  // latch
    note_on(arp, C); note_on(arp, E);
    note_off(arp, C); note_off(arp, E);

    const std::vector<int> got = run(arp, 4);
    expect(got == std::vector<int>({60, 64, 60, 64}),
           "latch did not hold the chord: " + show(got));

    // A new key starts a new chord rather than joining the latched one.
    note_on(arp, G);
    const std::vector<int> after = run(arp, 2, 4);
    expect(after == std::vector<int>({67, 67}),
           "a key pressed after latch joined the old chord: " + show(after));
  }

  // A note-off mid-chord is not "the hands left". Latch should keep adding.
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    arp.set_parameter(5, 1.0);  // latch
    note_on(arp, C); note_on(arp, E); note_on(arp, G);
    note_off(arp, C);
    note_on(arp, 65);  // F
    const std::vector<int> got = run(arp, 4);
    bool saw_e = false, saw_g = false, saw_f = false;
    for (int n : got) {
      if (n == 64) saw_e = true;
      if (n == 67) saw_g = true;
      if (n == 65) saw_f = true;
    }
    expect(saw_e && saw_g && saw_f,
           "latch treated a mid-chord note-off as a new chord: " + show(got));
  }

  // --- stopping the transport releases everything ----------------------------
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    note_on(arp, C); note_on(arp, E);
    run(arp, 2);

    nirbija::TransportInfo stopped;
    stopped.playing = false;
    stopped.tempo_bpm = kTempo;
    stopped.beats = 2 * kBlockBeats;
    arp.set_transport(stopped);
    arp.process(nullptr, nullptr, kBlock);

    nirbija::MidiEvent buffer[64];
    const size_t count = arp.take_midi_output(buffer, 64);
    bool released = false;
    for (size_t e = 0; e < count; ++e)
      if ((buffer[e].data[0] & 0xf0) == 0x80) released = true;
    expect(released, "stopping the transport left a note ringing");
  }

  // --- the same beat twice plays one step ------------------------------------
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    note_on(arp, C); note_on(arp, E); note_on(arp, G);

    std::vector<int> got;
    for (const double beat : {0.0, 0.0, -1e-12}) {
      nirbija::TransportInfo transport;
      transport.playing = true;
      transport.tempo_bpm = kTempo;
      transport.beats = beat;
      arp.set_transport(transport);
      arp.process(nullptr, nullptr, kBlock);

      nirbija::MidiEvent buffer[64];
      const size_t count = arp.take_midi_output(buffer, 64);
      for (size_t e = 0; e < count; ++e)
        if ((buffer[e].data[0] & 0xf0) == 0x90 && buffer[e].data[2] > 0)
          got.push_back(buffer[e].data[1]);
    }
    expect(got.size() == 1,
           "a beat reported three times played " + std::to_string(got.size()) +
               " notes, wanted 1");
  }

  // --- several steps inside one block still balance ---------------------------
  //
  // At 1/32 two steps land in every block here. The gate releases what was
  // held once, before the steps are walked, so it cannot cover the ones that
  // start and end inside the same block: each step has to let go of the note
  // before it. Without that a synth downstream collects notes it will never be
  // told to stop.
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    arp.set_parameter(0, 3.0);  // 1/32
    arp.set_parameter(3, 1.0);  // full gate, so nothing ends early
    note_on(arp, C); note_on(arp, E); note_on(arp, G);

    int held = 0;
    int most = 0;
    for (int i = 0; i < 8; ++i) {
      nirbija::TransportInfo transport;
      transport.playing = true;
      transport.tempo_bpm = kTempo;
      transport.beats = i * kBlockBeats;
      arp.set_transport(transport);
      arp.process(nullptr, nullptr, kBlock);

      nirbija::MidiEvent buffer[64];
      const size_t count = arp.take_midi_output(buffer, 64);
      for (size_t e = 0; e < count; ++e) {
        const uint8_t status = buffer[e].data[0] & 0xf0;
        if (status == 0x90 && buffer[e].data[2] > 0) ++held;
        else if (status == 0x80) --held;
        most = std::max(most, held);
      }
    }
    expect(most <= 1, "two steps in one block left " + std::to_string(most) +
                          " notes sounding at once, wanted 1");
  }

  // --- input is swallowed unless asked for ------------------------------------
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    arp.set_parameter(6, 1.0);  // thru
    note_on(arp, C, 77);

    const std::vector<int> got = block(arp, 0);
    expect(std::count(got.begin(), got.end(), 60) >= 1,
           "with thru on, the input note did not come out");

    Arp quiet;
    quiet.activate(kRate, kBlock);
    note_on(quiet, 30);  // below anything the figure would reach on its own
    const std::vector<int> muffled = block(quiet, 0);
    expect(std::count(muffled.begin(), muffled.end(), 30) == 1,
           "without thru, the input was passed on as well as arpeggiated");
  }

  // --- state survives a round trip -------------------------------------------
  {
    Arp arp;
    arp.activate(kRate, kBlock);
    arp.set_parameter(0, 1.0);
    arp.set_parameter(1, Arp::DownUp);
    arp.set_parameter(2, 3.0);
    arp.set_parameter(4, -7.0);
    arp.set_parameter(5, 1.0);

    Arp restored;
    restored.activate(kRate, kBlock);
    restored.load_state(arp.save_state());
    for (uint32_t id = 0; id <= 6; ++id)
      if (restored.parameter_value(id) != arp.parameter_value(id))
        fail("parameter " + std::to_string(id) + " did not survive the state");
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
