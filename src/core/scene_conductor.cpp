// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "core/scene_conductor.h"

#include <algorithm>
#include <cmath>

#include "core/audio_graph.h"

namespace nirbija {

namespace {
// The Step Sequencer's own parameter ids (step_sequencer.cpp). A pattern
// target is only ever built for a sequencer, and the insert is found by its
// chain tag, so these never reach another plugin.
constexpr uint32_t kSeqPattern = 196;
constexpr uint32_t kSeqNextPattern = 197;  // 0 none, 1-16 pattern 0-15

// The bottom of the fader (Skin::kMinDb). A level walks in decibels, which is
// what the fader's travel is, so a fade looks like a hand pulling it.
constexpr float kFloorDb = -70.0f;
// A scene with no fade still slopes: the gate over the mute's time.
constexpr double kShortSeconds = 0.010;

float to_db(float linear) {
  return linear <= 0.0f ? kFloorDb : std::max(kFloorDb, 20.0f * std::log10(linear));
}

uint32_t id_at(const SceneTable* table, int index) {
  if (table == nullptr || index < 0 || index >= static_cast<int>(table->scenes.size()))
    return 0;
  return table->scenes[static_cast<size_t>(index)].id;
}

// Where the scene called `id`, last seen at `was`, sits in `table`. A table
// without ids is matched by position, as long as the position is still there.
int index_of(const SceneTable* table, uint32_t id, int was) {
  if (table == nullptr || was < 0) return SceneConductor::kNone;
  const int count = static_cast<int>(table->scenes.size());
  if (id == 0) return was < count ? was : SceneConductor::kNone;
  for (int i = 0; i < count; ++i)
    if (table->scenes[static_cast<size_t>(i)].id == id) return i;
  return SceneConductor::kNone;
}
}  // namespace

void SceneConductor::publish(std::shared_ptr<const SceneTable> table) {
  live_.store(table.get(), std::memory_order_release);
  retired_.retire(std::move(owned_), generation_.load(std::memory_order_acquire));
  owned_ = std::move(table);
}

void SceneConductor::reclaim(bool audio_running) {
  retired_.reclaim(generation_.load(std::memory_order_acquire), audio_running);
}

uint32_t SceneConductor::level_writes(bool bus, size_t strip) const {
  if (bus) return strip < kMaxBuses ? bus_writes_[strip].load(std::memory_order_relaxed) : 0;
  return strip < kMaxChannels ? channel_writes_[strip].load(std::memory_order_relaxed) : 0;
}

void SceneConductor::arm(int scene) {
  // Counted in the newest table, which may not be the one run() last saw:
  // kept by id, and placed again when run() catches up with the table.
  const SceneTable* table = live_.load(std::memory_order_acquire);
  const int count = table != nullptr ? static_cast<int>(table->scenes.size()) : 0;
  armed_ = scene < 0 || scene >= count ? kNone : scene;
  armed_id_ = id_at(table, armed_);
  apply_now_ = armed_ != kNone;  // only honoured while stopped; see run()
  pub_armed_.store(armed_, std::memory_order_relaxed);
}

void SceneConductor::hand(bool bus, size_t strip, SceneTarget::What what,
                          uint32_t tag, uint32_t param) {
  if (what == SceneTarget::What::Level) {
    if (LevelRamp* ramp = ramp_of(bus, strip)) ramp->active = false;
    return;
  }
  if (what != SceneTarget::What::Param) return;
  for (ParamRamp& ramp : param_ramps_)
    if (ramp.active && ramp.bus == bus && ramp.strip == strip && ramp.tag == tag &&
        ramp.param == param)
      ramp.active = false;
}

SceneConductor::ParamRamp* SceneConductor::param_ramp_for(const SceneTarget& target,
                                                          bool claim) {
  ParamRamp* free = nullptr;
  for (ParamRamp& ramp : param_ramps_) {
    if (ramp.active && ramp.bus == target.bus && ramp.strip == target.strip &&
        ramp.tag == target.insert_tag && ramp.param == target.param)
      return &ramp;
    if (!ramp.active && free == nullptr) free = &ramp;
  }
  return claim ? free : nullptr;
}

ChannelStrip* SceneConductor::strip_of(AudioGraph& graph, bool bus, size_t index) {
  return bus ? graph.live_bus(index) : graph.live_channel(index);
}

SceneConductor::LevelRamp* SceneConductor::ramp_of(bool bus, size_t index) {
  if (bus) return index < kMaxBuses ? &bus_ramps_[index] : nullptr;
  return index < kMaxChannels ? &channel_ramps_[index] : nullptr;
}

void SceneConductor::run(AudioGraph& graph, const TransportInfo& transport,
                         uint32_t frames, double sample_rate) {
  if (sample_rate > 0.0) sample_rate_ = sample_rate;
  const SceneTable* table = live_.load(std::memory_order_acquire);
  if (table != seen_) follow_table(table);

  const double tempo = transport.tempo_bpm;
  const int numerator = std::max(1, transport.numerator);
  const bool timed = transport.playing && tempo > 0.0 && sample_rate > 0.0;

  if (table != nullptr && !timed) {
    // No bar line to wait for: an armed scene lands now, on the short slopes,
    // and starts counting its bars from the first line once play begins.
    if (apply_now_ && armed_ != kNone) {
      start(graph, *table, armed_, -1, 0, 0.0);
      armed_ = kNone;
    }
  }
  apply_now_ = false;

  if (table != nullptr && timed) {
    const double beats = transport.beats;
    // Back to the top, or back in time: the first bar line from here is a
    // fresh one, whatever was handled before. From the top the song starts
    // over at the first scene - unless a scene was picked while stopped,
    // which is someone saying where to start.
    if ((transport.changed && beats == 0.0) || beats + 1e-9 < last_beats_) {
      handled_line_ = static_cast<int64_t>(std::ceil(beats / numerator - 1e-9)) - 1;
      if (beats == 0.0 && scene_line_ >= 0) {
        current_ = kNone;
        scene_line_ = -1;
      }
    }
    last_beats_ = beats;

    // The first bar line at or after the block's start. boundary_frame()
    // looks strictly ahead, which would miss a line sitting exactly on the
    // block's first frame; the handled counter is what stops one being seen
    // twice instead.
    const double beats_per_frame = tempo / 60.0 / sample_rate;
    const auto line = static_cast<int64_t>(std::ceil(beats / numerator - 1e-9));
    const double at = (static_cast<double>(line) * numerator - beats) / beats_per_frame;
    const double bar_samples = numerator / beats_per_frame;
    if (line > handled_line_ && at < static_cast<double>(frames)) {
      handled_line_ = line;
      at_line(graph, *table, line, static_cast<uint32_t>(std::max(0.0, at)), bar_samples);
    }
    if (scene_line_ >= 0 && current_ != kNone)
      pub_bar_.store(static_cast<int>(std::max<int64_t>(0, handled_line_ - scene_line_)),
                     std::memory_order_relaxed);

    // The sequencers switch pattern on their own next bar line, which is
    // this one's: whatever that line will start has its patterns queued
    // ahead. Not in the first blocks after a line, though. A sequencer counts
    // its bar from where its last block started, so for one block after a
    // line it still thinks the line is ahead of it and would switch at once.
    const double block_beats = frames * beats_per_frame;
    const double line_beats = static_cast<double>(handled_line_) * numerator;
    const bool settled = handled_line_ < 0 || beats - block_beats >= line_beats - 1e-9;
    const int next = upcoming(*table);
    if (settled && next != prepared_) prepare_patterns(graph, *table, next);
  }

  walk_levels(graph, frames);
  walk_params(graph, frames);
  clock_ += frames;
  remember_ids(table);
  publish_state();
  generation_.fetch_add(1, std::memory_order_release);
}

int SceneConductor::upcoming(const SceneTable& table) const {
  if (armed_ != kNone) return armed_;
  const int count = static_cast<int>(table.scenes.size());
  if (count == 0) return kNone;
  if (current_ == kNone) return auto_.load(std::memory_order_relaxed) ? 0 : kNone;
  if (scene_line_ < 0) return kNone;
  const auto& scene = table.scenes[static_cast<size_t>(current_)];
  if (scene.bars == 0) return kNone;
  if (!auto_.load(std::memory_order_relaxed) || hold_.load(std::memory_order_relaxed))
    return kNone;
  if (current_ + 1 >= count) return kNone;
  const int64_t next_line = handled_line_ + 1;
  return next_line - scene_line_ >= static_cast<int64_t>(scene.bars) ? current_ + 1 : kNone;
}

void SceneConductor::at_line(AudioGraph& graph, const SceneTable& table, int64_t line,
                             uint32_t frame, double bar_samples) {
  const int count = static_cast<int>(table.scenes.size());
  auto fade_of = [&](int scene) {
    return static_cast<double>(table.scenes[static_cast<size_t>(scene)].fade_bars) *
           bar_samples;
  };
  if (armed_ != kNone) {
    const int scene = armed_;
    armed_ = kNone;
    start(graph, table, scene, line, frame, fade_of(scene));
    return;
  }
  if (count == 0) return;
  if (current_ == kNone) {
    if (auto_.load(std::memory_order_relaxed)) start(graph, table, 0, line, frame, fade_of(0));
    return;
  }
  if (scene_line_ < 0) {  // applied while stopped: this is its first bar
    scene_line_ = line;
    return;
  }
  const auto& scene = table.scenes[static_cast<size_t>(current_)];
  if (scene.bars == 0 || line - scene_line_ < static_cast<int64_t>(scene.bars)) return;
  const bool walk = auto_.load(std::memory_order_relaxed) &&
                    !hold_.load(std::memory_order_relaxed) && current_ + 1 < count;
  if (walk) start(graph, table, current_ + 1, line, frame, fade_of(current_ + 1));
  else scene_line_ = line;  // held, or the list stopped walking: it plays again
}

void SceneConductor::start(AudioGraph& graph, const SceneTable& table, int scene,
                           int64_t line, uint32_t frame, double fade_samples) {
  current_ = scene;
  scene_line_ = line;
  pub_changes_.fetch_add(1, std::memory_order_relaxed);
  pub_bar_.store(line >= 0 ? 0 : -1, std::memory_order_relaxed);

  const auto& entry = table.scenes[static_cast<size_t>(scene)];
  const size_t end = std::min<size_t>(table.targets.size(),
                                      static_cast<size_t>(entry.first) + entry.count);
  for (size_t i = entry.first; i < end; ++i) {
    const SceneTarget& target = table.targets[i];
    ChannelStrip* strip = strip_of(graph, target.bus, target.strip);
    if (strip == nullptr) continue;
    switch (target.what) {
      case SceneTarget::What::Gate: {
        const double floor = kShortSeconds * sample_rate_;
        const auto length = static_cast<uint32_t>(std::max(floor, fade_samples));
        strip->start_scene_gate(target.value >= 0.5f, frame, length);
        break;
      }
      case SceneTarget::What::Level: {
        LevelRamp* ramp = ramp_of(target.bus, target.strip);
        if (ramp == nullptr) break;
        if (fade_samples < 1.0) {
          // The fader's own smoothing is the slope.
          ramp->active = false;
          strip->set_gain(target.value);
          if (target.bus) bus_writes_[target.strip].fetch_add(1, std::memory_order_relaxed);
          else channel_writes_[target.strip].fetch_add(1, std::memory_order_relaxed);
          break;
        }
        ramp->active = true;
        ramp->from_db = to_db(strip->gain_target());
        ramp->to_db = to_db(target.value);
        ramp->to_linear = target.value;
        ramp->start = clock_ + frame;
        ramp->length = static_cast<uint64_t>(fade_samples);
        break;
      }
      case SceneTarget::What::Param: {
        PluginInstance* plugin = strip->insert_by_tag(target.insert_tag);
        if (plugin == nullptr) break;
        if (target.stepped || fade_samples < 1.0) {
          // On the line, and nothing still walking it the other way.
          if (ParamRamp* ramp = param_ramp_for(target, false)) ramp->active = false;
          plugin->set_parameter_rt(target.param, target.value);
          break;
        }
        ParamRamp* ramp = param_ramp_for(target, true);
        if (ramp == nullptr) {  // more walks at once than there is room for
          plugin->set_parameter_rt(target.param, target.value);
          break;
        }
        // A walk already under way on this parameter goes on from where it
        // got to; otherwise from where the plugin is - which, for a plugin
        // whose value only the UI thread may read, the UI last saw.
        if (ramp->active) {
          const uint64_t at = clock_ + frame;
          const double t =
              at <= ramp->start ? 0.0
                                : std::min(1.0, static_cast<double>(at - ramp->start) /
                                                    static_cast<double>(std::max<uint64_t>(
                                                        1, ramp->length)));
          ramp->from = ramp->from + (ramp->to - ramp->from) * static_cast<float>(t);
        } else {
          ramp->from = target.polled && table.now != nullptr
                           ? table.now[i].load(std::memory_order_relaxed)
                           : static_cast<float>(plugin->parameter_value(target.param));
        }
        ramp->active = true;
        ramp->bus = target.bus;
        ramp->strip = target.strip;
        ramp->tag = target.insert_tag;
        ramp->param = target.param;
        ramp->to = target.value;
        ramp->start = clock_ + frame;
        ramp->length = static_cast<uint64_t>(fade_samples);
        break;
      }
      case SceneTarget::What::Pattern: {
        PluginInstance* seq = strip->insert_by_tag(target.insert_tag);
        if (seq == nullptr) break;
        if (frame == 0) {
          // The line is the block's first frame: the whole block is the new
          // bar, so the pattern changes before the sequencer looks.
          seq->set_parameter(kSeqPattern, target.value);
          seq->set_parameter(kSeqNextPattern, 0.0);
        } else {
          seq->set_parameter(kSeqNextPattern, target.value + 1.0);
        }
        break;
      }
    }
  }
  prepared_ = kNone;
}

void SceneConductor::prepare_patterns(AudioGraph& graph, const SceneTable& table,
                                      int scene) {
  auto each_pattern = [&](int which, auto&& fn) {
    if (which == kNone || which >= static_cast<int>(table.scenes.size())) return;
    const auto& entry = table.scenes[static_cast<size_t>(which)];
    const size_t end = std::min<size_t>(table.targets.size(),
                                        static_cast<size_t>(entry.first) + entry.count);
    for (size_t i = entry.first; i < end; ++i) {
      const SceneTarget& target = table.targets[i];
      if (target.what != SceneTarget::What::Pattern) continue;
      ChannelStrip* strip = strip_of(graph, target.bus, target.strip);
      if (strip == nullptr) continue;
      if (PluginInstance* seq = strip->insert_by_tag(target.insert_tag)) fn(seq, target);
    }
  };
  // Whatever was queued for a scene that is no longer coming is taken back.
  each_pattern(prepared_, [](PluginInstance* seq, const SceneTarget&) {
    seq->set_parameter(kSeqNextPattern, 0.0);
  });
  each_pattern(scene, [](PluginInstance* seq, const SceneTarget& target) {
    seq->set_parameter(kSeqNextPattern, target.value + 1.0);
  });
  prepared_ = scene;
}

void SceneConductor::walk_levels(AudioGraph& graph, uint32_t frames) {
  const uint64_t end = clock_ + frames;
  auto walk = [&](LevelRamp& ramp, bool bus, size_t index,
                  std::atomic<uint32_t>& writes) {
    if (!ramp.active || end <= ramp.start) return;
    ChannelStrip* strip = strip_of(graph, bus, index);
    if (strip == nullptr) {
      ramp.active = false;
      return;
    }
    // Where the fade will be at the end of this block; the fader's 15 ms
    // smoothing draws the line between one block's value and the next.
    const double t = std::min(1.0, static_cast<double>(end - ramp.start) /
                                       static_cast<double>(std::max<uint64_t>(1, ramp.length)));
    float gain = ramp.to_linear;
    if (t < 1.0) {
      const double db = ramp.from_db + (ramp.to_db - ramp.from_db) * t;
      gain = db <= kFloorDb ? 0.0f : static_cast<float>(std::pow(10.0, db / 20.0));
    } else {
      ramp.active = false;
    }
    strip->set_gain(gain);
    writes.fetch_add(1, std::memory_order_relaxed);
  };
  for (size_t i = 0; i < kMaxChannels; ++i)
    walk(channel_ramps_[i], false, i, channel_writes_[i]);
  for (size_t i = 0; i < kMaxBuses; ++i) walk(bus_ramps_[i], true, i, bus_writes_[i]);
}

void SceneConductor::walk_params(AudioGraph& graph, uint32_t frames) {
  const uint64_t end = clock_ + frames;
  for (ParamRamp& ramp : param_ramps_) {
    if (!ramp.active || end <= ramp.start) continue;
    ChannelStrip* strip = strip_of(graph, ramp.bus, ramp.strip);
    PluginInstance* plugin = strip != nullptr ? strip->insert_by_tag(ramp.tag) : nullptr;
    if (plugin == nullptr) {
      ramp.active = false;
      continue;
    }
    // Where the fade is at the end of this block, like the fader: a plugin
    // that smooths its own parameters (the FX Pad does, over 8 ms) turns the
    // block's steps into a line.
    const double t = std::min(1.0, static_cast<double>(end - ramp.start) /
                                       static_cast<double>(std::max<uint64_t>(1, ramp.length)));
    const float value = t >= 1.0 ? ramp.to
                                 : ramp.from + (ramp.to - ramp.from) * static_cast<float>(t);
    plugin->set_parameter_rt(ramp.param, value);
    if (t >= 1.0) ramp.active = false;
  }
}

void SceneConductor::follow_table(const SceneTable* table) {
  const int was_current = current_;
  const int count = table != nullptr ? static_cast<int>(table->scenes.size()) : 0;
  // armed_ may already count in this table (arm() saw it first); its id is
  // right either way.
  current_ = index_of(table, current_id_, current_);
  armed_ = index_of(table, armed_id_, armed_);
  prepared_ = index_of(table, prepared_id_, prepared_);
  // The scene playing was taken out from under the song. The strips stay
  // where it left them, and the scene that came after it - now in its place
  // - takes over on the next bar line, as if it had run its bars out. The
  // bar count the removed one had is gone with it, so waiting any longer
  // would be a guess.
  if (was_current != kNone && current_ == kNone && current_id_ != 0) {
    scene_line_ = -1;
    if (armed_ == kNone && auto_.load(std::memory_order_relaxed) &&
        !hold_.load(std::memory_order_relaxed) && was_current < count)
      armed_ = was_current;
  }
  seen_ = table;
  remember_ids(table);
}

void SceneConductor::remember_ids(const SceneTable* table) {
  current_id_ = id_at(table, current_);
  armed_id_ = id_at(table, armed_);
  prepared_id_ = id_at(table, prepared_);
}

void SceneConductor::publish_state() {
  pub_current_.store(current_, std::memory_order_relaxed);
  pub_armed_.store(armed_, std::memory_order_relaxed);
  if (current_ == kNone) pub_bar_.store(-1, std::memory_order_relaxed);
}

}  // namespace nirbija
