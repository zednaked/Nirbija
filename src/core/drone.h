#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// A drone instrument: six strings that never stop sounding. Each string is a
// pitch some interval from one root, detuned by a few cents so the set beats
// against itself, and left to drift slowly - the way a tanpura, a shruti box
// or a stack of held organ notes does when nobody touches it for a minute.
//
// There is no note-on and no note-off. The performer rides one thing, the
// swell, which brings the whole drone up and down over seconds rather than
// milliseconds. A MIDI note re-roots the drone and the strings glide there;
// releasing the key changes nothing, because a drone that stopped when you let
// go would be a synth.
//
// Everything continuous is smoothed per sample and every pitch glides, so a
// string can be dragged while it sounds without a click - the one rule that
// matters more for this instrument than for any other in the host, since the
// thing it makes is a sustained tone and a sustained tone shows every seam.
class DroneInstance : public PluginInstance {
 public:
  static constexpr int kVoices = 6;

  // Per voice, four parameters at kVoiceStride * voice + Param.
  enum VoiceParam : uint32_t { Interval = 0, Detune, Level, Shape };
  static constexpr uint32_t kVoiceStride = 4;

  // Globals, after the voices.
  enum GlobalParam : uint32_t {
    Swell = kVoices * kVoiceStride,  // 24
    Rise,        // seconds the swell takes, either way
    Root,        // MIDI note
    Just,        // 1: intervals are just ratios, 0: twelve-tone equal
    Drift,       // how far pitch and level wander on their own
    Tide,        // how fast they wander, and the filter breathes
    Cutoff,
    Resonance,
    Motion,      // how far the filter breathes with the tide
    Space,       // reverb: wet and decay together
    Grit,        // saturation before the filter
    Width,       // left and right detuned against each other
    Glide,       // how long a root change takes to arrive
    kParamCount
  };

  DroneInstance();

  static PluginDescriptor make_descriptor();
  static const char* param_name(uint32_t id);
  static double param_min(uint32_t id);
  static double param_max(uint32_t id);
  static double param_default(uint32_t id);

  // The frequency multiplier an interval stands for. Just intonation keeps a
  // fifth at exactly 3/2 so two strings a fifth apart lock instead of beating
  // slowly against the temperament - which is most of why drones are tuned
  // that way.
  static double interval_ratio(int semitones, bool just);
  static double midi_to_hz(double note);

  // A set of strings with a name. Presets are the strings and the tuning
  // only: root, swell and the weather are the performance and stay where
  // they were, so a preset can be changed under a drone that is sounding.
  struct Preset {
    const char* name;
    int interval[kVoices];
    double detune[kVoices];
    double level[kVoices];
    double shape[kVoices];
    bool just;
  };
  static int preset_count();
  static const Preset& preset(int index);
  // UI thread. Out of range does nothing.
  void apply_preset(int index);

  // PluginInstance ------------------------------------------------------------
  void set_channel_layout(int channels) override { channels_ = channels; }
  bool activate(double sample_rate, uint32_t max_block_frames) override;
  void deactivate() override {}

  void queue_midi(const MidiEvent& event) override;
  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override;

  std::vector<ParameterInfo> parameters() const override;
  double parameter_value(uint32_t id) const override;
  void set_parameter(uint32_t id, double value) override;

  std::vector<uint8_t> save_state() const override;
  bool load_state(const std::vector<uint8_t>& blob) override;

  const PluginDescriptor& descriptor() const override { return descriptor_; }

  // --- readouts for the editor, UI thread, written by the audio thread ------
  // What each string is actually putting out right now, drift and swell
  // included, 0..1ish. For drawing the string moving.
  float voice_gain(int voice) const;
  // Where the swell has got to, 0..1. Lags the parameter by `rise` seconds.
  float swell_position() const { return env_out_.load(std::memory_order_relaxed); }
  // The filter's breathing, -1..1, for a dot that wanders across the pad.
  float filter_breath() const { return breath_out_.load(std::memory_order_relaxed); }
  // Peak of the last block, for the glow.
  float peak() const { return peak_out_.load(std::memory_order_relaxed); }
  // The root as it sounds this instant, gliding, in MIDI note units.
  double root_now() const { return root_out_.load(std::memory_order_relaxed); }

 private:
  struct Walker {
    float value = 0.0f;
    float target = 0.0f;
    float countdown = 0.0f;  // seconds until the next target
  };

  struct Voice {
    double phase[2] = {0.0, 0.0};
    float gain = 0.0f;      // smoothed level, per sample
    double pitch = 0.0;     // smoothed pitch in semitones from root, per sample
    Walker pitch_walk;
    Walker level_walk;
  };

  struct Comb {
    std::vector<float> data;
    size_t w = 0;
    void setup(size_t n);
  };

  struct Allpass {
    std::vector<float> data;
    size_t w = 0;
    void setup(size_t n);
  };

  static float osc(double phase, double inc, float shape);
  void step_walker(Walker& w, float seconds_per_sample, float period_seconds,
                   float coeff);
  float reverb(int ch, float in);
  uint32_t rng_next();

  PluginDescriptor descriptor_;
  int channels_ = 2;
  double sample_rate_ = 48000.0;

  std::array<std::atomic<double>, kParamCount> params_{};

  // --- audio thread only -----------------------------------------------------
  std::array<Voice, kVoices> voices_{};
  float env_ = 0.0f;         // swell position, ramps linearly over `rise`
  double root_ = 38.0;       // gliding root, semitones
  float cutoff_log_ = 0.0f;  // smoothed log2(cutoff hz)
  float res_ = 0.0f;
  float shape_smooth_[kVoices] = {};
  float grit_ = 0.0f;
  float space_ = 0.0f;
  float width_ = 0.0f;
  float motion_ = 0.0f;
  double breath_phase_ = 0.0;
  Walker breath_walk_;
  float svf_lp_[2] = {};
  float svf_bp_[2] = {};
  uint32_t rng_ = 0x9e3779b9u;

  static constexpr int kCombs = 8;
  Comb comb_[2][kCombs];
  Allpass ap_[2][2];
  float damp_state_[2][kCombs] = {};

  std::array<std::atomic<float>, kVoices> gain_out_{};
  std::atomic<float> env_out_{0.0f};
  std::atomic<float> breath_out_{0.0f};
  std::atomic<float> peak_out_{0.0f};
  std::atomic<double> root_out_{38.0};
};

}  // namespace nirbija
