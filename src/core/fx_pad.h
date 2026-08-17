#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// A performance pad: sixteen effects, held like keys. HOLD latches whatever
// is down so both hands can leave the glass. Tempo-synced pads (stutter,
// gate, cutter, delays) read the same grid the looper does.
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
  bool hold() const { return hold_.load(std::memory_order_relaxed); }
  void set_hold(bool on);

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

  void attack(int pad);
  void process_sample(float* left, float* right);

  PluginDescriptor descriptor_;
  int channels_ = 2;
  double sample_rate_ = 48000.0;
  TransportInfo transport_;

  std::array<std::atomic<bool>, kPads> pad_{};
  std::atomic<bool> hold_{false};

  // Smoothed wet, audio thread. A pad that slams in clicks.
  std::array<float, kPads> mix_{};
  std::array<bool, kPads> was_on_{};

  Delay hist_[2];
  Delay delay_[2];
  Delay reverse_[2];
  Delay stutter_[2];
  Delay reverb_comb_[4][2];
  Delay reverb_ap_[2][2];
  Delay flange_[2];

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
  uint32_t stutter_len_ = 0;
  uint32_t stutter_pos_ = 0;
  size_t reverse_play_ = 0;
};

}  // namespace nirbija
