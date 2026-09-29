// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

// Scenes, on the audio thread. A scene is where the strips should be — which
// ones play, at what level, on which sequencer pattern — and how many bars the
// way there takes. The conductor keeps the song's place in a list of them and
// changes scene on a bar line, to the frame, walking every level on the way.
// See design/scenes.md.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/channel_strip.h"
#include "core/dsp.h"
#include "core/plugin.h"

namespace nirbija {

class AudioGraph;

struct SceneTarget {
  enum class What : uint8_t {
    Gate,     // the strip's scene gate, on or off
    Level,    // the fader, linear gain
    Pattern,  // a step sequencer's pattern, 0-15
    Param,    // a plugin parameter, in the plugin's own range
  };
  What what = What::Gate;
  bool bus = false;
  // A parameter that only takes whole values (a mode, a division) jumps on
  // the bar line; every other one walks over the fade.
  bool stepped = false;
  uint16_t strip = 0;       // graph slot
  uint32_t insert_tag = 0;  // Pattern and Param: the plugin, by chain tag
  uint32_t param = 0;       // Param: its id
  float value = 0.0f;
};

// Only a plugin whose set_parameter is a plain store may be reached from the
// audio thread: the built-in ones and LV2 (a control port). The UI builds
// Param targets for nothing else; CLAP and VST3 need a queue of their own
// first (design/scenes.md, phase 3).

// Built whole on the UI thread and never written again once published: the
// audio thread reads it without a lock for as long as it is live.
struct SceneTable {
  struct Scene {
    uint32_t bars = 0;       // 0 = until something else happens
    uint32_t fade_bars = 0;  // 0 = the controls' own short slopes
    uint32_t first = 0;      // into `targets`
    uint32_t count = 0;
  };
  std::vector<Scene> scenes;
  std::vector<SceneTarget> targets;
};

class SceneConductor {
 public:
  static constexpr int kNone = -1;

  // --- UI thread ------------------------------------------------------------
  // Replaces the scene list. The old one is kept until the audio thread has
  // provably left it (see dsp::RetiredList). A scene index the new table does
  // not have stops being current or armed on the next block.
  void publish(std::shared_ptr<const SceneTable> table);
  // Frees retired tables; `audio_running` false frees them all at once.
  void reclaim(bool audio_running);

  // Whether the list walks on by itself at the end of a scene, and whether
  // the current scene repeats instead. Plain flags; read once per block.
  void set_auto(bool on) { auto_.store(on, std::memory_order_relaxed); }
  void set_hold(bool on) { hold_.store(on, std::memory_order_relaxed); }
  bool auto_advance() const { return auto_.load(std::memory_order_relaxed); }
  bool hold() const { return hold_.load(std::memory_order_relaxed); }

  // What the audio thread last did, for the UI to draw.
  int current() const { return pub_current_.load(std::memory_order_relaxed); }
  int armed() const { return pub_armed_.load(std::memory_order_relaxed); }
  // Bars since the current scene started, from 0; -1 before its first bar.
  int bar_in_scene() const { return pub_bar_.load(std::memory_order_relaxed); }
  // Bumped every time a scene starts. The UI clears its hands on a change.
  uint32_t scene_changes() const { return pub_changes_.load(std::memory_order_relaxed); }
  // Bumped each time the conductor moves a strip's fader, so the UI can tell
  // its own fader moves from the scene's.
  uint32_t level_writes(bool bus, size_t strip) const;

  // --- audio thread ---------------------------------------------------------
  // Arms a scene for the next bar line, or cancels with kNone. With the
  // transport stopped there is no bar line: the scene lands on the next block.
  void arm(int scene);
  // The player took a control: the scene stops walking it until the next
  // change. Only a level has anything to stop; a gate or a pattern is set
  // once, on the line.
  // `tag` and `param` name the plugin parameter for a Param hand.
  void hand(bool bus, size_t strip, SceneTarget::What what, uint32_t tag = 0,
            uint32_t param = 0);

  // Once per block, after the transport is known and before the graph
  // renders, so a bar line inside this block is acted on in this block.
  void run(AudioGraph& graph, const TransportInfo& transport, uint32_t frames,
           double sample_rate);

 private:
  struct LevelRamp {
    bool active = false;
    float from_db = 0.0f;
    float to_db = 0.0f;
    float to_linear = 0.0f;
    uint64_t start = 0;   // on clock_
    uint64_t length = 0;  // samples
  };

  // A plugin parameter on its way somewhere, found again by tag each block
  // so a plugin removed mid-fade simply stops being walked.
  struct ParamRamp {
    bool active = false;
    bool bus = false;
    uint16_t strip = 0;
    uint32_t tag = 0;
    uint32_t param = 0;
    float from = 0.0f;
    float to = 0.0f;
    uint64_t start = 0;
    uint64_t length = 0;
  };
  static constexpr size_t kMaxParamRamps = 256;

  static ChannelStrip* strip_of(AudioGraph& graph, bool bus, size_t index);
  ParamRamp* param_ramp_for(const SceneTarget& target, bool claim);
  void walk_params(AudioGraph& graph, uint32_t frames);
  LevelRamp* ramp_of(bool bus, size_t index);
  void at_line(AudioGraph& graph, const SceneTable& table, int64_t line,
               uint32_t frame, double fade_unit_samples);
  void start(AudioGraph& graph, const SceneTable& table, int scene, int64_t line,
             uint32_t frame, double fade_samples);
  // The scene the next bar line will start if nothing else happens first.
  int upcoming(const SceneTable& table) const;
  void prepare_patterns(AudioGraph& graph, const SceneTable& table, int scene);
  void walk_levels(AudioGraph& graph, uint32_t frames);
  void publish_state();

  // UI side.
  std::shared_ptr<const SceneTable> owned_;
  dsp::RetiredList<const SceneTable> retired_;

  std::atomic<const SceneTable*> live_{nullptr};
  std::atomic<uint64_t> generation_{0};
  std::atomic<bool> auto_{true};
  std::atomic<bool> hold_{false};

  std::atomic<int> pub_current_{kNone};
  std::atomic<int> pub_armed_{kNone};
  std::atomic<int> pub_bar_{-1};
  std::atomic<uint32_t> pub_changes_{0};
  std::array<std::atomic<uint32_t>, kMaxChannels> channel_writes_{};
  std::array<std::atomic<uint32_t>, kMaxBuses> bus_writes_{};

  // Audio thread only.
  int current_ = kNone;
  int armed_ = kNone;
  bool apply_now_ = false;          // armed while stopped
  int64_t scene_line_ = -1;         // the bar line the current scene began on
  int64_t handled_line_ = -1;       // the last bar line acted on
  double last_beats_ = -1.0;
  uint64_t clock_ = 0;              // samples since the conductor started
  double sample_rate_ = 48000.0;
  int prepared_ = kNone;            // whose patterns wait in the sequencers
  std::array<LevelRamp, kMaxChannels> channel_ramps_{};
  std::array<LevelRamp, kMaxBuses> bus_ramps_{};
  std::array<ParamRamp, kMaxParamRamps> param_ramps_{};
};

}  // namespace nirbija
