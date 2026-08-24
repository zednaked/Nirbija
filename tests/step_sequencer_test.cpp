// Drives the step sequencer against a synthetic transport and reads back the
// MIDI it produced. No JACK, no plugins, no display — the whole point of the
// thing is arithmetic against transport position, and that is testable at a
// desk.

#include <algorithm>
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
  int channel;
};

// Runs `blocks` blocks from beat zero and returns every note message, with the
// frame offset folded back into a song position so the assertions can talk in
// beats rather than in samples.
std::vector<Note> run(nirbija::StepSequencerInstance& seq, int blocks,
                      bool playing = true) {
  const double block_beats = kBlock / kRate * kTempo / 60.0;
  std::vector<Note> out;
  nirbija::MidiEvent buffer[128];

  for (int i = 0; i < blocks; ++i) {
    nirbija::TransportInfo transport;
    transport.playing = playing;
    transport.tempo_bpm = kTempo;
    transport.beats = i * block_beats;
    transport.changed = false;
    seq.set_transport(transport);
    seq.process(nullptr, nullptr, kBlock);

    const size_t count = seq.take_midi_output(buffer, 128);
    for (size_t e = 0; e < count; ++e) {
      const uint8_t status = buffer[e].data[0] & 0xf0;
      if (status != 0x90 && status != 0x80) continue;
      const bool on = status == 0x90 && buffer[e].data[2] > 0;
      out.push_back({on, buffer[e].data[1], buffer[e].data[2],
                     transport.beats + buffer[e].frame / kRate * kTempo / 60.0,
                     buffer[e].data[0] & 0x0f});
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
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i)
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
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i)
      seq.set_parameter(80 + i, 1.0);
    seq.set_parameter(2, 1.0);
    for (int lane = 1; lane < nirbija::StepSequencerInstance::kLanes; ++lane)
      seq.set_lane_mute(lane, true);

    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));

    int held = 0;
    int most = 0;
    for (const Note& note : notes) {
      held += note.on ? 1 : -1;
      most = std::max(most, held);
      if (held < 0) fail("a note-off arrived for a note that was not sounding");
    }
    expect(most <= 1, "a single active lane is monophonic, but " +
                          std::to_string(most) + " notes sounded at once");
  }

  // --- a step that is off stays silent ---------------------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i)
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
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i)
      seq.set_parameter(80 + i, 1.0);
    seq.set_parameter(2, 1.0);  // full gate, so a note is always sounding
    seq.set_lane_mute(1, false);
    seq.set_focus(1);
    seq.set_parameter(2, 1.0);
    seq.set_parameter(4, 10.0);  // wire channel 9
    seq.set_focus(0);
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i)
      seq.set_cell(0, 1, i, 80, 100, true, 1.0f);

    std::vector<Note> notes = run(seq, 4);
    int held = 0;
    for (const Note& note : notes) held += note.on ? 1 : -1;
    expect(held == 2, "full gate on two lanes should leave two notes sounding");

    // Now the transport stops.
    nirbija::TransportInfo stopped;
    stopped.playing = false;
    stopped.tempo_bpm = kTempo;
    stopped.beats = 4 * block_beats;
    seq.set_transport(stopped);
    seq.process(nullptr, nullptr, kBlock);

    nirbija::MidiEvent buffer[128];
    const size_t count = seq.take_midi_output(buffer, 128);
    bool released_ch0 = false;
    bool released_ch9 = false;
    for (size_t e = 0; e < count; ++e) {
      const uint8_t status = buffer[e].data[0] & 0xf0;
      const int channel = buffer[e].data[0] & 0x0f;
      if (status == 0x80 || (status == 0x90 && buffer[e].data[2] == 0)) {
        if (channel == 0) released_ch0 = true;
        if (channel == 9) released_ch9 = true;
      }
    }
    expect(released_ch0 && released_ch9,
           "stopping the transport did not release both held notes on their channels");
  }

  // --- the pattern wraps at its length ---------------------------------------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i) {
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
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i)
      seq.set_parameter(80 + i, 1.0);

    nirbija::MidiEvent buffer[128];
    int ons = 0;
    for (int i = 0; i < 16; ++i) {  // sixteen blocks, so sixteen steps
      nirbija::TransportInfo transport;
      transport.playing = true;
      transport.tempo_bpm = kTempo;
      transport.beats = i * 0.25;
      seq.set_transport(transport);
      seq.process(nullptr, nullptr, 6000);

      const size_t count = seq.take_midi_output(buffer, 128);
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
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i)
      seq.set_parameter(80 + i, 1.0);

    nirbija::MidiEvent buffer[128];
    int ons = 0;
    // The same beat three times over, then a hair behind it.
    for (const double beat : {0.0, 0.0, 0.0, -1e-12}) {
      nirbija::TransportInfo transport;
      transport.playing = true;
      transport.tempo_bpm = kTempo;
      transport.beats = beat;
      seq.set_transport(transport);
      seq.process(nullptr, nullptr, 6000);

      const size_t count = seq.take_midi_output(buffer, 128);
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
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i)
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

    nirbija::MidiEvent buffer[128];
    const size_t count = seq.take_midi_output(buffer, 128);
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
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i)
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
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i) {
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
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i) {
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
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i)
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
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i) {
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
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i)
      seq.set_parameter(80 + i, 1.0);

    const double block_beats_local = kBlock / kRate * kTempo / 60.0;
    int ons = 0;
    nirbija::MidiEvent buffer[128];
    const int blocks = static_cast<int>(std::ceil(1.0 / block_beats_local));
    for (int i = 0; i < blocks; ++i) {
      nirbija::TransportInfo transport;
      transport.playing = false;
      transport.rolling = true;
      transport.tempo_bpm = kTempo;
      transport.beats = i * block_beats_local;
      seq.set_transport(transport);
      seq.process(nullptr, nullptr, kBlock);
      const size_t count = seq.take_midi_output(buffer, 128);
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

  // --- Record snaps a live note to the nearest step, velocity and all ------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(15, 1.0);  // arm

    nirbija::TransportInfo transport;
    transport.playing = true;
    transport.tempo_bpm = kTempo;
    transport.beats = 0.0;
    seq.set_transport(transport);

    nirbija::MidiEvent on{};
    on.frame = 5;
    on.size = 3;
    on.data[0] = 0x90;
    on.data[1] = 66;
    on.data[2] = 77;
    seq.queue_midi(on);
    nirbija::MidiEvent off{};
    off.frame = 200;
    off.size = 3;
    off.data[0] = 0x80;
    off.data[1] = 66;
    seq.queue_midi(off);

    seq.process(nullptr, nullptr, kBlock);

    if (seq.parameter_value(16 + 0) != 66.0)
      fail("a captured note did not land on the nearest step");
    if (seq.parameter_value(48 + 0) != 77.0)
      fail("a captured note lost its velocity");
    if (seq.parameter_value(80 + 0) < 0.5)
      fail("a captured note did not arm its step");
    if (seq.parameter_value(176 + 0) >= 0.5)
      fail("a hit that never left its own step should not be tied");
  }

  // --- Record goes quiet itself; what you play still passes through --------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(15, 1.0);  // arm
    for (int i = 0; i < nirbija::StepSequencerInstance::kVisibleSteps; ++i)
      seq.set_parameter(80 + i, 1.0);  // every step on, so silence proves it

    nirbija::TransportInfo transport;
    transport.playing = true;
    transport.tempo_bpm = kTempo;
    transport.beats = 0.0;
    seq.set_transport(transport);

    nirbija::MidiEvent on{};
    on.frame = 5;
    on.size = 3;
    on.data[0] = 0x90;
    on.data[1] = 72;
    on.data[2] = 90;
    seq.queue_midi(on);

    seq.process(nullptr, nullptr, kBlock);

    nirbija::MidiEvent buffer[128];
    const size_t count = seq.take_midi_output(buffer, 128);
    int ons = 0;
    bool passed = false;
    for (size_t e = 0; e < count; ++e) {
      if ((buffer[e].data[0] & 0xf0) == 0x90 && buffer[e].data[2] > 0) ++ons;
      if (buffer[e].data[1] == 72 && buffer[e].frame == 5) passed = true;
    }
    expect(passed, "input did not pass through while Record was armed");
    expect(ons == 1, "the pattern itself sounded while Record was armed, got " +
                          std::to_string(ons) + " note-ons");
  }

  // --- a note held across steps ties through them, not just the first ------
  {
    nirbija::StepSequencerInstance seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(15, 1.0);  // arm

    const double block_beats_local = kBlock / kRate * kTempo / 60.0;
    const int blocks_per_step =
        static_cast<int>(std::ceil(0.25 / block_beats_local));
    const int release_block = blocks_per_step * 3;  // held through 3 steps

    nirbija::MidiEvent buffer[128];
    for (int i = 0; i <= release_block; ++i) {
      nirbija::TransportInfo transport;
      transport.playing = true;
      transport.tempo_bpm = kTempo;
      transport.beats = i * block_beats_local;
      seq.set_transport(transport);

      if (i == 0) {
        nirbija::MidiEvent on{};
        on.frame = 1;
        on.size = 3;
        on.data[0] = 0x90;
        on.data[1] = 50;
        on.data[2] = 100;
        seq.queue_midi(on);
      }
      if (i == release_block) {
        nirbija::MidiEvent off{};
        off.frame = 1;
        off.size = 3;
        off.data[0] = 0x80;
        off.data[1] = 50;
        seq.queue_midi(off);
      }

      seq.process(nullptr, nullptr, kBlock);
      seq.take_midi_output(buffer, 128);
    }

    for (int step = 0; step < 3; ++step) {
      if (seq.parameter_value(16 + step) != 50.0)
        fail("held note step " + std::to_string(step) + " lost its pitch");
      if (seq.parameter_value(80 + step) < 0.5)
        fail("held note step " + std::to_string(step) + " was not turned on");
      if (seq.parameter_value(176 + step) < 0.5)
        fail("held note step " + std::to_string(step) +
             " should have tied into the next");
    }
  }

  using Seq = nirbija::StepSequencerInstance;

  auto blob_of = [](const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
  };

  auto lanes_empty_muted = [](const Seq& seq, const std::string& who) {
    for (int lane = 1; lane < Seq::kLanes; ++lane) {
      if (!seq.lane_muted(lane))
        fail(who + " lane " + std::to_string(lane) + " was not muted");
      for (int i = 0; i < Seq::kMaxSteps; ++i) {
        if (seq.cell_active(0, lane, i)) {
          fail(who + " lane " + std::to_string(lane) + " step " +
               std::to_string(i) + " kept a factory hit");
          break;
        }
      }
    }
  };

  // --- constructor paints a muted kit; no audio needed ----------------------
  {
    Seq seq;
    const std::vector<uint8_t> blob = seq.save_state();
    const std::string text(blob.begin(), blob.end());
    if (text.find("version 2\n") != 0)
      fail("a fresh instance did not save as v2");
    if (!seq.lane_muted(1)) fail("constructor lane 1 was not muted");
    if (!seq.cell_active(0, 1, 4) || !seq.cell_active(0, 1, 12))
      fail("constructor snare did not land on steps 4 and 12");
    if (seq.cell_active(0, 1, 0)) fail("constructor snare had extra hits");
    if (seq.cell_note(0, 1, 4) != Seq::kUnlockedNote)
      fail("constructor kit cells were locked");
    if (seq.lane_note(1) != 38) fail("constructor snare was not GM 38");
    for (int i = 0; i < Seq::kVisibleSteps; ++i) {
      const bool want = (i % 2) == 0;
      if (seq.cell_active(0, 0, i) != want)
        fail("constructor lane 0 seed hit " + std::to_string(i));
      if (seq.cell_active(0, 2, i) != want)
        fail("constructor closed-hat hit " + std::to_string(i));
    }
    if (!seq.cell_active(0, 3, 14)) fail("constructor open-hat missed step 14");
    if (!seq.cell_active(0, 6, 4)) fail("constructor clap missed step 4");
    for (int lane : {4, 5, 7}) {
      for (int i = 0; i < Seq::kMaxSteps; ++i)
        if (seq.cell_active(0, lane, i))
          fail("constructor empty lane " + std::to_string(lane) + " had a hit");
    }
    for (int lane = 1; lane < Seq::kLanes; ++lane)
      if (!seq.lane_muted(lane))
        fail("constructor lane " + std::to_string(lane) + " was not muted");
    if (seq.lane_muted(0)) fail("constructor lane 0 was muted");
  }

  // --- v2 roundtrip preserves a cell the shim cannot reach ------------------
  {
    Seq seq;
    seq.set_cell(0, 3, 40, 48, 90, true, 0.25f);
    const std::vector<uint8_t> blob = seq.save_state();
    Seq restored;
    if (!restored.load_state(blob)) fail("v2 roundtrip blob was refused");
    if (restored.cell_note(0, 3, 40) != 48)
      fail("v2 roundtrip lost lane 3 step 40 note");
    if (restored.cell_velocity(0, 3, 40) != 90)
      fail("v2 roundtrip lost lane 3 step 40 velocity");
    if (!restored.cell_active(0, 3, 40))
      fail("v2 roundtrip lost lane 3 step 40 on");
    if (std::fabs(restored.cell_probability(0, 3, 40) - 0.25f) > 1e-4)
      fail("v2 roundtrip lost lane 3 step 40 probability");
  }

  // --- jam-goth v1 loads as lane 0; other lanes stay empty ------------------
  {
    const std::string kick =
        "division 2\nlength 16\ngate 0.2500\ntranspose 0\nchannel 0\n"
        "step 36 118 1\nstep 60 100 0\nstep 60 100 0\nstep 36 88 1\n"
        "step 36 118 1\nstep 60 100 0\nstep 60 100 0\nstep 60 100 0\n"
        "step 36 118 1\nstep 60 100 0\nstep 60 100 0\nstep 36 92 1\n"
        "step 36 118 1\nstep 60 100 0\nstep 60 100 0\nstep 60 100 0\n";
    const std::string snare =
        "division 2\nlength 16\ngate 0.2500\ntranspose 0\nchannel 0\n"
        "step 60 100 0\nstep 60 100 0\nstep 60 100 0\nstep 60 100 0\n"
        "step 38 120 1\nstep 60 100 0\nstep 60 100 0\nstep 37 62 1\n"
        "step 60 100 0\nstep 60 100 0\nstep 60 100 0\nstep 60 100 0\n"
        "step 38 120 1\nstep 60 100 0\nstep 60 100 0\nstep 37 70 1\n";
    const std::string hat =
        "division 2\nlength 16\ngate 0.2500\ntranspose 0\nchannel 0\n"
        "step 60 100 0\nstep 60 100 0\nstep 43 96 1\nstep 60 100 0\n"
        "step 60 100 0\nstep 60 100 0\nstep 41 104 1\nstep 60 100 0\n"
        "step 60 100 0\nstep 60 100 0\nstep 43 96 1\nstep 60 100 0\n"
        "step 60 100 0\nstep 60 100 0\nstep 41 110 1\nstep 60 100 0\n";
    const std::string perc =
        "division 2\nlength 16\ngate 0.2500\ntranspose 0\nchannel 0\n"
        "step 60 100 0\nstep 42 54 1\nstep 60 100 0\nstep 60 100 0\n"
        "step 60 100 0\nstep 42 58 1\nstep 60 100 0\nstep 60 100 0\n"
        "step 60 100 0\nstep 42 54 1\nstep 60 100 0\nstep 60 100 0\n"
        "step 60 100 0\nstep 42 58 1\nstep 60 100 0\nstep 46 72 1\n";

    const std::string blobs[] = {kick, snare, hat, perc};
    const char* names[] = {"kick", "snare", "hat", "perc"};
    for (int b = 0; b < 4; ++b) {
      Seq seq;
      if (!seq.load_state(blob_of(blobs[b])))
        fail(std::string("jam-goth ") + names[b] + " blob was refused");
      lanes_empty_muted(seq, std::string("jam-goth ") + names[b]);
    }

    Seq seq;
    seq.activate(kRate, kBlock);
    if (!seq.load_state(blob_of(kick))) fail("jam-goth kick blob was refused");
    const int kick_on[] = {0, 3, 4, 8, 11, 12};
    for (int i = 0; i < Seq::kVisibleSteps; ++i) {
      bool want = false;
      for (int k : kick_on)
        if (k == i) want = true;
      if (seq.cell_active(0, 0, i) != want)
        fail("jam-goth kick step " + std::to_string(i) +
             (want ? " was off" : " was on"));
      if (want && seq.cell_note(0, 0, i) != 36)
        fail("jam-goth kick step " + std::to_string(i) + " was not 36");
    }
    if (seq.cell_active(0, 1, 4))
      fail("jam-goth kick grew a constructor snare on lane 1");

    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    std::vector<int> pitches;
    for (const Note& note : notes)
      if (note.on) pitches.push_back(note.pitch);
    if (pitches.size() != 6)
      fail("jam-goth kick played " + std::to_string(pitches.size()) +
           " notes, wanted 6");
    for (int p : pitches)
      if (p != 36) fail("jam-goth kick played pitch " + std::to_string(p));
  }

  // --- a truncated v2 dump does not throw -----------------------------------
  {
    Seq seq;
    seq.set_cell(0, 3, 40, 48, 90, true, 0.25f);
    const std::vector<uint8_t> blob = seq.save_state();
    Seq damaged;
    const std::vector<uint8_t> half(blob.begin(),
                                    blob.begin() + blob.size() / 2);
    if (!damaged.load_state(half)) fail("truncated v2 blob was refused");
  }

  // --- inflated blobs give up without eating the constructor seed -----------
  {
    Seq seq;
    std::vector<uint8_t> junk(2 * 1024 * 1024, static_cast<uint8_t>('x'));
    if (!seq.load_state(junk)) fail("2 MiB junk blob was refused");
    if (!seq.cell_active(0, 1, 4) || !seq.cell_active(0, 1, 12))
      fail("2 MiB junk overwrote constructor defaults");
    if (seq.cell_note(0, 0, 0) != 57)
      fail("2 MiB junk overwrote the lane 0 seed");
  }

  {
    std::string text = "version 2\n";
    for (int p = 0; p < Seq::kPatterns; ++p)
      for (int l = 0; l < Seq::kLanes; ++l)
        for (int s = 0; s < Seq::kMaxSteps; ++s)
          text += "pstep " + std::to_string(p) + " " + std::to_string(l) + " " +
                  std::to_string(s) + " 10 100 0 1.0000 0 0 0.0000 1 0 0\n";
    for (int i = 0; i < 2000; ++i)
      text += "pstep 0 0 0 99 100 1 1.0000 0 0 0.0000 1 0 0\n";
    Seq seq;
    if (!seq.load_state(blob_of(text))) fail("10k pstep blob was refused");
    if (seq.cell_note(0, 0, 0) != 10)
      fail("pstep lines past 16*8*64 still wrote the first cell");
    if (seq.cell_active(0, 0, 0))
      fail("overflow pstep lines armed step 0");
  }

  {
    std::string text = "version 2\n";
    for (int i = 0; i < 9000; ++i) text += "junk 1\n";
    text += "pstep 0 0 0 77 100 1 1.0000 0 0 0.0000 1 0 0\n";
    Seq seq;
    if (!seq.load_state(blob_of(text))) fail("line-capped blob was refused");
    if (seq.cell_note(0, 0, 0) == 77)
      fail("a pstep past the line cap still landed");
  }

  // --- shim IDs 16–191 always write pattern 0 / lane 0 / steps 0–15 ---------
  {
    Seq seq;
    const std::string focused = "version 2\nfocus 3\n";
    if (!seq.load_state(blob_of(focused))) fail("focus blob was refused");
    seq.set_parameter(16, 64.0);
    if (seq.cell_note(0, 0, 0) != 64)
      fail("set_parameter(16) did not write lane 0 step 0");
    if (seq.parameter_value(16) != 64.0)
      fail("parameter_value(16) did not read the shim cell");
  }

  // --- polymeter: length 16 against length 12 --------------------------------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_lane_mute(1, false);
    for (int lane = 2; lane < Seq::kLanes; ++lane) seq.set_lane_mute(lane, true);
    for (int i = 0; i < 16; ++i)
      seq.set_cell(0, 0, i, 40 + i, 100, true, 1.0f);
    for (int i = 0; i < 12; ++i)
      seq.set_cell(0, 1, i, 80 + i, 100, true, 1.0f);
    seq.set_focus(1);
    seq.set_parameter(1, 12.0);
    seq.set_focus(0);

    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    int ons0 = 0, ons1 = 0;
    std::vector<int> pitches1;
    for (const Note& note : notes) {
      if (!note.on) continue;
      if (note.pitch >= 40 && note.pitch < 56) ++ons0;
      if (note.pitch >= 80 && note.pitch < 92) {
        ++ons1;
        pitches1.push_back(note.pitch);
      }
    }
    expect(ons0 == 16, "polymeter lane 0 played " + std::to_string(ons0) +
                           " notes, wanted 16");
    expect(ons1 == 16, "polymeter lane 1 played " + std::to_string(ons1) +
                           " notes (12+remainder), wanted 16");
    if (pitches1.size() >= 16) {
      for (int i = 0; i < 16; ++i) {
        const int want = 80 + (i % 12);
        if (pitches1[static_cast<size_t>(i)] != want)
          fail("polymeter lane 1 step " + std::to_string(i) + " was " +
               std::to_string(pitches1[static_cast<size_t>(i)]));
      }
    }
  }

  // --- poly across lanes; still monophonic per lane --------------------------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_lane_mute(1, false);
    for (int lane = 2; lane < Seq::kLanes; ++lane) seq.set_lane_mute(lane, true);
    seq.set_parameter(2, 1.0);
    seq.set_focus(1);
    seq.set_parameter(2, 1.0);
    seq.set_focus(0);
    for (int i = 0; i < Seq::kVisibleSteps; ++i) {
      seq.set_cell(0, 0, i, 40, 100, true, 1.0f);
      seq.set_cell(0, 1, i, 80, 100, true, 1.0f);
    }

    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    int held = 0, most = 0;
    int held0 = 0, most0 = 0, held1 = 0, most1 = 0;
    for (const Note& note : notes) {
      const int delta = note.on ? 1 : -1;
      held += delta;
      most = std::max(most, held);
      if (held < 0) fail("poly: a note-off arrived for a note that was not sounding");
      if (note.pitch == 40) {
        held0 += delta;
        most0 = std::max(most0, held0);
      } else if (note.pitch == 80) {
        held1 += delta;
        most1 = std::max(most1, held1);
      }
    }
    expect(most >= 2, "two lanes on at step 0 with full gate never overlapped");
    expect(most <= 8, "global poly exceeded 8 voices, got " + std::to_string(most));
    expect(most0 <= 1, "lane 0 stacked " + std::to_string(most0) + " notes");
    expect(most1 <= 1, "lane 1 stacked " + std::to_string(most1) + " notes");
  }

  // --- chase-off uses the channel the note went out on -----------------------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    for (int i = 0; i < Seq::kVisibleSteps; ++i) seq.set_parameter(80 + i, 1.0);
    seq.set_parameter(2, 1.0);
    for (int lane = 1; lane < Seq::kLanes; ++lane) seq.set_lane_mute(lane, true);
    run(seq, 4);
    seq.set_parameter(4, 10.0);  // live lane now wire channel 9

    nirbija::TransportInfo stopped;
    stopped.playing = false;
    stopped.tempo_bpm = kTempo;
    stopped.beats = 4 * block_beats;
    seq.set_transport(stopped);
    seq.process(nullptr, nullptr, kBlock);

    nirbija::MidiEvent buffer[128];
    const size_t count = seq.take_midi_output(buffer, 128);
    bool off_on_stored = false;
    bool off_on_live = false;
    for (size_t e = 0; e < count; ++e) {
      const uint8_t status = buffer[e].data[0] & 0xf0;
      const int channel = buffer[e].data[0] & 0x0f;
      if (status != 0x80 && !(status == 0x90 && buffer[e].data[2] == 0)) continue;
      if (channel == 0) off_on_stored = true;
      if (channel == 9) off_on_live = true;
    }
    expect(off_on_stored, "chase-off did not use the stored channel");
    expect(!off_on_live, "chase-off followed the live lane channel");
  }

  // --- mute silences emit, not the light -------------------------------------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_lane_mute(1, false);
    for (int i = 0; i < Seq::kVisibleSteps; ++i)
      seq.set_cell(0, 1, i, 90, 100, true, 1.0f);
    seq.set_lane_mute(1, true);
    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    int ons90 = 0;
    for (const Note& note : notes)
      if (note.on && note.pitch == 90) ++ons90;
    expect(ons90 == 0, "muted lane 1 still emitted, got " + std::to_string(ons90));
  }

  // --- unmute of lane 1 sounds the factory snare -----------------------------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_lane_mute(1, false);
    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    int snares = 0;
    for (const Note& note : notes)
      if (note.on && note.pitch == 38) ++snares;
    expect(snares == 2, "unmuted lane 1 snare played " + std::to_string(snares) +
                            " times, wanted 2 (steps 4 and 12)");
  }

  // --- typed setters: out of range is a no-op, euclid stays a bang -----------
  {
    Seq seq;
    seq.set_cell(0, 0, 0, 40, 80, true, 0.5f, true, true);
    expect(seq.cell_note(0, 0, 0) == 40, "set_cell did not store the note");
    expect(seq.cell_accent(0, 0, 0), "set_cell did not store accent");
    expect(seq.cell_tie(0, 0, 0), "set_cell did not store tie");
    seq.set_cell(-1, 0, 0, 99, 1, true, 1.0f, false, false);
    expect(seq.cell_note(0, 0, 0) == 40, "out of range set_cell wrote a cell");

    seq.set_trig(0, 0, 0, 0.25f, 4, 3, 2);
    expect(std::fabs(seq.cell_microtiming(0, 0, 0) - 0.25f) < 1e-4,
           "set_trig did not store microtiming");
    expect(seq.cell_ratchet(0, 0, 0) == 4, "set_trig did not store ratchet");
    expect(seq.cell_condition(0, 0, 0) == 3, "set_trig did not store cond");
    seq.set_trig(99, 0, 0, 0.0f, 1, 0, 0);
    expect(seq.cell_ratchet(0, 0, 0) == 4, "out of range set_trig wrote a cell");

    seq.set_lane_euclid(0, 8);
    const int euclid = seq.lane_euclid(0);
    seq.set_lane(0, 50, 12, 1, 2, 3, true, 0.75);
    expect(seq.lane_note(0) == 50, "set_lane did not store the note");
    expect(seq.lane_length(0) == 12, "set_lane did not store length");
    expect(seq.lane_channel(0) == 3, "set_lane did not store channel");
    expect(seq.lane_muted(0), "set_lane did not store mute");
    expect(seq.lane_euclid(0) == euclid, "set_lane rewrote euclid");

    seq.set_focus(0);
    seq.set_lane_euclid(3, 5);
    expect(seq.lane_euclid(3) == 5, "set_lane_euclid missed lane 3");
    expect(seq.cell_active(0, 3, 0), "euclid 5 on length 16 missed step 0");

    seq.set_extra_head(1, 2, 3, 1, 4, 8, -5, false);
    expect(seq.extra_head_lane(1) == 2, "set_extra_head did not store lane");
    expect(seq.extra_head_rate(1) == 3, "set_extra_head did not store rate");
    expect(seq.extra_head_transpose(1) == -5,
           "set_extra_head did not store transpose");
    expect(!seq.extra_head_muted(1), "set_extra_head did not store mute");
    seq.set_extra_head(99, 0, 0, 0, 0, 16, 0, true);
    expect(seq.extra_head_lane(1) == 2, "out of range set_extra_head wrote");
  }

  // --- macros 192-200: round trip, and out of the shim's way -----------------
  {
    Seq seq;
    seq.set_parameter(192, 1.5);
    expect(std::fabs(seq.parameter_value(192) - 1.5) < 1e-4,
           "density did not round-trip");
    seq.set_parameter(193, 0.5);
    expect(std::fabs(seq.parameter_value(193) - 0.5) < 1e-4,
           "chaos did not round-trip");
    seq.set_parameter(194, 0.25);
    expect(std::fabs(seq.parameter_value(194) - 0.25) < 1e-4,
           "ratchet amount did not round-trip");
    seq.set_parameter(195, 0.75);
    expect(std::fabs(seq.parameter_value(195) - 0.75) < 1e-4,
           "master probability did not round-trip");
    seq.set_parameter(196, 5.0);
    expect(seq.parameter_value(196) == 5.0, "pattern did not round-trip");
    seq.set_parameter(198, 1.0);
    expect(seq.parameter_value(198) == 1.0, "fill did not round-trip");
    seq.set_parameter(199, 3.0);
    expect(seq.parameter_value(199) == 3.0 && seq.focus() == 3,
           "focused lane did not round-trip");
    seq.set_parameter(200, 1.0);
    expect(seq.parameter_value(200) == 1.0, "view did not round-trip");

    bool has_shim = false, has_density = false, has_view = false;
    for (const nirbija::ParameterInfo& info : seq.parameters()) {
      if (info.id >= 16 && info.id < 192) has_shim = true;
      if (info.id == 192) has_density = true;
      if (info.id == 200) has_view = true;
    }
    expect(!has_shim, "parameters() still lists the dead per-step shim");
    expect(has_density, "parameters() is missing density");
    expect(has_view, "parameters() is missing view");
    // Unlisted does not mean unreachable: an old MIDI learn map still works.
    seq.set_parameter(16, 70.0);
    expect(seq.parameter_value(16) == 70.0, "the unlisted shim stopped working");
  }

  // --- Density above 1 spends its excess on ghost notes at off steps --------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(13, 1.0);    // clear hits: every step off
    seq.set_parameter(192, 2.0);   // density maxed: ghost chance is certain

    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    int ons = 0;
    for (const Note& note : notes) if (note.on) ++ons;
    expect(ons == 16, "density ghosts did not fill every off step, got " +
                          std::to_string(ons));
    if (!notes.empty())
      expect(notes.front().velocity == 70,
             "a ghost note did not come in at 70% velocity, got " +
                 std::to_string(notes.front().velocity));
  }

  // --- master probability is a hard ceiling on top of per-step chance -------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(195, 0.0);  // master probability zero

    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    int ons = 0;
    for (const Note& note : notes) if (note.on) ++ons;
    expect(ons == 0, "master probability 0 did not silence the pattern, got " +
                          std::to_string(ons));
  }

  // --- Record lands on the focused lane, and does not lock a kit row --------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_focus(1);
    seq.set_parameter(15, 1.0);  // arm

    nirbija::TransportInfo transport;
    transport.playing = true;
    transport.tempo_bpm = kTempo;
    transport.beats = 0.0;
    seq.set_transport(transport);

    nirbija::MidiEvent on{};
    on.frame = 5;
    on.size = 3;
    on.data[0] = 0x90;
    on.data[1] = 66;
    on.data[2] = 90;
    seq.queue_midi(on);
    nirbija::MidiEvent off{};
    off.frame = 200;
    off.size = 3;
    off.data[0] = 0x80;
    off.data[1] = 66;
    seq.queue_midi(off);

    seq.process(nullptr, nullptr, kBlock);

    expect(seq.cell_active(0, 1, 0), "Rec did not land on the focused lane");
    expect(seq.cell_note(0, 0, 0) == 57,
           "Rec leaked into lane 0's factory pattern while lane 1 was focused");
    expect(seq.cell_note(0, 1, 0) == Seq::kUnlockedNote,
           "Rec locked a fresh kit row's pitch");
    expect(seq.cell_velocity(0, 1, 0) == 90,
           "Rec on an unlocked cell lost the played velocity");
  }

  // --- a held note across steps keeps the lock decision it opened with ------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_focus(2);
    seq.set_parameter(15, 1.0);  // arm

    const double block_beats_local = kBlock / kRate * kTempo / 60.0;
    const int blocks_per_step =
        static_cast<int>(std::ceil(0.25 / block_beats_local));
    const int release_block = blocks_per_step * 2;  // held through 2 steps

    nirbija::MidiEvent buffer[128];
    for (int i = 0; i <= release_block; ++i) {
      nirbija::TransportInfo transport;
      transport.playing = true;
      transport.tempo_bpm = kTempo;
      transport.beats = i * block_beats_local;
      seq.set_transport(transport);

      if (i == 0) {
        nirbija::MidiEvent on{};
        on.frame = 1;
        on.size = 3;
        on.data[0] = 0x90;
        on.data[1] = 71;
        on.data[2] = 100;
        seq.queue_midi(on);
      }
      if (i == release_block) {
        nirbija::MidiEvent off{};
        off.frame = 1;
        off.size = 3;
        off.data[0] = 0x80;
        off.data[1] = 71;
        seq.queue_midi(off);
      }

      seq.process(nullptr, nullptr, kBlock);
      seq.take_midi_output(buffer, 128);
    }

    for (int step = 0; step < 2; ++step) {
      if (!seq.cell_active(0, 2, step))
        fail("held note step " + std::to_string(step) + " was not armed");
      if (seq.cell_note(0, 2, step) != Seq::kUnlockedNote)
        fail("held note step " + std::to_string(step) +
             " locked a kit row that started unlocked");
      if (step < 1 && !seq.cell_tie(0, 2, step))
        fail("held note step " + std::to_string(step) +
             " should have tied into the next");
    }
  }

  // --- pattern bank: audio follows pattern_, isolation between banks --------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(13, 1.0);  // clear lane 0 pattern 0
    seq.set_cell(1, 0, 0, 72, 100, true, 1.0f);
    seq.set_parameter(196, 1.0);
    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    int ons = 0;
    int pitch = -1;
    for (const Note& note : notes) {
      if (!note.on) continue;
      ++ons;
      pitch = note.pitch;
    }
    expect(ons == 1, "pattern 1 should sound once per bar, got " +
                         std::to_string(ons));
    expect(pitch == 72, "pattern 1 did not play its own pitch");
    expect(seq.cell_active(0, 0, 0) == false,
           "switching pattern rewrote pattern 0");
  }

  // --- FILL / NotFill -------------------------------------------------------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(13, 1.0);
    seq.set_cell(0, 0, 0, 60, 100, true, 1.0f);
    seq.set_trig(0, 0, 0, 0.0f, 1, Seq::Fill, 0);
    auto silent = run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    int ons = 0;
    for (const Note& note : silent) if (note.on) ++ons;
    expect(ons == 0, "Fill condition sounded with FILL off");
  }
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(13, 1.0);
    seq.set_cell(0, 0, 0, 60, 100, true, 1.0f);
    seq.set_trig(0, 0, 0, 0.0f, 1, Seq::Fill, 0);
    seq.set_parameter(198, 1.0);
    auto loud = run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    int ons = 0;
    for (const Note& note : loud) if (note.on) ++ons;
    expect(ons == 1, "Fill condition stayed silent with FILL on, got " +
                         std::to_string(ons));
  }

  // --- NEI miss: neighbor silent at this index stays silent next cycle ------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_lane_mute(1, false);
    seq.set_parameter(13, 1.0);  // clear focused (0)
    seq.set_focus(1);
    seq.set_parameter(13, 1.0);  // clear lane 1
    seq.set_cell(0, 1, 0, 62, 100, true, 1.0f);
    seq.set_trig(0, 1, 0, 0.0f, 1, Seq::Nei, 0);
    // Lane 0 step 0 is off, so Nei on lane 1 step 0 must miss after one bar
    // of visits. First bar writes last_on[0][0]=false; second bar Nei reads it.
    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(8.0 / block_beats)));
    int ons62 = 0;
    for (const Note& note : notes)
      if (note.on && note.pitch == 62) ++ons62;
    expect(ons62 == 0, "Nei fired when the neighbor had missed that index");
  }

  // --- A:B 1:2 fires every other cycle of the lane --------------------------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(13, 1.0);
    seq.set_cell(0, 0, 0, 64, 100, true, 1.0f);
    seq.set_trig(0, 0, 0, 0.0f, 1, Seq::AOverB, (1 << 4) | 2);
    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(16.0 / block_beats)));
    int ons = 0;
    for (const Note& note : notes) if (note.on) ++ons;
    expect(ons == 2, "1:2 over four bars should fire twice, got " +
                         std::to_string(ons));
  }

  // --- ratchet 4, gate=1, 256-frame blocks: ons on the step/N grid ----------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(13, 1.0);
    seq.set_parameter(2, 1.0);  // full gate
    seq.set_parameter(194, 1.0);  // ratchet macro fully on
    seq.set_cell(0, 0, 0, 60, 100, true, 1.0f);
    seq.set_trig(0, 0, 0, 0.0f, 4, Seq::Always, 0);
    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(1.0 / block_beats)));
    std::vector<double> ons;
    for (const Note& note : notes)
      if (note.on) ons.push_back(note.beat);
    expect(ons.size() >= 4, "ratchet 4 should emit four ons, got " +
                                std::to_string(ons.size()));
    if (ons.size() >= 4) {
      const double tol = block_beats;
      expect(std::fabs(ons[0] - 0.0) < tol, "ratchet pulse 0 missed the grid");
      expect(std::fabs(ons[1] - 0.0625) < tol,
             "ratchet pulse 1 missed 0.0625");
      expect(std::fabs(ons[2] - 0.125) < tol, "ratchet pulse 2 missed 0.125");
      expect(std::fabs(ons[3] - 0.1875) < tol,
             "ratchet pulse 3 missed 0.1875");
    }
  }

  // --- a ratchet survives its own gate closing between pulses ----------------
  //
  // Gate under 1.0 means the note-off between two ratchet pulses fires
  // before the next pulse's time comes around — completely normal, the
  // same silence gate leaves between two ordinary steps. That used to look
  // exactly like the voice having been cancelled from outside, so every
  // ratchet but the very first pulse quietly died the moment gate left its
  // default of 0.5. No lane in this file touches gate, so this is the
  // lane's own factory setting, not a contrived edge case.
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(13, 1.0);
    seq.set_parameter(194, 1.0);  // ratchet macro fully on; gate stays 0.5
    seq.set_cell(0, 0, 0, 60, 100, true, 1.0f);
    seq.set_trig(0, 0, 0, 0.0f, 4, Seq::Always, 0);
    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(1.0 / block_beats)));
    std::vector<double> ons;
    for (const Note& note : notes)
      if (note.on) ons.push_back(note.beat);
    expect(ons.size() >= 4,
           "a ratchet at the lane's default gate should still emit four "
           "ons, got " +
               std::to_string(ons.size()));
    if (ons.size() >= 4) {
      const double tol = block_beats;
      expect(std::fabs(ons[1] - 0.0625) < tol,
             "ratchet pulse 1 missed 0.0625 with gate under 1.0");
      expect(std::fabs(ons[2] - 0.125) < tol,
             "ratchet pulse 2 missed 0.125 with gate under 1.0");
      expect(std::fabs(ons[3] - 0.1875) < tol,
             "ratchet pulse 3 missed 0.1875 with gate under 1.0");
    }
  }

  // --- ratchet 8 in a 1024-frame block includes pulse 2 in the same block ---
  {
    Seq seq;
    seq.activate(kRate, 1024);
    seq.set_parameter(13, 1.0);
    seq.set_parameter(2, 1.0);
    seq.set_parameter(194, 1.0);
    seq.set_cell(0, 0, 0, 60, 100, true, 1.0f);
    seq.set_trig(0, 0, 0, 0.0f, 8, Seq::Always, 0);

    nirbija::TransportInfo transport;
    transport.playing = true;
    transport.tempo_bpm = kTempo;
    transport.beats = 0.0;
    seq.set_transport(transport);
    seq.process(nullptr, nullptr, 1024);
    nirbija::MidiEvent buffer[128];
    const size_t count = seq.take_midi_output(buffer, 128);
    int ons = 0;
    for (size_t e = 0; e < count; ++e)
      if ((buffer[e].data[0] & 0xf0) == 0x90 && buffer[e].data[2] > 0) ++ons;
    expect(ons >= 2, "1024-frame ratchet 8 should contain trig + pulse 2, got " +
                         std::to_string(ons));
  }

  // --- mute before pulse 2 silences the rest of the ratchet -----------------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(13, 1.0);
    seq.set_parameter(2, 1.0);
    seq.set_parameter(194, 1.0);
    seq.set_cell(0, 0, 0, 60, 100, true, 1.0f);
    seq.set_trig(0, 0, 0, 0.0f, 8, Seq::Always, 0);

    nirbija::TransportInfo transport;
    transport.playing = true;
    transport.tempo_bpm = kTempo;
    transport.beats = 0.0;
    seq.set_transport(transport);
    seq.process(nullptr, nullptr, kBlock);
    nirbija::MidiEvent buffer[128];
    seq.take_midi_output(buffer, 128);
    seq.set_lane_mute(0, true);
    transport.beats = kBlock / kRate * kTempo / 60.0;
    seq.set_transport(transport);
    seq.process(nullptr, nullptr, kBlock);
    const size_t count = seq.take_midi_output(buffer, 128);
    int ons = 0;
    for (size_t e = 0; e < count; ++e)
      if ((buffer[e].data[0] & 0xf0) == 0x90 && buffer[e].data[2] > 0) ++ons;
    expect(ons == 0, "mute during a ratchet still emitted, got " +
                         std::to_string(ons));
  }

  // --- extra head rate×2 doubles ons; muted extra is silent -----------------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(13, 1.0);
    for (int i = 0; i < Seq::kVisibleSteps; ++i)
      seq.set_cell(0, 0, i, 60, 100, true, 1.0f);
    seq.set_extra_head(0, 0, 1, Seq::Forward, 0, 16, 12, false);
    const std::vector<Note> notes =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    int native = 0, extra = 0;
    for (const Note& note : notes) {
      if (!note.on) continue;
      if (note.pitch == 60) ++native;
      if (note.pitch == 72) ++extra;
    }
    expect(native == 16, "native head lost ons with an extra running, got " +
                             std::to_string(native));
    expect(extra == 32, "rate×2 extra should double the ons, got " +
                            std::to_string(extra));

    seq.set_extra_head(0, 0, 1, Seq::Forward, 0, 16, 12, true);
    const std::vector<Note> quiet =
        run(seq, static_cast<int>(std::ceil(4.0 / block_beats)));
    extra = 0;
    for (const Note& note : quiet)
      if (note.on && note.pitch == 72) ++extra;
    expect(extra == 0, "muted extra still sounded");
  }

  // --- extra reverse window start=48 length=16 never indexes 64+ ------------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(1, 64.0);  // lane 0 length 64
    for (int i = 0; i < 64; ++i)
      seq.set_cell(0, 0, i, 40 + i, 100, true, 1.0f);
    seq.set_extra_head(0, 0, 0, Seq::Reverse, 48, 16, 0, false);
    nirbija::TransportInfo transport;
    transport.playing = true;
    transport.tempo_bpm = kTempo;
    transport.beats = 0.0;
    seq.set_transport(transport);
    seq.process(nullptr, nullptr, kBlock);
    expect(seq.extra_head_step(0) == 63,
           "reverse window first step was not 63, got " +
               std::to_string(seq.extra_head_step(0)));
  }

  // --- next pattern queues on the host bar ----------------------------------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(13, 1.0);
    seq.set_cell(0, 0, 0, 60, 100, true, 1.0f);
    seq.set_cell(1, 0, 0, 72, 100, true, 1.0f);
    seq.set_parameter(197, 2.0);  // next = pattern 1
    const int blocks = static_cast<int>(std::ceil(8.0 / block_beats));
    const std::vector<Note> notes = run(seq, blocks);
    bool heard60 = false, heard72 = false;
    for (const Note& note : notes) {
      if (!note.on) continue;
      if (note.pitch == 60 && note.beat < 4.0) heard60 = true;
      if (note.pitch == 72 && note.beat >= 4.0) heard72 = true;
    }
    expect(heard60, "first bar did not play pattern 0");
    expect(heard72, "second bar did not switch to the queued pattern");
    expect(seq.pattern() == 1, "queued pattern did not become current");
    expect(seq.next_pattern() < 0, "next pattern did not clear after the bar");
  }

  // --- Rec routes a pad pitch onto that lane, unlocked stays unlocked -------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    seq.set_parameter(15, 1.0);
    nirbija::TransportInfo transport;
    transport.playing = true;
    transport.tempo_bpm = kTempo;
    transport.beats = 0.0;
    seq.set_transport(transport);
    nirbija::MidiEvent on{};
    on.frame = 5;
    on.size = 3;
    on.data[0] = 0x90;
    on.data[1] = 38;  // GM snare = lane 1
    on.data[2] = 111;
    seq.queue_midi(on);
    seq.process(nullptr, nullptr, kBlock);
    nirbija::MidiEvent buffer[128];
    seq.take_midi_output(buffer, 128);
    expect(seq.cell_active(0, 1, 0), "snare rec did not land on lane 1");
    expect(seq.cell_note(0, 1, 0) == Seq::kUnlockedNote,
           "snare rec locked the kit row");
    expect(seq.cell_velocity(0, 1, 0) == 111, "snare rec lost velocity");
  }

  // --- stopping stores playhead -1, not step 0 ------------------------------
  {
    Seq seq;
    seq.activate(kRate, kBlock);
    run(seq, 4);
    expect(seq.playhead() >= 0, "playing should have a playhead");
    nirbija::TransportInfo stopped;
    stopped.playing = false;
    stopped.tempo_bpm = kTempo;
    seq.set_transport(stopped);
    seq.process(nullptr, nullptr, kBlock);
    expect(seq.playhead() == -1, "stop did not clear playhead");
    expect(seq.native_head_step(0) < 0,
           "stop left native head_steps at 0, lighting column 1");
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
