// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// Scenes change on the bar line, to the frame, and never click: a strip the
// scene switches off slopes out over the fade, a level walks, a sequencer
// pattern lands on the same frame the scene does, a hand stops the walk, and
// the list walks on, holds, and stays on its last scene.

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/audio_graph.h"
#include "core/fx_pad.h"
#include "core/scene_conductor.h"
#include "core/step_sequencer.h"

namespace {

using nirbija::SceneConductor;
using nirbija::SceneTable;
using nirbija::SceneTarget;

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kBlock = 256;
constexpr uint32_t kSeqPattern = 196;
int failures = 0;

void expect(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what.c_str());
    ++failures;
  }
}

class DcSource : public nirbija::AudioSource {
 public:
  explicit DcSource(float level) : level_(level) {}
  void read(float* const* dest, int channels, uint32_t frames) override {
    for (int ch = 0; ch < channels; ++ch) std::fill_n(dest[ch], frames, level_);
  }

 private:
  float level_;
};

// A graph, a conductor and a transport, run a block at a time the way the
// engine runs them. `out` is the strip's own output, after its fader, so the
// master's limiter is not in the measurement.
struct Rig {
  nirbija::AudioGraph graph;
  SceneConductor scenes;
  size_t strip = 0;
  double tempo = 110.0;  // a bar is 104727.27 frames: lines fall mid-block
  double beats = 0.0;
  bool playing = true;
  bool changed = true;
  std::vector<float> out;

  explicit Rig(double bpm) : tempo(bpm) {
    graph.prepare(kSampleRate, kBlock);
    strip = graph.add_channel("dc", 1, std::make_unique<DcSource>(0.5f));
  }

  void block() {
    nirbija::TransportInfo t;
    t.playing = playing;
    t.rolling = playing;
    t.tempo_bpm = tempo;
    t.numerator = 4;
    t.denominator = 4;
    t.beats = beats;
    t.changed = changed;
    changed = false;
    graph.set_transport(t);
    scenes.run(graph, t, kBlock, kSampleRate);
    std::vector<float> l(kBlock), r(kBlock);
    float* master[2] = {l.data(), r.data()};
    graph.render(master, kBlock);
    const float* cache = graph.channel(strip).output_cache(0);
    out.insert(out.end(), cache, cache + kBlock);
    if (playing) beats += kBlock / kSampleRate * tempo / 60.0;
  }
  void run_bars(double bars) {
    const double frames = bars * 4.0 * 60.0 / tempo * kSampleRate;
    for (double done = 0.0; done < frames; done += kBlock) block();
  }
  // The frame, counted from the start of `out`, where bar line `bar` falls.
  size_t line_frame(int bar) const {
    return static_cast<size_t>(std::ceil(bar * 4.0 * 60.0 / tempo * kSampleRate));
  }
};

float max_jump(const std::vector<float>& v, size_t from, size_t to) {
  float most = 0.0f;
  for (size_t i = from + 1; i < to && i < v.size(); ++i)
    most = std::max(most, std::fabs(v[i] - v[i - 1]));
  return most;
}

SceneTarget gate(size_t strip, bool on) {
  SceneTarget t;
  t.what = SceneTarget::What::Gate;
  t.strip = static_cast<uint16_t>(strip);
  t.value = on ? 1.0f : 0.0f;
  return t;
}

SceneTarget level(size_t strip, float gain) {
  SceneTarget t;
  t.what = SceneTarget::What::Level;
  t.strip = static_cast<uint16_t>(strip);
  t.value = gain;
  return t;
}

void add_scene(SceneTable& table, uint32_t bars, uint32_t fade,
               std::vector<SceneTarget> targets) {
  SceneTable::Scene scene;
  scene.bars = bars;
  scene.fade_bars = fade;
  scene.first = static_cast<uint32_t>(table.targets.size());
  scene.count = static_cast<uint32_t>(targets.size());
  table.scenes.push_back(scene);
  for (auto& t : targets) table.targets.push_back(t);
}

}  // namespace

int main() {
  // --- a gate that fades out over a bar, starting on the line's frame --------
  for (double bpm : {110.0, 120.0}) {  // mid-block lines, then lines on a block edge
    const std::string at = " at " + std::to_string(static_cast<int>(bpm)) + " bpm";
    Rig rig(bpm);
    auto table = std::make_shared<SceneTable>();
    add_scene(*table, 2, 0, {gate(rig.strip, true)});
    add_scene(*table, 0, 1, {gate(rig.strip, false)});
    rig.scenes.publish(table);
    rig.run_bars(4.2);

    expect(rig.scenes.current() == 1, "the list did not walk to the second scene" + at);
    const size_t line = rig.line_frame(2);
    expect(std::fabs(rig.out[line - 1] - 0.5f) < 1e-4f,
           "the strip moved before the bar line" + at);
    expect(rig.out[line + 64] < 0.5f, "the fade did not start on the bar line" + at);
    const size_t bar = rig.line_frame(3) - line;
    expect(rig.out[line + bar / 2] > 0.1f && rig.out[line + bar / 2] < 0.4f,
           "halfway through the fade the strip was not halfway" + at);
    expect(rig.out[rig.line_frame(3) + 16] < 1e-4f, "the strip was not off after the fade" + at);
    // A bar-long raised cosine from 0.5 moves under 1e-4 per sample.
    expect(max_jump(rig.out, 0, rig.out.size()) < 1e-4f,
           "the scene gate stepped: " + std::to_string(max_jump(rig.out, 0, rig.out.size())) + at);
  }

  // --- a fade of zero still slopes --------------------------------------------
  {
    Rig rig(110.0);
    auto table = std::make_shared<SceneTable>();
    add_scene(*table, 1, 0, {gate(rig.strip, true)});
    add_scene(*table, 0, 0, {gate(rig.strip, false)});
    rig.scenes.publish(table);
    rig.run_bars(2.5);
    // 10 ms of raised cosine from 0.5: at most pi/2 * 0.5/480 per sample.
    expect(max_jump(rig.out, 0, rig.out.size()) < 0.0017f,
           "a scene with no fade cut the strip: " +
               std::to_string(max_jump(rig.out, 0, rig.out.size())));
    expect(rig.out.back() < 1e-6f, "a scene with no fade did not switch the strip off");
  }

  // --- a level walks in decibels, and a hand stops it --------------------------
  {
    Rig rig(110.0);
    auto table = std::make_shared<SceneTable>();
    add_scene(*table, 2, 0, {level(rig.strip, 1.0f)});
    add_scene(*table, 0, 2, {level(rig.strip, 0.1f)});
    rig.scenes.publish(table);
    rig.run_bars(1.9);
    expect(std::fabs(rig.graph.channel(rig.strip).gain_target() - 1.0f) < 1e-6f,
           "the level moved before its scene");
    rig.run_bars(1.1);  // a bar into a two-bar fade: -10 dB, halfway in dB
    const float mid = rig.graph.channel(rig.strip).gain_target();
    expect(mid > 0.25f && mid < 0.4f, "a level fade is not walking in decibels: " +
                                          std::to_string(mid));
    expect(rig.scenes.level_writes(false, rig.strip) > 0, "the UI was not told of the walk");
    expect(max_jump(rig.out, 0, rig.out.size()) < 1e-4f,
           "the level walk stepped: " + std::to_string(max_jump(rig.out, 0, rig.out.size())));

    rig.scenes.hand(false, rig.strip, SceneTarget::What::Level);
    rig.graph.channel(rig.strip).set_gain(0.7f);
    rig.run_bars(2.0);
    expect(std::fabs(rig.graph.channel(rig.strip).gain_target() - 0.7f) < 1e-6f,
           "the scene kept walking a fader the player took");
  }

  // --- a level walk that runs to the end lands exactly -------------------------
  {
    Rig rig(120.0);
    auto table = std::make_shared<SceneTable>();
    add_scene(*table, 1, 0, {level(rig.strip, 0.0f)});
    add_scene(*table, 0, 1, {level(rig.strip, 0.8f)});
    rig.scenes.publish(table);
    rig.run_bars(3.0);
    expect(std::fabs(rig.graph.channel(rig.strip).gain_target() - 0.8f) < 1e-6f,
           "a level fade up from silence did not end on its value");
    // From the first line on: the scene that starts the song cuts the level
    // to zero over the fader's own 15 ms, which is not what is measured.
    expect(max_jump(rig.out, rig.line_frame(1) - 1, rig.out.size()) < 1e-4f,
           "a fade up from silence stepped: " +
               std::to_string(max_jump(rig.out, rig.line_frame(1) - 1, rig.out.size())));
  }

  // --- patterns land on the scene's line, armed early or late -----------------
  for (double bpm : {110.0, 120.0}) {
    const std::string at = " at " + std::to_string(static_cast<int>(bpm)) + " bpm";
    Rig rig(bpm);
    auto seq = std::make_unique<nirbija::StepSequencerInstance>();
    nirbija::PluginInstance* raw = seq.get();
    rig.graph.channel(rig.strip).add_insert(std::move(seq));
    const uint32_t tag = rig.graph.channel(rig.strip).insert_tag(0);
    expect(tag != 0 && rig.graph.channel(rig.strip).insert_by_tag(tag) == raw,
           "a sequencer is not found by its tag");

    SceneTarget pattern;
    pattern.what = SceneTarget::What::Pattern;
    pattern.strip = static_cast<uint16_t>(rig.strip);
    pattern.insert_tag = tag;
    auto table = std::make_shared<SceneTable>();
    pattern.value = 0.0f;
    add_scene(*table, 2, 0, {pattern});
    pattern.value = 3.0f;
    add_scene(*table, 0, 0, {pattern});
    pattern.value = 5.0f;
    add_scene(*table, 0, 0, {pattern});
    rig.scenes.publish(table);

    // The walk from scene 1 to 2 at the end of bar 2.
    rig.run_bars(1.99);
    expect(raw->parameter_value(kSeqPattern) == 0.0,
           "the pattern changed early" + at + ": " +
               std::to_string(raw->parameter_value(kSeqPattern)));
    while (rig.out.size() + kBlock <= rig.line_frame(2)) rig.block();
    rig.block();  // the block holding line 2 (or starting on it)
    rig.block();
    expect(raw->parameter_value(kSeqPattern) == 3.0,
           "the pattern did not change with the scene" + at + ": " +
               std::to_string(raw->parameter_value(kSeqPattern)));

    // Armed in the middle of a bar: the next line, not before.
    rig.run_bars(0.5);
    rig.scenes.arm(2);
    rig.block();
    expect(raw->parameter_value(kSeqPattern) == 3.0, "an armed pattern did not wait for the line" + at);
    expect(rig.scenes.armed() == 2, "the armed scene was not published" + at);
    rig.run_bars(0.6);
    expect(rig.scenes.current() == 2 && raw->parameter_value(kSeqPattern) == 5.0,
           "an armed scene did not land on the next line" + at);
  }

  // --- a plugin parameter walks over the fade, a stepped one jumps -------------
  {
    Rig rig(110.0);
    auto fx = std::make_unique<nirbija::FxPadInstance>();
    nirbija::PluginInstance* raw = fx.get();
    rig.graph.channel(rig.strip).add_insert(std::move(fx));
    const uint32_t tag = rig.graph.channel(rig.strip).insert_tag(0);
    auto param = [&](uint32_t id, float value, bool stepped = false) {
      SceneTarget t;
      t.what = SceneTarget::What::Param;
      t.strip = static_cast<uint16_t>(rig.strip);
      t.insert_tag = tag;
      t.param = id;
      t.value = value;
      t.stepped = stepped;
      return t;
    };
    const uint32_t filter = nirbija::FxPadInstance::Filter;
    const uint32_t crush = nirbija::FxPadInstance::Crush;
    auto table = std::make_shared<SceneTable>();
    add_scene(*table, 1, 0, {param(filter, 0.0f), param(crush, 0.0f, true)});
    add_scene(*table, 0, 2, {param(filter, 0.8f), param(crush, 0.5f, true)});
    rig.scenes.publish(table);

    rig.run_bars(0.9);
    expect(raw->parameter_value(filter) == 0.0, "a parameter moved before its scene");
    while (rig.out.size() <= rig.line_frame(1)) rig.block();
    expect(std::fabs(raw->parameter_value(crush) - 0.5) < 1e-6,
           "a stepped parameter did not jump on the line");
    expect(raw->parameter_value(filter) < 0.05,
           "a walking parameter jumped instead of starting from where it was");
    rig.run_bars(1.0);
    const double mid = raw->parameter_value(filter);
    expect(mid > 0.35 && mid < 0.45,
           "a bar into a two-bar fade the parameter is not halfway: " + std::to_string(mid));

    rig.scenes.hand(false, rig.strip, SceneTarget::What::Param, tag, filter);
    raw->set_parameter(filter, 0.1);
    rig.run_bars(1.5);
    expect(std::fabs(raw->parameter_value(filter) - 0.1) < 1e-6,
           "the scene kept walking a parameter the player took");
  }

  // --- the list: walk, hold, stay on the last ----------------------------------
  {
    Rig rig(120.0);
    auto table = std::make_shared<SceneTable>();
    add_scene(*table, 1, 0, {});
    add_scene(*table, 1, 0, {});
    add_scene(*table, 1, 0, {});
    rig.scenes.publish(table);
    rig.run_bars(0.5);
    expect(rig.scenes.current() == 0, "play from the top did not start the first scene");
    rig.scenes.set_hold(true);
    rig.run_bars(3.0);
    expect(rig.scenes.current() == 0, "a held scene walked on");
    expect(rig.scenes.bar_in_scene() == 0, "a held one-bar scene did not start over");
    rig.scenes.set_hold(false);
    rig.run_bars(1.0);
    expect(rig.scenes.current() == 1, "released, the list did not walk on");
    rig.run_bars(5.0);
    expect(rig.scenes.current() == 2, "the last scene did not stay");
    rig.scenes.set_auto(false);
    rig.scenes.arm(0);
    rig.run_bars(3.0);
    expect(rig.scenes.current() == 0, "with the list stopped, an armed scene did not play");
  }

  // --- stopped: an armed scene lands now and play starts there; a rewind starts over
  {
    Rig rig(120.0);
    rig.playing = false;
    auto table = std::make_shared<SceneTable>();
    add_scene(*table, 4, 4, {gate(rig.strip, true)});
    add_scene(*table, 4, 4, {gate(rig.strip, false)});
    rig.scenes.publish(table);
    rig.scenes.arm(1);
    rig.block();
    expect(rig.scenes.current() == 1 && rig.scenes.armed() == SceneConductor::kNone,
           "stopped, an armed scene did not land at once");
    for (int i = 0; i < 4; ++i) rig.block();
    expect(rig.out.back() < 1e-6f, "stopped, the scene did not use the short slope");

    // Picked while stopped, it is where play starts, counting from bar 1.
    rig.playing = true;
    rig.changed = true;
    rig.run_bars(0.5);
    expect(rig.scenes.current() == 1 && rig.scenes.bar_in_scene() == 0,
           "play did not start from the scene picked while stopped");

    // A rewind after playing goes back to the first scene.
    rig.run_bars(1.0);
    rig.beats = 0.0;
    rig.changed = true;
    rig.run_bars(0.5);
    expect(rig.scenes.current() == 0, "a rewind did not go back to the first scene");
  }

  // --- a table swapped mid-song, and one that shrank -----------------------------
  {
    Rig rig(120.0);
    auto table = std::make_shared<SceneTable>();
    add_scene(*table, 0, 0, {});
    add_scene(*table, 0, 0, {});
    add_scene(*table, 0, 0, {});
    rig.scenes.publish(table);
    rig.scenes.arm(2);
    rig.run_bars(1.5);
    expect(rig.scenes.current() == 2, "setup: scene 3 did not start");
    auto smaller = std::make_shared<SceneTable>();
    add_scene(*smaller, 0, 0, {});
    rig.scenes.publish(smaller);
    rig.block();
    expect(rig.scenes.current() == SceneConductor::kNone,
           "a scene the new table does not have is still current");
    rig.run_bars(4.0);
    rig.scenes.reclaim(true);
    rig.scenes.reclaim(false);
  }

  if (failures == 0) std::puts("scene_conductor: ok");
  return failures == 0 ? 0 : 1;
}
