// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "core/dsp.h"
#include "core/plugin.h"

namespace nirbija {

// A performance pad: sixteen effects, held like keys. Each pad is an amount,
// not a switch — the character of the effect tracks how far it is pushed.
// Pitch, Filter, Comb and Ring are bipolar (centre off, up and down go
// opposite ways). HOLD latches whatever is down so both hands can leave the
// glass. Tempo-synced pads (stutter, gate, cutter, delays) read the same grid
// the looper does.
class FxPadInstance : public PluginInstance {
 public:
  static constexpr int kPads = 16;

  enum Pad : int {
    Crush = 0,
    Pitch,
    Comb,
    Ring,
    Reverb,
    Stutter,
    Gate,
    Filter,
    Cutter,
    Reverse,
    Dub,
    TempoDelay,
    Talkbox,
    Vibroflange,
    Dirty,
    Compressor,
  };

  FxPadInstance();

  static PluginDescriptor make_descriptor();
  static const char* pad_name(int pad);
  static bool pad_bipolar(int pad);

  void set_channel_layout(int channels) override { channels_ = channels; }
  bool activate(double sample_rate, uint32_t max_block_frames) override;
  void deactivate() override {}

  void set_transport(const TransportInfo& transport) override {
    transport_ = transport;
  }
  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override;

  std::vector<ParameterInfo> parameters() const override;
  double parameter_value(uint32_t id) const override;
  void set_parameter(uint32_t id, double value) override;

  std::vector<uint8_t> save_state() const override;
  bool load_state(const std::vector<uint8_t>& blob) override;

  const PluginDescriptor& descriptor() const override { return descriptor_; }

  bool pad_on(int pad) const;
  void set_pad(int pad, bool on);
  float pad_amount(int pad) const;
  void set_pad_amount(int pad, float amount);
  bool hold() const { return hold_.load(std::memory_order_relaxed); }
  void set_hold(bool on);

  // How long an on/off edge (gate, cutter) or a seam (stutter, reverse) is
  // ramped or crossfaded over. One and a half milliseconds: still a chop to
  // the ear, no longer a click.
  static constexpr double kEdgeSeconds = 0.0015;

 private:
  static constexpr uint32_t kHoldId = 16;

  struct Delay {
    std::vector<float> data;
    size_t w = 0;
    void setup(size_t n);
    void push(float x);
    float tap(size_t delay) const;
    float tap_lerp(float delay) const;
  };

  // Everything process_sample needs that only changes once per block: the
  // wet weights and every coefficient derived from an amount or the tempo.
  // The amounts themselves only move once per block (mix_), so per-sample
  // exp2 and divisions were recomputing the same numbers 256 times.
  struct BlockCoefs {
    float wet[kPads] = {};
    uint32_t crush_period = 2;
    float crush_steps = 48.0f;
    float pitch_rate = 1.0f;
    float pitch_grain = 64.0f;
    float comb_delay = 2.0f;
    float comb_fb = 0.5f;
    float ring_inc = 0.0f;
    float reverb_decay = 0.5f;
    uint32_t stutter_play = 32;
    uint32_t stutter_xf = 0;
    double gate_step = 0.0;
    float gate_duty = 0.5f;
    float filter_f = 0.01f;
    float filter_q = 0.2f;
    bool filter_low = true;
    double cutter_step = 0.0;
    float cutter_duty = 0.5f;
    float dub_delay = 2.0f;
    float dub_fb = 0.35f;
    float echo_delay = 2.0f;
    float echo_fb = 0.18f;
    float talk_freqs[3] = {};
    float vib_inc = 0.0f;
    float vib_depth = 6.0f;
    float dirty_drive = 1.4f;
    float comp_thresh = 0.55f;
    float comp_makeup = 1.05f;
  };

  void attack(int pad);
  void prepare_block(uint32_t frames);
  void process_sample(float* left, float* right);

  PluginDescriptor descriptor_;
  int channels_ = 2;
  double sample_rate_ = 48000.0;
  TransportInfo transport_;

  std::array<std::atomic<float>, kPads> amount_{};
  std::atomic<bool> hold_{false};

  // Smoothed amount, audio thread. Signed for the bipolar pads. A pad that
  // slams in clicks, so this is what process_sample reads, not the atomic.
  std::array<float, kPads> mix_{};
  std::array<bool, kPads> was_on_{};
  BlockCoefs co_;

  Delay hist_[2];
  Delay delay_[2];
  Delay echo_[2];
  Delay reverse_[2];
  Delay stutter_[2];
  Delay reverb_comb_[4][2];
  Delay reverb_ap_[2][2];
  Delay flange_[2];
  Delay comb_[2];

  float crush_hold_[2] = {};
  uint32_t crush_count_ = 0;
  float ring_phase_ = 0.0f;
  float pitch_pos_[2] = {};
  float filter_lp_[2] = {};
  float filter_bp_[2] = {};
  float talk_lp_[3][2] = {};
  float env_peak_ = 0.0f;
  float comp_gain_ = 1.0f;
  float vib_phase_ = 0.0f;
  float talk_phase_ = 0.0f;
  double gate_phase_ = 0.0;
  double cutter_phase_ = 0.0;
  dsp::LinearRamp gate_ramp_;
  dsp::LinearRamp cutter_ramp_;
  uint32_t edge_frames_ = 72;
  uint32_t stutter_cap_ = 0;       // frames of slice the pad can play
  uint32_t stutter_captured_ = 0;  // frames actually captured, cap + seam
  uint32_t stutter_pos_ = 0;
  bool stutter_first_ = true;
  size_t reverse_play_ = 0;
  size_t reverse_alt_ = 0;  // the fresh tap fading in as the old one runs out
};

}  // namespace nirbija
