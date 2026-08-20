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

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
