// Feeds the chord plugin trigger keys and passthrough notes, reads back what
// it emits. Same shape as arpeggiator_test.cpp: no JACK, no display, no
// transport - this plugin is purely reactive to queue_midi.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "core/chord.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

void expect(bool condition, const std::string& what) {
  if (!condition) fail(what);
}

using Chord = nirbija::ChordInstance;

enum Param : uint32_t {
  kRoot = 0,
  kScale = 1,
  kSplit = 2,
  kOctave = 3,
  kInversion = 4,
  kVoices = 5,
  kSpread = 6,
  kPassthrough = 7,
  kChannel = 8,
};

void note_on(Chord& chord, int pitch, int velocity = 100, uint32_t frame = 0) {
  nirbija::MidiEvent event{};
  event.frame = frame;
  event.size = 3;
  event.data[0] = 0x90;
  event.data[1] = static_cast<uint8_t>(pitch);
  event.data[2] = static_cast<uint8_t>(velocity);
  chord.queue_midi(event);
}

void note_off(Chord& chord, int pitch, uint32_t frame = 0) {
  nirbija::MidiEvent event{};
  event.frame = frame;
  event.size = 3;
  event.data[0] = 0x80;
  event.data[1] = static_cast<uint8_t>(pitch);
  chord.queue_midi(event);
}

struct Out {
  std::vector<int> ons;
  std::vector<int> offs;
};

Out drain(Chord& chord) {
  nirbija::MidiEvent buffer[64];
  const size_t count = chord.take_midi_output(buffer, 64);
  Out out;
  for (size_t e = 0; e < count; ++e) {
    const uint8_t status = buffer[e].data[0] & 0xf0;
    if (status == 0x90 && buffer[e].data[2] > 0) out.ons.push_back(buffer[e].data[1]);
    else if (status == 0x80 || (status == 0x90 && buffer[e].data[2] == 0))
      out.offs.push_back(buffer[e].data[1]);
  }
  return out;
}

std::string show(const std::vector<int>& values) {
  std::string text;
  for (const int value : values) text += std::to_string(value) + " ";
  return text;
}

std::vector<int> sorted(std::vector<int> values) {
  std::sort(values.begin(), values.end());
  return values;
}

}  // namespace

int main() {
  // --- degree I of C major is a C major triad, whatever octave the trigger
  // key was struck in ------------------------------------------------------
  {
    Chord chord;
    chord.activate(48000.0, 512);
    note_on(chord, 60);  // C, below the default split of 60? split is *at*
                          // 60 -> trigger zone is strictly below it, so use 59
    const Out out = drain(chord);
    // 60 is the split itself: passthrough, unquantized -> just 60 back.
    expect(out.ons == std::vector<int>({60}),
           "note at the split played " + show(out.ons) + ", wanted passthrough 60");
  }
  {
    Chord chord;
    chord.activate(48000.0, 512);
    note_on(chord, 48);  // C, an octave below the split, still degree I
    const Out out = drain(chord);
    expect(sorted(out.ons) == std::vector<int>({60, 64, 67}),
           "degree I trigger at C3 played " + show(out.ons) + ", wanted C E G");
  }

  // --- degree ii is the diatonic minor triad on the second degree ------------
  {
    Chord chord;
    chord.activate(48000.0, 512);
    note_on(chord, 50);  // D, below split
    const Out out = drain(chord);
    expect(sorted(out.ons) == std::vector<int>({62, 65, 69}),
           "degree ii trigger played " + show(out.ons) + ", wanted D F A");
  }

  // --- a note the scale does not have stays silent, not the nearest guess ---
  {
    Chord chord;
    chord.activate(48000.0, 512);
    note_on(chord, 49);  // C#, not in C major
    const Out out = drain(chord);
    expect(out.ons.empty(), "an out-of-scale trigger played " + show(out.ons));
  }

  // --- letting go of the trigger key releases exactly the voices it started -
  {
    Chord chord;
    chord.activate(48000.0, 512);
    note_on(chord, 48);
    drain(chord);
    note_off(chord, 48);
    const Out out = drain(chord);
    expect(sorted(out.offs) == std::vector<int>({60, 64, 67}),
           "releasing the trigger key let go of " + show(out.offs) +
               ", wanted C E G");
  }

  // --- natural minor's tonic triad really is minor ----------------------------
  {
    Chord chord;
    chord.activate(48000.0, 512);
    chord.set_parameter(kScale, Chord::NaturalMinor);
    note_on(chord, 48);  // C, degree i in C natural minor
    const Out out = drain(chord);
    expect(sorted(out.ons) == std::vector<int>({60, 63, 67}),
           "C natural minor's i chord played " + show(out.ons) +
               ", wanted C Eb G");
  }

  // --- root transposes the whole degree map -----------------------------------
  {
    Chord chord;
    chord.activate(48000.0, 512);
    chord.set_parameter(kRoot, 7);  // G major
    note_on(chord, 55);  // G, degree I
    const Out out = drain(chord);
    expect(sorted(out.ons) == std::vector<int>({67, 71, 74}),
           "G major's I chord played " + show(out.ons) + ", wanted G B D");
  }

  // --- performance octave moves the chord, not the trigger key ---------------
  {
    Chord chord;
    chord.activate(48000.0, 512);
    chord.set_parameter(kOctave, 1);
    note_on(chord, 48);
    const Out out = drain(chord);
    expect(sorted(out.ons) == std::vector<int>({72, 76, 79}),
           "octave +1 played " + show(out.ons) + ", wanted C E G an octave up");
  }

  // --- inversion pushes the bottom voice(s) up an octave ----------------------
  {
    Chord chord;
    chord.activate(48000.0, 512);
    chord.set_parameter(kInversion, 1);
    note_on(chord, 48);
    const Out out = drain(chord);
    expect(sorted(out.ons) == std::vector<int>({64, 67, 72}),
           "first inversion played " + show(out.ons) + ", wanted E G C");
  }

  // --- four voices adds the diatonic seventh ----------------------------------
  {
    Chord chord;
    chord.activate(48000.0, 512);
    chord.set_parameter(kVoices, 4);
    note_on(chord, 48);
    const Out out = drain(chord);
    expect(sorted(out.ons) == std::vector<int>({60, 64, 67, 71}),
           "seventh chord played " + show(out.ons) + ", wanted C E G B");
  }

  // --- open spread lifts the second-lowest voice an octave --------------------
  {
    Chord chord;
    chord.activate(48000.0, 512);
    chord.set_parameter(kSpread, 1);
    note_on(chord, 48);
    const Out out = drain(chord);
    expect(sorted(out.ons) == std::vector<int>({60, 67, 76}),
           "open spread played " + show(out.ons) + ", wanted C G E-up-an-octave");
  }

  // --- above the split, a note passes through unchanged when quantize is off -
  {
    Chord chord;
    chord.activate(48000.0, 512);
    note_on(chord, 73, 88);  // C#5, above the default split
    const Out out = drain(chord);
    expect(out.ons == std::vector<int>({73}),
           "passthrough without quantize played " + show(out.ons));
  }

  // --- quantize pulls a passthrough note to the nearest scale tone -----------
  {
    Chord chord;
    chord.activate(48000.0, 512);
    chord.set_parameter(kPassthrough, 1);
    note_on(chord, 61);  // C#, above split, not in C major
    const Out out = drain(chord);
    expect(out.ons == std::vector<int>({62}),
           "quantized passthrough played " + show(out.ons) + ", wanted D (62)");

    note_off(chord, 61);
    const Out released = drain(chord);
    expect(released.offs == std::vector<int>({62}),
           "quantized passthrough note-off released " + show(released.offs) +
               ", wanted the quantized pitch back");
  }

  // --- moving the split with a key still down does not strand a note ---------
  {
    Chord chord;
    chord.activate(48000.0, 512);
    note_on(chord, 48);  // trigger, split still at the default 60
    drain(chord);

    chord.set_parameter(kSplit, 0);  // now 48 would read as passthrough
    note_off(chord, 48);
    const Out out = drain(chord);
    expect(sorted(out.offs) == std::vector<int>({60, 64, 67}),
           "moving the split mid-hold stranded " + show(out.offs) +
               " instead of releasing the chord");
  }

  // --- a retrigger without a note-off in between still releases cleanly ------
  {
    Chord chord;
    chord.activate(48000.0, 512);
    note_on(chord, 48);
    drain(chord);
    chord.set_parameter(kOctave, 1);  // next chord under the same key differs
    note_on(chord, 48);               // retrigger, no note-off sent first
    const Out out = drain(chord);
    expect(sorted(out.offs) == std::vector<int>({60, 64, 67}),
           "retrigger released " + show(out.offs) + ", wanted the old chord");
    expect(sorted(out.ons) == std::vector<int>({72, 76, 79}),
           "retrigger started " + show(out.ons) + ", wanted the new chord");

    note_off(chord, 48);
    const Out released = drain(chord);
    expect(sorted(released.offs) == std::vector<int>({72, 76, 79}),
           "final release let go of " + show(released.offs) +
               " instead of the retriggered chord");
  }

  // --- state survives a round trip --------------------------------------------
  {
    Chord chord;
    chord.activate(48000.0, 512);
    chord.set_parameter(kRoot, 5);
    chord.set_parameter(kScale, Chord::Dorian);
    chord.set_parameter(kSplit, 55);
    chord.set_parameter(kOctave, -2);
    chord.set_parameter(kInversion, 2);
    chord.set_parameter(kVoices, 4);
    chord.set_parameter(kSpread, 1);
    chord.set_parameter(kPassthrough, 1);
    chord.set_parameter(kChannel, 3);

    Chord restored;
    restored.activate(48000.0, 512);
    restored.load_state(chord.save_state());
    for (uint32_t id = kRoot; id <= kChannel; ++id)
      if (restored.parameter_value(id) != chord.parameter_value(id))
        fail("parameter " + std::to_string(id) + " did not survive the state");
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
