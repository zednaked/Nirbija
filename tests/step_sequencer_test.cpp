// Drives the step sequencer against a synthetic transport and reads back the
// MIDI it produced. No JACK, no plugins, no display — the whole point of the
// thing is arithmetic against transport position, and that is testable at a
// desk.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/step_sequencer.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 256;
constexpr double kTempo = 120.0;  // a beat is half a second, a 1/16 is 125 ms

struct Note {
  bool on;
  int pitch;
  int velocity;
  double beat;  // absolute song position where it landed
};

// Runs `blocks` blocks from beat zero and returns every note message, with the
// frame offset folded back into a song position so the assertions can talk in
// beats rather than in samples.
std::vector<Note> run(nirbija::StepSequencerInstance& seq, int blocks,
                      bool playing = true) {
  const double block_beats = kBlock / kRate * kTempo / 60.0;
  std::vector<Note> out;
  nirbija::MidiEvent buffer[64];

  for (int i = 0; i < blocks; ++i) {
    nirbija::TransportInfo transport;
    transport.playing = playing;
    transport.tempo_bpm = kTempo;
    transport.beats = i * block_beats;
    transport.changed = false;
    seq.set_transport(transport);
    seq.process(nullptr, nullptr, kBlock);

    const size_t count = seq.take_midi_output(buffer, 64);
    for (size_t e = 0; e < count; ++e) {
      const uint8_t status = buffer[e].data[0] & 0xf0;
      if (status != 0x90 && status != 0x80) continue;
      const bool on = status == 0x90 && buffer[e].data[2] > 0;
      out.push_back({on, buffer[e].data[1], buffer[e].data[2],
                     transport.beats + buffer[e].frame / kRate * kTempo / 60.0});
    }
  }
  return out;
}

void expect(bool condition, const std::string& what) {
  if (!condition) fail(what);
}

}  // namespace

int main() {
  const double block_beats = kBlock / kRate * kTempo / 60.0;

  // --- one note per active step, on the sixteenth ----------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    // Every step on, so the count is predictable.
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i)
      seq.set_parameter(80 + i, 1.0);

    // Four beats is a bar: at 1/16 that is sixteen steps.
    const int blocks = static_cast<int>(std::ceil(4.0 / block_beats));
    const std::vector<Note> notes = run(seq, blocks);

    int ons = 0;
    for (const Note& note : notes)
      if (note.on) ++ons;
    expect(ons == 16, "a bar of sixteenths should be sixteen notes, got " +
                          std::to_string(ons));

    // Each one lands on its own sixteenth. Half a block of tolerance: an event
    // can only be placed on a frame boundary.
    const double tolerance = block_beats;
    int index = 0;
    for (const Note& note : notes) {
      if (!note.on) continue;
      const double want = index * 0.25;
      if (std::abs(note.beat - want) > tolerance)
        fail("note " + std::to_string(index) + " landed on beat " +
             std::to_string(note.beat) + ", wanted " + std::to_string(want));
      ++index;
    }
  }

  // --- one at a time, and every note gets its off ----------------------------
  //
  // Full gate on purpose. At the default half gate a note ends in the middle
  // of its own step, so nothing ever overlaps and the check proves nothing —
  // it only bites when each step is still sounding as the next one starts.
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i)
      seq.set_parameter(80 + i, 1.0);
    seq.set_parameter(2, 1.0);

    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));

    int held = 0;
    int most = 0;
    for (const Note& note : notes) {
      held += note.on ? 1 : -1;
      most = std::max(most, held);
      if (held < 0) fail("a note-off arrived for a note that was not sounding");
    }
    expect(most <= 1, "the sequencer is monophonic, but " +
                          std::to_string(most) + " notes sounded at once");
  }

  // --- a step that is off stays silent ---------------------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i)
      seq.set_parameter(80 + i, 0.0);
    seq.set_parameter(80 + 0, 1.0);   // only the first
    seq.set_parameter(16 + 0, 64.0);  // on a note we can recognise

    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));

    int ons = 0;
    for (const Note& note : notes) {
      if (!note.on) continue;
      ++ons;
      if (note.pitch != 64) fail("a step that was off still played");
    }
    expect(ons == 1, "one step in a bar should sound once, got " +
                         std::to_string(ons));
  }

  // --- stopping releases what is held ----------------------------------------
  //
  // The failure this guards against is silent and horrible: a synth downstream
  // left ringing on a note whose off never came.
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i)
      seq.set_parameter(80 + i, 1.0);
    seq.set_parameter(2, 1.0);  // full gate, so a note is always sounding

    std::vector<Note> notes = run(seq, 4);
    int held = 0;
    for (const Note& note : notes) held += note.on ? 1 : -1;
    expect(held == 1, "a full gate should leave one note sounding");

    // Now the transport stops.
    nirbija::TransportInfo stopped;
    stopped.playing = false;
    stopped.tempo_bpm = kTempo;
    stopped.beats = 4 * block_beats;
    seq.set_transport(stopped);
    seq.process(nullptr, nullptr, kBlock);

    nirbija::MidiEvent buffer[64];
    const size_t count = seq.take_midi_output(buffer, 64);
    bool released = false;
    for (size_t e = 0; e < count; ++e) {
      const uint8_t status = buffer[e].data[0] & 0xf0;
      if (status == 0x80 || (status == 0x90 && buffer[e].data[2] == 0))
        released = true;
    }
    expect(released, "stopping the transport did not release the held note");
  }

  // --- the pattern wraps at its length ---------------------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i) {
      seq.set_parameter(80 + i, 1.0);
      seq.set_parameter(16 + i, 40.0 + i);  // each step a different pitch
    }
    seq.set_parameter(1, 4.0);  // four steps, then round again

    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(2.0 / block_beats)));

    std::vector<int> pitches;
    for (const Note& note : notes)
      if (note.on) pitches.push_back(note.pitch);

    if (pitches.size() < 8) {
      fail("expected at least eight notes in two beats of sixteenths");
    } else {
      for (size_t i = 0; i < 8; ++i) {
        const int want = 40 + static_cast<int>(i % 4);
        if (pitches[i] != want)
          fail("step " + std::to_string(i) + " played " +
               std::to_string(pitches[i]) + ", wanted " + std::to_string(want));
      }
    }
  }

  // --- a block boundary landing exactly on a step ----------------------------
  //
  // 6000 frames at 48 kHz and 120 bpm is a quarter of a beat, so every block
  // starts precisely on a sixteenth. That is the case where a step gets played
  // twice - once at the end of one block and again at the start of the next -
  // and the ordinary 256-frame blocks above never line up to catch it.
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, 6000);
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i)
      seq.set_parameter(80 + i, 1.0);

    nirbija::MidiEvent buffer[64];
    int ons = 0;
    for (int i = 0; i < 16; ++i) {  // sixteen blocks, so sixteen steps
      nirbija::TransportInfo transport;
      transport.playing = true;
      transport.tempo_bpm = kTempo;
      transport.beats = i * 0.25;
      seq.set_transport(transport);
      seq.process(nullptr, nullptr, 6000);

      const size_t count = seq.take_midi_output(buffer, 64);
      for (size_t e = 0; e < count; ++e)
        if ((buffer[e].data[0] & 0xf0) == 0x90 && buffer[e].data[2] > 0) ++ons;
    }
    expect(ons == 16, "blocks aligned to the step played " +
                          std::to_string(ons) + " notes, wanted 16");
  }

  // --- the same beat reported twice plays the step once ----------------------
  //
  // Song position is a double the host derives from a frame count, so two
  // blocks can arrive claiming the same beat, or the second can round back a
  // hair behind the first. Either way the step under it has already played,
  // and playing it again is a doubled note.
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, 6000);
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i)
      seq.set_parameter(80 + i, 1.0);

    nirbija::MidiEvent buffer[64];
    int ons = 0;
    // The same beat three times over, then a hair behind it.
    for (const double beat : {0.0, 0.0, 0.0, -1e-12}) {
      nirbija::TransportInfo transport;
      transport.playing = true;
      transport.tempo_bpm = kTempo;
      transport.beats = beat;
      seq.set_transport(transport);
      seq.process(nullptr, nullptr, 6000);

      const size_t count = seq.take_midi_output(buffer, 64);
      for (size_t e = 0; e < count; ++e)
        if ((buffer[e].data[0] & 0xf0) == 0x90 && buffer[e].data[2] > 0) ++ons;
    }
    expect(ons == 1, "a beat reported four times played " + std::to_string(ons) +
                         " notes, wanted 1");
  }

  // --- notes from earlier in the chain pass through ---------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i)
      seq.set_parameter(80 + i, 0.0);  // silent, so only the passthrough shows

    nirbija::MidiEvent incoming{};
    incoming.frame = 10;
    incoming.size = 3;
    incoming.data[0] = 0x90;
    incoming.data[1] = 72;
    incoming.data[2] = 90;
    seq.queue_midi(incoming);

    nirbija::TransportInfo transport;
    transport.playing = true;
    transport.tempo_bpm = kTempo;
    seq.set_transport(transport);
    seq.process(nullptr, nullptr, kBlock);

    nirbija::MidiEvent buffer[64];
    const size_t count = seq.take_midi_output(buffer, 64);
    bool passed = false;
    for (size_t e = 0; e < count; ++e)
      if (buffer[e].data[1] == 72 && buffer[e].frame == 10) passed = true;
    expect(passed, "a note from earlier in the chain did not pass through");
  }

  // --- state survives a round trip -------------------------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(0, 1.0);      // division 1/8
    seq.set_parameter(1, 7.0);      // seven steps
    seq.set_parameter(3, -5.0);     // transpose
    seq.set_parameter(4, 10.0);     // channel 10
    seq.set_parameter(16 + 3, 55);  // step 4 note
    seq.set_parameter(80 + 3, 1.0);

    const std::vector<uint8_t> blob = seq.save_state();

    nirbija::StepSequencerInstance restored;
    restored.activate(kRate, kBlock);
    if (!restored.load_state(blob)) fail("load_state refused its own blob");

    for (const auto& [id, what] :
         {std::pair<uint32_t, const char*>{0, "division"},
          {1, "length"},
          {3, "transpose"},
          {4, "channel"},
          {16 + 3, "a step's note"},
          {80 + 3, "a step's on/off"}}) {
      if (restored.parameter_value(id) != seq.parameter_value(id))
        fail(std::string(what) + " did not survive the state round trip");
    }

    // A truncated blob must give defaults back rather than throw.
    nirbija::StepSequencerInstance damaged;
    damaged.activate(kRate, kBlock);
    const std::vector<uint8_t> half(blob.begin(), blob.begin() + blob.size() / 2);
    damaged.load_state(half);
  }

  // --- swing leans the off-beats late ---------------------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i)
      seq.set_parameter(80 + i, 1.0);
    seq.set_parameter(5, 1.0);  // full swing: odd sixteenths +1/32

    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(1.0 / block_beats)));
    std::vector<double> beats;
    for (const Note& note : notes)
      if (note.on) beats.push_back(note.beat);
    if (beats.size() < 4) {
      fail("swing produced too few notes");
    } else {
      const double tol = block_beats;
      if (std::abs(beats[0] - 0.0) > tol)
        fail("swung step 0 left the grid");
      if (std::abs(beats[1] - 0.375) > tol)
        fail("swung step 1 landed on " + std::to_string(beats[1]) +
             ", wanted ~0.375");
    }
  }

  // --- reverse plays the last step first ------------------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i) {
      seq.set_parameter(80 + i, 1.0);
      seq.set_parameter(16 + i, 40.0 + i);
    }
    seq.set_parameter(6, 1.0);  // reverse
    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(1.0 / block_beats)));
    int first = -1;
    for (const Note& note : notes)
      if (note.on) {
        first = note.pitch;
        break;
      }
    expect(first == 55, "reverse started on " + std::to_string(first) +
                            ", wanted the last step (55)");
  }

  // --- pendulum turns without repeating the end -----------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(1, 4.0);
    for (int i = 0; i < 4; ++i) {
      seq.set_parameter(80 + i, 1.0);
      seq.set_parameter(16 + i, 40.0 + i);
    }
    seq.set_parameter(6, 2.0);  // pendulum
    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(1.5 / block_beats)));
    std::vector<int> pitches;
    for (const Note& note : notes)
      if (note.on) pitches.push_back(note.pitch);
    const int want[] = {40, 41, 42, 43, 42, 41};
    if (pitches.size() < 6) {
      fail("pendulum produced too few notes");
    } else {
      for (int i = 0; i < 6; ++i)
        if (pitches[i] != want[i])
          fail("pendulum step " + std::to_string(i) + " was " +
               std::to_string(pitches[i]));
    }
  }

  // --- probability 0 never fires --------------------------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i) {
      seq.set_parameter(80 + i, 1.0);
      seq.set_parameter(112 + i, 0.0);
    }
    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    int ons = 0;
    for (const Note& note : notes)
      if (note.on) ++ons;
    expect(ons == 0, "probability 0 still produced notes");
  }

  // --- accent lifts the velocity --------------------------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i)
      seq.set_parameter(80 + i, 0.0);
    seq.set_parameter(80 + 0, 1.0);
    seq.set_parameter(48 + 0, 100.0);
    seq.set_parameter(144 + 0, 1.0);
    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(1.0 / block_beats)));
    bool lifted = false;
    for (const Note& note : notes)
      if (note.on && note.velocity >= 120) lifted = true;
    expect(lifted, "accent did not raise the velocity");
  }

  // --- a tie holds the same pitch without retriggering ----------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i) {
      seq.set_parameter(80 + i, 1.0);
      seq.set_parameter(16 + i, 60.0);
      seq.set_parameter(176 + i, 1.0);
    }
    seq.set_parameter(2, 1.0);
    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    int ons = 0;
    for (const Note& note : notes)
      if (note.on) ++ons;
    expect(ons == 1, "a fully tied bar retriggered, got " +
                         std::to_string(ons) + " note-ons");
  }

  // --- euclid 8 in 16 is every other step -----------------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(14, 8.0);
    for (int i = 0; i < 16; ++i) {
      const bool on = seq.parameter_value(80 + i) >= 0.5;
      const bool want = (i % 2) == 0;
      if (on != want)
        fail("euclid 8/16 step " + std::to_string(i) +
             (on ? " was on" : " was off"));
    }
  }

  // --- nudge slides the pattern ---------------------------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(16 + 0, 50.0);
    seq.set_parameter(10, 1.0);  // nudge right
    if (seq.parameter_value(16 + 1) != 50.0)
      fail("nudge right did not move step 0 onto step 1");
  }

  // --- the metronome is not Play -------------------------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < nirbija::StepSequencerInstance::kSteps; ++i)
      seq.set_parameter(80 + i, 1.0);

    const double block_beats_local = kBlock / kRate * kTempo / 60.0;
    int ons = 0;
    nirbija::MidiEvent buffer[64];
    const int blocks = static_cast<int>(std::ceil(1.0 / block_beats_local));
    for (int i = 0; i < blocks; ++i) {
      nirbija::TransportInfo transport;
      transport.playing = false;
      transport.rolling = true;
      transport.tempo_bpm = kTempo;
      transport.beats = i * block_beats_local;
      seq.set_transport(transport);
      seq.process(nullptr, nullptr, kBlock);
      const size_t count = seq.take_midi_output(buffer, 64);
      for (size_t e = 0; e < count; ++e)
        if ((buffer[e].data[0] & 0xf0) == 0x90 && buffer[e].data[2] > 0) ++ons;
    }
    expect(ons == 0, "the metronome grid fired notes with Play off");
  }

  // --- new fields survive a round trip; an old blob still loads -------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(5, 0.4);
    seq.set_parameter(6, 2.0);
    seq.set_parameter(112 + 2, 0.25);
    seq.set_parameter(144 + 2, 1.0);
    seq.set_parameter(176 + 2, 1.0);
    const auto blob = seq.save_state();

    nirbija::StepSequencerInstance restored;
    restored.activate(kRate, kBlock);
    if (!restored.load_state(blob)) fail("new state blob was refused");
    if (std::fabs(restored.parameter_value(5) - 0.4) > 1e-4)
      fail("swing did not survive save");
    if (restored.parameter_value(6) != 2.0) fail("direction did not survive save");
    if (std::fabs(restored.parameter_value(112 + 2) - 0.25) > 1e-4)
      fail("probability did not survive save");
    if (restored.parameter_value(144 + 2) < 0.5) fail("accent did not survive save");
    if (restored.parameter_value(176 + 2) < 0.5) fail("tie did not survive save");

    const std::string old = "division 2\nlength 16\ngate 0.5000\ntranspose 0\n"
                            "channel 0\nstep 60 100 1\n";
    nirbija::StepSequencerInstance legacy;
    legacy.activate(kRate, kBlock);
    if (!legacy.load_state(std::vector<uint8_t>(old.begin(), old.end())))
      fail("a pre-swing state blob was refused");
    if (legacy.parameter_value(16 + 0) != 60.0)
      fail("legacy step note did not load");
    if (legacy.parameter_value(112 + 0) < 0.99)
      fail("a legacy step did not default to chance 1");
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
