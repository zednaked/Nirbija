// Drives the looper the way a player would: record a phrase, close the loop,
// hear it repeat, overdub on top, clear it away.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/looper.h"

namespace {

constexpr uint32_t kBlock = 256;
constexpr double kRate = 48000.0;

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

// Runs one block. `level` is the input fed in; the return is the output peak.
float run_block(nirbija::LooperInstance& looper, float level,
                double tempo = 120.0, bool playing = true) {
  std::vector<float> in_l(kBlock, level), in_r(kBlock, level);
  std::vector<float> out_l(kBlock), out_r(kBlock);
  const float* ins[2] = {in_l.data(), in_r.data()};
  float* outs[2] = {out_l.data(), out_r.data()};

  nirbija::TransportInfo transport;
  transport.playing = playing;
  transport.tempo_bpm = tempo;
  transport.numerator = 4;
  looper.set_transport(transport);
  looper.process(ins, outs, kBlock);

  float peak = 0.0f;
  for (float sample : out_l) peak = std::max(peak, std::fabs(sample));
  return peak;
}

}  // namespace

int main() {
  nirbija::LooperInstance looper;
  looper.set_channel_layout(2);
  looper.activate(kRate, kBlock);
  looper.set_parameter(3, 0.0);  // quantise off: the test is its own clock

  // Live input passes through even with nothing recorded.
  if (run_block(looper, 0.5f) < 0.45f) fail("input does not pass through");

  // Record 8 blocks of tone, then close the loop.
  looper.set_parameter(0, 1.0);
  for (int i = 0; i < 8; ++i) run_block(looper, 0.5f);
  looper.set_parameter(0, 0.0);

  // Input now silent: whatever comes out is the loop.
  float loop_peak = 0.0f;
  for (int i = 0; i < 16; ++i) loop_peak = std::max(loop_peak, run_block(looper, 0.0f));
  if (loop_peak < 0.45f) fail("the closed loop does not play back");

  // Overdub doubles the layer.
  looper.set_parameter(0, 1.0);
  for (int i = 0; i < 8; ++i) run_block(looper, 0.5f);
  looper.set_parameter(0, 0.0);
  float dubbed = 0.0f;
  for (int i = 0; i < 16; ++i) dubbed = std::max(dubbed, run_block(looper, 0.0f));
  if (dubbed < loop_peak + 0.3f) fail("overdub did not add a layer");

  // Play off mutes the loop without touching the live input.
  looper.set_parameter(1, 0.0);
  if (run_block(looper, 0.0f) > 1e-6f) fail("play off still sounds the loop");
  if (run_block(looper, 0.5f) < 0.45f) fail("play off swallowed the live input");
  looper.set_parameter(1, 1.0);

  // Clear empties it and drops Rec, or the next block starts a new take
  // on the empty tape and the button stays lit.
  looper.set_parameter(0, 1.0);
  run_block(looper, 0.0f);
  if (!looper.recording()) fail("rec did not arm before clear");
  looper.set_parameter(2, 1.0);
  float cleared = 0.0f;
  for (int i = 0; i < 8; ++i) cleared = std::max(cleared, run_block(looper, 0.0f));
  if (cleared > 1e-6f) fail("clear left audio behind");
  if (looper.recording()) fail("clear left rec armed");
  if (looper.has_audio()) fail("clear left rec running on the empty tape");

  // --- trim and fade -----------------------------------------------------
  // A fresh phrase: loud for the first half, silent for the second, so trim
  // can be told which half survived by ear (well, by peak).
  looper.set_parameter(0, 1.0);
  for (int i = 0; i < 8; ++i) run_block(looper, 0.8f);
  for (int i = 0; i < 8; ++i) run_block(looper, 0.0f);
  looper.set_parameter(0, 0.0);
  // Closing the loop is a state change apply_requests() makes inside
  // process(), not something set_parameter() does on the spot - one more
  // block is what actually defines length_.
  run_block(looper, 0.0f);

  if (looper.waveform(4).empty()) fail("waveform() returned nothing for a closed loop");
  bool waveform_has_signal = false;
  for (float peak : looper.waveform(8)) waveform_has_signal |= peak > 0.1f;
  if (!waveform_has_signal) fail("waveform() came back silent for a loud loop");

  looper.set_trim(0.5, 1.0);  // keep only the silent half
  float silent_half = 0.0f;
  for (int i = 0; i < 16; ++i) silent_half = std::max(silent_half, run_block(looper, 0.0f));
  if (silent_half > 1e-6f) fail("trim did not cut the loud half out of playback");

  looper.set_trim(0.0, 0.5);  // keep only the loud half
  float loud_half = 0.0f;
  for (int i = 0; i < 16; ++i) loud_half = std::max(loud_half, run_block(looper, 0.0f));
  if (loud_half < 0.7f) fail("trim did not let the loud half play back");

  // A fade-in spanning the whole visible window means a block starting right
  // at the window's own start is well under full amplitude. set_trim() does
  // not move the play position by itself - only the self-heal in process()
  // does, and only once the position lands outside the window - so a narrow
  // trim followed by one throwaway block is how the test pins the position
  // back to a known spot before timing each measurement.
  auto resync_to_start = [&] {
    looper.set_trim(0.0, 0.01);
    run_block(looper, 0.0f);
  };

  resync_to_start();
  looper.set_trim(0.0, 1.0);
  looper.set_fades(1.0, 0.0);
  const float faded_in = run_block(looper, 0.0f);

  looper.set_fades(0.0, 0.0);
  resync_to_start();
  looper.set_trim(0.0, 1.0);
  const float not_faded = run_block(looper, 0.0f);

  if (faded_in >= not_faded) fail("fade-in did not lower the first block's peak");

  // A window collapsed to a point at the end used to index one sample past
  // the loop. It must play the whole phrase instead of going silent or
  // walking off the buffer.
  looper.set_trim(1.0, 1.0);
  float collapsed = 0.0f;
  for (int i = 0; i < 16; ++i) collapsed = std::max(collapsed, run_block(looper, 0.0f));
  if (collapsed < 0.7f) fail("a collapsed trim window silenced the loop");

  // A silent loop, then overdub only the second half: the first half must
  // stay silent. Writing at frame 0 before snapping into the window used
  // to leak one sample (and then the rest) into the dry side.
  looper.set_parameter(2, 1.0);
  run_block(looper, 0.0f);
  looper.set_parameter(0, 1.0);
  for (int i = 0; i < 16; ++i) run_block(looper, 0.0f);
  looper.set_parameter(0, 0.0);
  run_block(looper, 0.0f);
  looper.set_trim(0.5, 1.0);
  run_block(looper, 0.0f);
  looper.set_parameter(0, 1.0);
  for (int i = 0; i < 8; ++i) run_block(looper, 0.4f);
  looper.set_parameter(0, 0.0);
  run_block(looper, 0.0f);
  looper.set_trim(0.0, 0.5);
  float leaked = 0.0f;
  for (int i = 0; i < 16; ++i) leaked = std::max(leaked, run_block(looper, 0.0f));
  if (leaked > 1e-6f) fail("overdub wrote outside the trim window");

  looper.set_parameter(2, 1.0);
  run_block(looper, 0.0f);
  looper.set_parameter(0, 1.0);
  for (int i = 0; i < 4; ++i) run_block(looper, 0.6f);
  if (!looper.has_audio()) fail("has_audio() was false during the first record pass");
  bool defining_waveform = false;
  for (float peak : looper.waveform(8)) defining_waveform |= peak > 0.1f;
  if (!defining_waveform) fail("waveform() was empty while the first pass was still open");

  // The metronome can roll the grid with Play off. Rec must wait for a
  // bar line in that state, not punch in on the first block.
  {
    nirbija::LooperInstance grid;
    grid.set_channel_layout(2);
    grid.activate(kRate, kBlock);
    grid.set_parameter(3, 2.0);  // 1 bar

    auto run_at = [&](double beats, bool rolling) {
      std::vector<float> in_l(kBlock, 0.6f), in_r(kBlock, 0.6f);
      std::vector<float> out_l(kBlock), out_r(kBlock);
      const float* ins[2] = {in_l.data(), in_r.data()};
      float* outs[2] = {out_l.data(), out_r.data()};
      nirbija::TransportInfo transport;
      transport.playing = false;
      transport.rolling = rolling;
      transport.tempo_bpm = 120.0;
      transport.numerator = 4;
      transport.beats = beats;
      grid.set_transport(transport);
      grid.process(ins, outs, kBlock);
    };

    grid.set_parameter(0, 1.0);
    run_at(0.5, true);  // mid-bar: must not start
    if (grid.has_audio()) fail("rec started mid-bar with only the metronome rolling");
    run_at(3.99, true);  // crosses the bar
    if (!grid.has_audio()) fail("rec did not start on the bar with the metronome rolling");
  }

  // --- length snap, pitch, tone ------------------------------------------
  {
    nirbija::LooperInstance snapper;
    snapper.set_channel_layout(2);
    snapper.activate(kRate, kBlock);
    snapper.set_parameter(3, 2.0);  // 1 bar

    auto run_at = [&](double beats, float level) {
      std::vector<float> in_l(kBlock, level), in_r(kBlock, level);
      std::vector<float> out_l(kBlock), out_r(kBlock);
      const float* ins[2] = {in_l.data(), in_r.data()};
      float* outs[2] = {out_l.data(), out_r.data()};
      nirbija::TransportInfo transport;
      transport.playing = true;
      transport.tempo_bpm = 120.0;
      transport.numerator = 4;
      transport.beats = beats;
      snapper.set_transport(transport);
      snapper.process(ins, outs, kBlock);
    };

    // Start at a bar line so the first rec is accepted immediately.
    snapper.set_parameter(0, 1.0);
    run_at(0.0, 0.5f);
    // A bit over one bar at 120 / 4/4: 1 bar is 2 s = 96000 frames ≈ 375
    // blocks. 400 blocks is 1.07 bars, which must snap to 1 bar, not stay
    // long.
    for (int i = 0; i < 400; ++i) run_at(0.01 * i, 0.5f);
    snapper.set_parameter(0, 0.0);
    // Sit just before a bar line so this block is the one that crosses it.
    run_at(3.99, 0.0f);

    const double beats = snapper.loop_beats();
    if (std::abs(beats - 4.0) > 0.15)
      fail("1-bar snap closed at " + std::to_string(beats) + " beats, wanted 4");
  }

  {
    nirbija::LooperInstance pitched;
    pitched.set_channel_layout(2);
    pitched.activate(kRate, kBlock);
    pitched.set_parameter(3, 0.0);
    pitched.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(pitched, 0.8f);
    for (int i = 0; i < 8; ++i) run_block(pitched, 0.0f);
    pitched.set_parameter(0, 0.0);
    run_block(pitched, 0.0f);
    pitched.set_trim(0.0, 0.01);
    run_block(pitched, 0.0f);
    pitched.set_trim(0.0, 1.0);
    pitched.set_parameter(5, 12.0);  // +1 octave: the loop plays in half time
    float early = 0.0f, late = 0.0f;
    for (int i = 0; i < 3; ++i) early = std::max(early, run_block(pitched, 0.0f));
    for (int i = 0; i < 2; ++i) run_block(pitched, 0.0f);  // the crossing
    for (int i = 0; i < 2; ++i) late = std::max(late, run_block(pitched, 0.0f));
    if (early < 0.6f) fail("octave-up did not play the loud half first");
    if (late > 0.05f) fail("octave-up still had the loud half after the fold");
  }

  {
    nirbija::LooperInstance dark;
    dark.set_channel_layout(2);
    dark.activate(kRate, kBlock);
    dark.set_parameter(3, 0.0);
    // A Nyquist-ish square: the dark tone must take the edge off.
    std::vector<float> in_l(kBlock), in_r(kBlock), out_l(kBlock), out_r(kBlock);
    for (uint32_t i = 0; i < kBlock; ++i)
      in_l[i] = in_r[i] = (i % 2 == 0) ? 0.8f : -0.8f;
    const float* ins[2] = {in_l.data(), in_r.data()};
    float* outs[2] = {out_l.data(), out_r.data()};
    nirbija::TransportInfo transport;
    transport.playing = true;
    dark.set_transport(transport);
    dark.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) dark.process(ins, outs, kBlock);
    dark.set_parameter(0, 0.0);
    dark.process(ins, outs, kBlock);

    std::vector<float> silent(kBlock, 0.0f);
    const float* zeros[2] = {silent.data(), silent.data()};
    dark.set_parameter(6, 1.0);
    dark.process(zeros, outs, kBlock);
    float open_peak = 0.0f;
    for (float s : out_l) open_peak = std::max(open_peak, std::fabs(s));
    dark.set_parameter(6, 0.0);
    // Drain the filter, then measure.
    for (int i = 0; i < 8; ++i) dark.process(zeros, outs, kBlock);
    float dark_peak = 0.0f;
    for (float s : out_l) dark_peak = std::max(dark_peak, std::fabs(s));
    if (dark_peak >= open_peak)
      fail("tone at 0 did not darken a bright loop");
  }

  {
    nirbija::LooperInstance original;
    original.set_channel_layout(2);
    original.activate(kRate, kBlock);
    original.set_parameter(3, 0.0);
    original.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(original, 0.7f);
    original.set_parameter(0, 0.0);
    run_block(original, 0.0f);
    if (!original.loop_closed()) fail("the take did not close before save");
    const auto blob = original.save_state();

    nirbija::LooperInstance restored;
    restored.set_channel_layout(2);
    restored.activate(kRate, kBlock);
    if (!restored.load_state(blob)) fail("load_state refused a blob with audio");
    if (!restored.loop_closed()) fail("the restored looper had no closed loop");
    bool heard = false;
    for (float peak : restored.waveform(8)) heard |= peak > 0.1f;
    if (!heard) fail("the restored loop was silent");
    float peak = 0.0f;
    for (int i = 0; i < 16; ++i) peak = std::max(peak, run_block(restored, 0.0f));
    if (peak < 0.5f) fail("the restored loop did not play back");

    // An old session, knobs only, still loads.
    const std::string old = "0\n1\n0\n1\n0\n0\n0\n1";
    if (!restored.load_state(std::vector<uint8_t>(old.begin(), old.end())))
      fail("a pre-audio state blob was refused");
  }

  {
    // Closing the app mid-take used to write length_ == 0 and come back empty.
    nirbija::LooperInstance original;
    original.set_channel_layout(2);
    original.activate(kRate, kBlock);
    original.set_parameter(3, 0.0);
    original.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(original, 0.7f);
    if (original.loop_closed()) fail("the open take closed itself before save");
    if (!original.has_audio()) fail("the open take had nothing on the tape");
    const auto blob = original.save_state();

    nirbija::LooperInstance restored;
    restored.set_channel_layout(2);
    restored.activate(kRate, kBlock);
    if (!restored.load_state(blob)) fail("load_state refused an open-take blob");
    if (!restored.loop_closed()) fail("the open take did not come back closed");
    bool heard = false;
    for (float peak : restored.waveform(8)) heard |= peak > 0.1f;
    if (!heard) fail("the restored open take was silent");
    float peak = 0.0f;
    for (int i = 0; i < 16; ++i)
      peak = std::max(peak, run_block(restored, 0.0f));
    if (peak < 0.5f) fail("the restored open take did not play back");
  }

  {
    // JACK often re-activates after the session has already loaded the tape.
    nirbija::LooperInstance original;
    original.set_channel_layout(2);
    original.activate(kRate, kBlock);
    original.set_parameter(3, 0.0);
    original.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(original, 0.7f);
    original.set_parameter(0, 0.0);
    run_block(original, 0.0f);
    const auto blob = original.save_state();

    nirbija::LooperInstance restored;
    restored.set_channel_layout(2);
    restored.activate(kRate, kBlock);
    if (!restored.load_state(blob)) fail("load_state refused a blob before reactivate");
    if (!restored.activate(44100.0, kBlock))
      fail("activate at a new rate failed");
    if (!restored.loop_closed())
      fail("activate after load wiped the loop");
    bool heard = false;
    for (float peak : restored.waveform(8)) heard |= peak > 0.1f;
    if (!heard) fail("the loop was silent after a rate change");

    // Same-rate activate is a no-op and must not clear either.
    if (!restored.activate(44100.0, kBlock))
      fail("same-rate activate failed");
    if (!restored.loop_closed())
      fail("same-rate activate wiped the loop");
  }

  {
    nirbija::LooperInstance layer;
    layer.set_channel_layout(2);
    layer.activate(kRate, kBlock);
    layer.set_parameter(3, 0.0);

    layer.capture_undo_empty();
    layer.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(layer, 0.6f);
    layer.set_parameter(0, 0.0);
    run_block(layer, 0.0f);
    if (!layer.can_undo()) fail("first take left nothing to undo");
    layer.undo();
    if (layer.loop_closed()) fail("undo did not peel the first take");
    if (!layer.can_redo()) fail("undo did not arm redo");
    layer.redo();
    if (!layer.loop_closed()) fail("redo did not put the first take back");

    float before = 0.0f;
    for (int i = 0; i < 8; ++i) before = std::max(before, run_block(layer, 0.0f));
    layer.capture_undo();
    layer.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(layer, 0.6f);
    layer.set_parameter(0, 0.0);
    run_block(layer, 0.0f);
    float dubbed = 0.0f;
    for (int i = 0; i < 8; ++i) dubbed = std::max(dubbed, run_block(layer, 0.0f));
    if (dubbed < before + 0.2f) fail("overdub before undo was too quiet");
    layer.undo();
    float peeled = 0.0f;
    for (int i = 0; i < 8; ++i) peeled = std::max(peeled, run_block(layer, 0.0f));
    if (peeled > before + 0.15f) fail("undo did not peel the overdub");
    layer.capture_undo();
    layer.set_parameter(2, 1.0);
    run_block(layer, 0.0f);
    if (layer.loop_closed()) fail("clear left a loop");
    layer.undo();
    if (!layer.loop_closed()) fail("undo did not restore a cleared loop");
  }

  // Two phrases in one Rec pass, split by silence. Undo has to drop only
  // the last one — the whole-pass swap used to throw the first away too.
  {
    nirbija::LooperInstance split;
    split.set_channel_layout(2);
    split.activate(kRate, kBlock);
    split.set_parameter(3, 0.0);
    split.capture_undo_empty();
    split.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(split, 0.7f);
    for (int i = 0; i < 32; ++i) run_block(split, 0.0f);
    for (int i = 0; i < 8; ++i) run_block(split, 0.7f);
    split.set_parameter(0, 0.0);
    run_block(split, 0.0f);

    const auto whole = split.waveform(8);
    if (whole.size() < 8 || whole.front() < 0.4f || whole.back() < 0.4f)
      fail("two-phrase take did not land both licks on the tape");

    split.undo();
    if (!split.loop_closed()) fail("undo of the last phrase dropped the loop");
    const auto peaks = split.waveform(8);
    if (peaks.size() < 8) fail("waveform of the split take was empty");
    if (peaks.front() < 0.4f)
      fail("undo of the last phrase took the first one too");
    if (peaks.back() > 0.05f)
      fail("the last phrase was still on the tape after undo");

    split.redo();
    const auto back = split.waveform(8);
    if (back.size() < 8 || back.back() < 0.4f)
      fail("redo did not put the last phrase back");

    split.undo();
    split.undo();
    if (split.loop_closed())
      fail("undo of the remaining phrase did not drop an empty take");
  }

  {
    nirbija::LooperInstance fx;
    fx.set_channel_layout(2);
    fx.activate(kRate, kBlock);
    fx.set_parameter(3, 0.0);
    fx.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(fx, 0.7f);
    fx.set_parameter(0, 0.0);
    run_block(fx, 0.0f);

    fx.set_parameter(7, 1.0);  // reverse
    run_block(fx, 0.0f);       // wrap off the start onto the tail
    const double a = fx.position_fraction();
    run_block(fx, 0.0f);
    const double b = fx.position_fraction();
    if (!(b < a - 0.01 || (a < 0.15 && b > 0.7)))
      fail("reverse did not walk the playhead backwards");

    fx.set_parameter(7, 0.0);
    fx.set_parameter(11, 2.0);  // double speed
    const double s0 = fx.position_fraction();
    run_block(fx, 0.0f);
    const double s1 = fx.position_fraction();
    const double step = s1 > s0 ? s1 - s0 : (1.0 - s0) + s1;
    if (step < 0.15)
      fail("double speed did not walk the tape faster");
    fx.set_parameter(11, 1.0);
  }

  {
    nirbija::LooperInstance fade;
    fade.set_channel_layout(2);
    fade.activate(kRate, kBlock);
    fade.set_parameter(3, 0.0);
    fade.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(fade, 0.8f);
    fade.set_parameter(0, 0.0);
    run_block(fade, 0.0f);

    fade.set_parameter(8, 0.0);  // feedback 0: overdub erases
    fade.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(fade, 0.0f);
    fade.set_parameter(0, 0.0);
    run_block(fade, 0.0f);
    float left = 0.0f;
    for (int i = 0; i < 8; ++i) left = std::max(left, run_block(fade, 0.0f));
    if (left > 0.05f) fail("feedback 0 left the old layer standing");
  }

  {
    nirbija::LooperInstance punch;
    punch.set_channel_layout(2);
    punch.activate(kRate, kBlock);
    punch.set_parameter(3, 0.0);
    punch.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(punch, 0.8f);
    punch.set_parameter(0, 0.0);
    run_block(punch, 0.0f);

    punch.set_parameter(9, 1.0);  // replace
    punch.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(punch, 0.0f);
    punch.set_parameter(0, 0.0);
    run_block(punch, 0.0f);
    float left = 0.0f;
    for (int i = 0; i < 8; ++i) left = std::max(left, run_block(punch, 0.0f));
    if (left > 0.05f) fail("replace did not overwrite the take");
  }

  {
    nirbija::LooperInstance once;
    once.set_channel_layout(2);
    once.activate(kRate, kBlock);
    once.set_parameter(3, 0.0);
    once.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(once, 0.7f);
    once.set_parameter(0, 0.0);
    run_block(once, 0.0f);
    once.set_parameter(10, 1.0);  // once
    float first = 0.0f;
    for (int i = 0; i < 8; ++i) first = std::max(first, run_block(once, 0.0f));
    if (first < 0.5f) fail("once muted the first pass");
    float after = 0.0f;
    for (int i = 0; i < 8; ++i) after = std::max(after, run_block(once, 0.0f));
    if (after > 1e-4f) fail("once kept playing after the wrap");
    if (once.playing()) fail("once did not drop Play");
  }

  {
    nirbija::LooperInstance doubled;
    doubled.set_channel_layout(2);
    doubled.activate(kRate, kBlock);
    doubled.set_parameter(3, 0.0);
    doubled.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(doubled, 0.6f);
    doubled.set_parameter(0, 0.0);
    run_block(doubled, 0.0f);
    const double beats = doubled.loop_beats();
    if (!doubled.can_multiply()) fail("a short take could not be multiplied");
    doubled.multiply();
    if (doubled.loop_beats() < beats * 1.9)
      fail("multiply did not double the loop");
    float peak = 0.0f;
    for (int i = 0; i < 16; ++i)
      peak = std::max(peak, run_block(doubled, 0.0f));
    if (peak < 0.4f) fail("the multiplied loop was silent");
  }

  {
    nirbija::LooperInstance original;
    original.set_channel_layout(2);
    original.activate(kRate, kBlock);
    original.set_parameter(3, 0.0);
    original.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(original, 0.6f);
    original.set_parameter(0, 0.0);
    run_block(original, 0.0f);
    original.set_parameter(7, 1.0);
    original.set_parameter(8, 0.7);
    original.set_parameter(11, 0.5);
    original.set_parameter(12, 1.0);
    const auto blob = original.save_state();

    nirbija::LooperInstance restored;
    restored.set_channel_layout(2);
    restored.activate(kRate, kBlock);
    if (!restored.load_state(blob)) fail("NLOOP2 blob was refused");
    if (restored.parameter_value(7) < 0.5) fail("reverse did not survive save");
    if (std::fabs(restored.parameter_value(8) - 0.7) > 1e-4)
      fail("feedback did not survive save");
    if (std::fabs(restored.parameter_value(11) - 0.5) > 1e-4)
      fail("speed did not survive save");
    if (restored.parameter_value(12) < 0.5) fail("count-in did not survive save");
    if (!restored.loop_closed()) fail("NLOOP2 came back without a loop");
  }

  // Count-in: one bar of clicks, no tape, then Rec. Play can be off.
  {
    nirbija::LooperInstance count;
    count.set_channel_layout(2);
    count.activate(kRate, kBlock);
    count.set_parameter(3, 0.0);
    count.set_parameter(12, 1.0);
    count.set_parameter(0, 1.0);

    constexpr double kTempo = 480.0;  // 4 beats = 0.5 s
    float click = 0.0f;
    for (int i = 0; i < 16; ++i) {
      click = std::max(click, run_block(count, 0.0f, kTempo, false));
      if (count.has_audio()) fail("count-in wrote the tape before the bar");
    }
    if (click < 0.05f) fail("count-in made no click with Play off");
    if (count.count_in_beats() <= 0) fail("count-in finished before a bar");

    int guard = 0;
    while (count.count_in_beats() > 0 && guard++ < 400)
      run_block(count, 0.0f, kTempo, false);
    if (count.count_in_beats() > 0) fail("count-in never reached Rec");

    for (int i = 0; i < 8; ++i) run_block(count, 0.6f, kTempo, false);
    count.set_parameter(0, 0.0);
    run_block(count, 0.0f, kTempo, false);
    if (!count.loop_closed()) fail("rec after count-in did not close a take");
    float heard = 0.0f;
    for (int i = 0; i < 16; ++i)
      heard = std::max(heard, run_block(count, 0.0f, kTempo, false));
    if (heard < 0.4f) fail("the take after count-in was silent");
  }

  {
    nirbija::LooperInstance cancel;
    cancel.set_channel_layout(2);
    cancel.activate(kRate, kBlock);
    cancel.set_parameter(12, 1.0);
    cancel.set_parameter(0, 1.0);
    run_block(cancel, 0.0f, 480.0, false);
    if (cancel.count_in_beats() <= 0) fail("count-in did not start");
    cancel.set_parameter(0, 0.0);
    run_block(cancel, 0.0f, 480.0, false);
    if (cancel.count_in_beats() > 0) fail("rec off did not cancel the count");
    if (cancel.recording()) fail("cancelled count left rec armed");
    if (cancel.has_audio()) fail("cancelled count wrote the tape");
  }

  {
    nirbija::LooperInstance latch;
    latch.set_channel_layout(2);
    latch.activate(kRate, kBlock);
    latch.set_parameter(12, 1.0);
    latch.set_parameter(0, 1.0);
    run_block(latch, 0.0f, 480.0, false);
    if (latch.count_in_beats() <= 0) fail("count-in did not start");
    latch.set_count_in(false);
    run_block(latch, 0.0f, 480.0, false);
    if (latch.count_in()) fail("count-in latch did not turn off");
    if (latch.count_in_beats() > 0) fail("turning Count off left the count running");
    if (latch.recording()) fail("turning Count off left rec armed");
    if (latch.has_audio()) fail("turning Count off punched Rec in");
  }

  // Length is 1 bar. Rec left down has to close on that bar and keep the
  // take at full level: writing silence onto the end used to bury the
  // phrase, and an overdub that scaled the old layer every sample ate it.
  {
    nirbija::LooperInstance held;
    held.set_channel_layout(2);
    held.activate(kRate, kBlock);
    held.set_parameter(3, 2.0);  // 1 bar
    held.set_parameter(0, 1.0);

    const int bar_blocks =
        static_cast<int>(kRate * 60.0 / 120.0 * 4.0 / kBlock);  // 375
    for (int i = 0; i < bar_blocks; ++i) run_block(held, 0.6f);
    if (!held.loop_closed())
      fail("a held Rec did not close the take at Length");
    if (!held.recording()) fail("closing at Length dropped Rec");

    float first = 0.0f;
    for (int i = 0; i < bar_blocks; ++i)
      first = std::max(first, run_block(held, 0.0f));
    if (first < 0.5f)
      fail("the take was gone once Rec stayed down (peak " +
           std::to_string(first) + ")");

    float later = 0.0f;
    for (int i = 0; i < bar_blocks * 3; ++i)
      later = std::max(later, run_block(held, 0.0f));
    if (later < first - 0.05f)
      fail("a held Rec faded the take (first " + std::to_string(first) +
           " later " + std::to_string(later) + ")");
  }

  // --- loop level meter ---------------------------------------------------
  {
    nirbija::LooperInstance meter;
    meter.set_channel_layout(2);
    meter.activate(kRate, kBlock);
    meter.set_parameter(3, 0.0);  // quantise off

    if (meter.loop_peak() > 1e-6f) fail("loop_peak was hot before anything played");

    meter.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(meter, 0.7f);
    meter.set_parameter(0, 0.0);

    float meter_peak = 0.0f;
    for (int i = 0; i < 16; ++i) {
      run_block(meter, 0.0f);
      meter_peak = std::max(meter_peak, meter.loop_peak());
    }
    if (meter_peak < 0.4f) fail("loop_peak did not track the closed loop");

    // Play off silences the loop's own contribution, dry pass-through aside.
    meter.set_parameter(1, 0.0);
    run_block(meter, 0.0f);
    if (meter.loop_peak() > 1e-6f) fail("loop_peak stayed hot with Play off");
  }

  // --- layer map -----------------------------------------------------------
  {
    nirbija::LooperInstance layers;
    layers.set_channel_layout(2);
    layers.activate(kRate, kBlock);
    layers.set_parameter(3, 0.0);  // quantise off

    // Base take: 16 blocks, all layer 0.
    layers.set_parameter(0, 1.0);
    for (int i = 0; i < 16; ++i) run_block(layers, 0.6f);
    layers.set_parameter(0, 0.0);
    run_block(layers, 0.0f);
    if (!layers.loop_closed()) fail("layer map test take did not close");

    auto base_map = layers.layer_map(8);
    for (int layer : base_map)
      if (layer != 0) fail("base take showed a layer above 0 before any overdub");

    // Overdub loudly over roughly the first half only.
    layers.set_parameter(0, 1.0);
    for (int i = 0; i < 8; ++i) run_block(layers, 0.8f);
    layers.set_parameter(0, 0.0);
    run_block(layers, 0.0f);

    auto dubbed_map = layers.layer_map(8);
    bool saw_layer_one = false;
    bool saw_layer_zero = false;
    for (int layer : dubbed_map) {
      if (layer >= 1) saw_layer_one = true;
      if (layer == 0) saw_layer_zero = true;
    }
    if (!saw_layer_one) fail("layer_map never showed the overdub pass");
    if (!saw_layer_zero) fail("layer_map lost the base take everywhere");
  }

  // --- sync to another looper's length -------------------------------------
  {
    nirbija::LooperInstance source;
    source.set_channel_layout(2);
    source.activate(kRate, kBlock);
    source.set_parameter(3, 3.0);  // Length: 2 bars
    nirbija::TransportInfo transport;
    transport.playing = true;
    transport.rolling = true;
    transport.tempo_bpm = 120.0;
    transport.numerator = 4;
    source.set_transport(transport);
    source.set_parameter(0, 1.0);
    const int bar_blocks =
        static_cast<int>(kRate * 60.0 / 120.0 * 4.0 / kBlock);
    for (int i = 0; i < bar_blocks * 3; ++i) run_block(source, 0.5f, 120.0);
    if (!source.loop_closed()) fail("sync source take did not close at 2 bars");
    if (std::fabs(source.loop_beats() - 8.0) > 0.5)
      fail("sync source did not close at 8 beats (2 bars of 4/4)");

    nirbija::LooperInstance follower;
    follower.set_channel_layout(2);
    follower.activate(kRate, kBlock);
    follower.set_parameter(3,
                           static_cast<double>(nirbija::LooperInstance::kQuantizeSync));
    if (follower.quantize() != nirbija::LooperInstance::kQuantizeSync)
      fail("Quantize did not accept kQuantizeSync");

    // Nothing pushed in yet: Sync behaves like free length.
    if (follower.sync_beats() != 0.0) fail("sync_beats was not 0 before anyone pushed one");

    // Mimic what MixerModel's poll does: copy the source's closed length in.
    follower.set_sync_beats(source.loop_beats());
    follower.set_transport(transport);
    follower.set_parameter(0, 1.0);
    for (int i = 0; i < bar_blocks * 3; ++i) run_block(follower, 0.5f, 120.0);
    if (!follower.loop_closed()) fail("synced take never closed");
    if (std::fabs(follower.loop_beats() - source.loop_beats()) > 0.5)
      fail("synced take did not land on the source's own length (got " +
           std::to_string(follower.loop_beats()) + " wanted " +
           std::to_string(source.loop_beats()) + ")");

    // The target choice round-trips through save/load, even with no audio.
    follower.set_sync_target(2, 1);
    const auto blob = follower.save_state();
    nirbija::LooperInstance restored;
    restored.set_channel_layout(2);
    if (!restored.load_state(blob)) fail("sync target blob was refused");
    if (restored.sync_target_row() != 2 || restored.sync_target_slot() != 1)
      fail("sync target did not survive save/load");
  }

  // The gap between the press and the tape. With Length on the bar and the
  // grid moving, Rec pressed mid-bar arms but does not write until the next
  // downbeat, and Rec released mid-bar keeps writing until the one after.
  // writing() and beats_to_boundary() are what the editor counts down from
  // for the player, so they have to tell the two apart.
  {
    nirbija::LooperInstance gap;
    gap.set_channel_layout(2);
    gap.activate(kRate, kBlock);
    gap.set_parameter(3, 2.0);  // 1 bar

    double beats = 0.0;
    const double block_beats = 120.0 / 60.0 * (kBlock / kRate);
    auto step = [&](float level) {
      std::vector<float> in_l(kBlock, level), in_r(kBlock, level);
      std::vector<float> out_l(kBlock), out_r(kBlock);
      const float* ins[2] = {in_l.data(), in_r.data()};
      float* outs[2] = {out_l.data(), out_r.data()};
      nirbija::TransportInfo transport;
      transport.playing = true;
      transport.rolling = true;
      transport.tempo_bpm = 120.0;
      transport.numerator = 4;
      transport.beats = beats;
      gap.set_transport(transport);
      gap.process(ins, outs, kBlock);
      beats += block_beats;
    };

    // Walk to the middle of the first bar, then press.
    while (beats < 2.0) step(0.0f);
    gap.set_parameter(0, 1.0);
    step(0.5f);
    if (!gap.recording()) fail("rec mid-bar did not arm");
    if (gap.writing()) fail("rec mid-bar wrote before the downbeat");
    if (gap.has_audio()) fail("rec mid-bar put audio on the tape early");
    const double to_go = gap.beats_to_boundary();
    if (to_go <= 1.5 || to_go > 2.0)
      fail("beats_to_boundary mid-bar was " + std::to_string(to_go) +
           ", wanted just under 2");

    // Across the downbeat: now it writes.
    while (beats < 4.5) step(0.5f);
    if (!gap.writing()) fail("rec did not start writing on the downbeat");
    if (!gap.has_audio()) fail("the tape stayed empty past the downbeat");

    // Release mid-bar: still writing until the bar closes.
    gap.set_parameter(0, 0.0);
    step(0.5f);
    if (gap.recording()) fail("rec off did not clear the request");
    if (!gap.writing()) fail("rec off mid-bar stopped the head before the bar");
    if (gap.loop_closed()) fail("the loop closed mid-bar");
    while (beats < 8.5) step(0.5f);
    if (gap.writing()) fail("the head kept writing past the bar after rec off");
    if (!gap.loop_closed()) fail("the loop did not close on the bar");

    // Free Length: no grid to wait for, and the mirror says so.
    gap.set_parameter(3, 0.0);
    step(0.0f);
    if (gap.beats_to_boundary() >= 0.0)
      fail("free Length still reported a boundary to wait for");
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
