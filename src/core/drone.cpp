#include "core/drone.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace nirbija {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// The twelve semitones above a root as small-integer ratios. Five-limit,
// which is what a harmonium, a tanpura and most drone records are tuned to;
// the tritone and the minor seventh have several defensible spellings, and
// these are the ones that sit still against a fifth and a major third.
constexpr double kJust[12] = {
    1.0,          16.0 / 15.0, 9.0 / 8.0, 6.0 / 5.0,
    5.0 / 4.0,    4.0 / 3.0,   45.0 / 32.0, 3.0 / 2.0,
    8.0 / 5.0,    5.0 / 3.0,   9.0 / 5.0,  15.0 / 8.0,
};

// Smooths the step of a naive saw so the top does not alias into a hiss.
// Two polynomial segments, one either side of the wrap.
float polyblep(double t, double dt) {
  if (t < dt) {
    t /= dt;
    return static_cast<float>(t + t - t * t - 1.0);
  }
  if (t > 1.0 - dt) {
    t = (t - 1.0) / dt;
    return static_cast<float>(t * t + t + t + 1.0);
  }
  return 0.0f;
}

// One-pole coefficient for a time constant, per update of `interval` seconds.
float coeff_for(double tau_seconds, double interval_seconds) {
  if (tau_seconds <= 0.0) return 1.0f;
  return static_cast<float>(1.0 - std::exp(-interval_seconds / tau_seconds));
}

// Slow controls run on an exponential scale: the difference between 0.05 s
// and 0.1 s of glide is audible, the difference between 20 s and 20.05 s is
// not, and a linear knob would spend nine tenths of its travel up there.
double tide_hz(double tide) { return 0.01 * std::exp2(tide * 6.64); }  // 0.01..1 Hz
double glide_seconds(double glide) { return 0.05 * std::exp2(glide * 7.3); }  // 0.05..8 s
double cutoff_hz(double cutoff) { return 40.0 * std::exp2(cutoff * 8.64); }  // 40..16k

}  // namespace

// --- static tables -----------------------------------------------------------

const char* DroneInstance::param_name(uint32_t id) {
  static constexpr const char* kVoiceNames[kVoiceStride] = {"Interval", "Detune",
                                                            "Level", "Shape"};
  static constexpr const char* kGlobalNames[] = {
      "Swell", "Rise",   "Root",   "Just",  "Drift", "Tide", "Cutoff",
      "Resonance", "Motion", "Space", "Grit", "Width", "Glide"};
  if (id < Swell) {
    // "String 1 Level": the number is one-based because the editor labels the
    // strings that way and a mapping list should read the same.
    static thread_local char buffer[32];
    const uint32_t voice = id / kVoiceStride + 1;
    const uint32_t which = id % kVoiceStride;
    std::snprintf(buffer, sizeof(buffer), "String %u %s", voice,
                  kVoiceNames[which]);
    return buffer;
  }
  if (id < kParamCount) return kGlobalNames[id - Swell];
  return "";
}

double DroneInstance::param_min(uint32_t id) {
  if (id < Swell) {
    switch (id % kVoiceStride) {
      case Interval: return -24.0;
      case Detune: return -50.0;
      default: return 0.0;
    }
  }
  switch (id) {
    case Rise: return 0.05;
    case Root: return 24.0;
    default: return 0.0;
  }
}

double DroneInstance::param_max(uint32_t id) {
  if (id < Swell) {
    switch (id % kVoiceStride) {
      case Interval: return 24.0;
      case Detune: return 50.0;
      default: return 1.0;
    }
  }
  switch (id) {
    case Rise: return 30.0;
    case Root: return 84.0;
    default: return 1.0;
  }
}

double DroneInstance::param_default(uint32_t id) {
  // A drone in D: the root, its sub-octave, a fifth, two octaves up and a
  // twelfth. Every string except the root a few cents off so the set beats
  // slowly against itself from the first second. Lower strings are rounder,
  // upper strings brighter, which is how a real set of strings behaves.
  struct VoiceDefault {
    double interval, detune, level, shape;
  };
  static constexpr VoiceDefault kVoices6[kVoices] = {
      {0.0, 0.0, 0.90, 0.55},   {-12.0, 2.0, 0.70, 0.35},
      {7.0, -4.0, 0.60, 0.50},  {12.0, 3.0, 0.50, 0.60},
      {19.0, 6.0, 0.35, 0.70},  {24.0, -2.0, 0.25, 0.80},
  };
  if (id < Swell) {
    const VoiceDefault& v = kVoices6[id / kVoiceStride];
    switch (id % kVoiceStride) {
      case Interval: return v.interval;
      case Detune: return v.detune;
      case Level: return v.level;
      default: return v.shape;
    }
  }
  switch (id) {
    // Sounding from the moment it is inserted, rising over Rise. It started
    // at zero, and a strip with a silent instrument on it reads as broken
    // before it reads as waiting.
    case Swell: return 0.75;
    case Rise: return 4.0;
    case Root: return 38.0;  // D2
    case Just: return 1.0;
    case Drift: return 0.3;
    case Tide: return 0.3;
    case Cutoff: return 0.55;
    case Resonance: return 0.25;
    case Motion: return 0.35;
    case Space: return 0.6;
    case Grit: return 0.15;
    case Width: return 0.6;
    case Glide: return 0.4;
    default: return 0.0;
  }
}

namespace {

// Six sets of strings. The names say what they are for, not what they
// contain: someone who wants a tanpura should not have to know that one is
// the fifth below, three are the tonic and one is the octave.
constexpr DroneInstance::Preset kPresets[] = {
    // The factory set: root, sub, fifth, octave, twelfth, double octave.
    {"Open fifths",
     {0, -12, 7, 12, 19, 24},
     {0.0, 2.0, -4.0, 3.0, 6.0, -2.0},
     {0.90, 0.70, 0.60, 0.50, 0.35, 0.25},
     {0.55, 0.35, 0.50, 0.60, 0.70, 0.80},
     true},
    // Pa Sa Sa Sa': the fifth below, three tonics a few cents apart, the
    // octave. Bright, for the jawari buzz.
    {"Tanpura",
     {-5, 0, 0, 0, 12, -12},
     {0.0, -3.0, 0.0, 4.0, 2.0, 0.0},
     {0.65, 0.85, 0.85, 0.80, 0.30, 0.50},
     {0.85, 0.80, 0.80, 0.80, 0.70, 0.60},
     true},
    // Two octaves down to two up, nothing but the tonic. Sines and
    // triangles, tiny detune, weight at the bottom.
    {"Sub and octaves",
     {-24, -12, 0, 12, 24, 0},
     {0.0, 1.0, 0.0, -2.0, 3.0, 2.0},
     {0.80, 0.90, 0.85, 0.45, 0.20, 0.60},
     {0.05, 0.15, 0.30, 0.30, 0.10, 0.40},
     true},
    // The first six harmonics, levels falling as 1/n. Just tuning makes
    // them exact: 1, 2, 3, 4, 5, 6 times the lowest string.
    {"Harmonic series",
     {-12, 0, 7, 12, 16, 19},
     {0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
     {1.00, 0.50, 0.33, 0.25, 0.20, 0.17},
     {0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
     true},
    // Major triad spread over two octaves, wider detune, saws on top.
    {"Major shimmer",
     {0, 4, 7, 12, 16, 19},
     {0.0, 5.0, -4.0, 3.0, -6.0, 5.0},
     {0.85, 0.55, 0.65, 0.50, 0.35, 0.40},
     {0.50, 0.60, 0.55, 0.70, 0.80, 0.75},
     true},
    // Minor with the seventh, round shapes, slow beating.
    {"Minor fog",
     {-12, 0, 3, 7, 10, 12},
     {0.0, 0.0, 6.0, -5.0, 4.0, -3.0},
     {0.70, 0.85, 0.55, 0.60, 0.40, 0.35},
     {0.30, 0.45, 0.50, 0.45, 0.55, 0.50},
     true},
    // Three tonics and two octaves fanned out in cents: a chorus of one
    // note, the beating itself the sound.
    {"Unison beat",
     {0, 0, 0, 12, 12, -12},
     {-7.0, 0.0, 7.0, -4.0, 4.0, 0.0},
     {0.80, 0.90, 0.80, 0.40, 0.40, 0.60},
     {0.65, 0.65, 0.65, 0.60, 0.60, 0.35},
     false},
};

}  // namespace

int DroneInstance::preset_count() {
  return static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
}

const DroneInstance::Preset& DroneInstance::preset(int index) {
  return kPresets[std::clamp(index, 0, preset_count() - 1)];
}

void DroneInstance::apply_preset(int index) {
  if (index < 0 || index >= preset_count()) return;
  const Preset& p = kPresets[index];
  for (int v = 0; v < kVoices; ++v) {
    const uint32_t base = static_cast<uint32_t>(v) * kVoiceStride;
    set_parameter(base + Interval, p.interval[v]);
    set_parameter(base + Detune, p.detune[v]);
    set_parameter(base + Level, p.level[v]);
    set_parameter(base + Shape, p.shape[v]);
  }
  set_parameter(Just, p.just ? 1.0 : 0.0);
}

double DroneInstance::interval_ratio(int semitones, bool just) {
  if (!just) return std::exp2(semitones / 12.0);
  int octave = semitones / 12;
  int cls = semitones % 12;
  if (cls < 0) {
    cls += 12;
    --octave;
  }
  return std::ldexp(kJust[cls], octave);
}

double DroneInstance::midi_to_hz(double note) {
  return 440.0 * std::exp2((note - 69.0) / 12.0);
}

PluginDescriptor DroneInstance::make_descriptor() {
  PluginDescriptor descriptor;
  descriptor.format = PluginFormat::Internal;
  descriptor.uid = "nirbija.drone";
  descriptor.name = "Drone";
  descriptor.vendor = "Nirbija";
  descriptor.audio_inputs = 0;
  descriptor.audio_outputs = 2;
  descriptor.has_midi_input = true;
  descriptor.category = "Drone";
  descriptor.kind = PluginKind::Instrument;
  return descriptor;
}

DroneInstance::DroneInstance() : descriptor_(make_descriptor()) {
  for (uint32_t id = 0; id < kParamCount; ++id)
    params_[id].store(param_default(id), std::memory_order_relaxed);
  for (auto& g : gain_out_) g.store(0.0f, std::memory_order_relaxed);
  root_ = param_default(Root);
  root_out_.store(root_, std::memory_order_relaxed);
}

// --- delay lines ---------------------------------------------------------------

void DroneInstance::Comb::setup(size_t n) {
  data.assign(std::max(n, size_t{2}), 0.0f);
  w = 0;
}

void DroneInstance::Allpass::setup(size_t n) {
  data.assign(std::max(n, size_t{2}), 0.0f);
  w = 0;
}

bool DroneInstance::activate(double sample_rate, uint32_t) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;

  // Comb lengths in milliseconds, mutually prime-ish so no two echoes line
  // up, spread wide so the tail is a wash rather than a flutter. The right
  // side is offset so the two channels decorrelate instead of summing to
  // mono in the reverb.
  static constexpr double kCombMs[kCombs] = {31.1, 37.7, 43.3, 47.9,
                                             53.9, 61.3, 67.9, 73.1};
  static constexpr double kApMs[2] = {5.0, 12.6};
  for (int ch = 0; ch < 2; ++ch) {
    const double offset = ch == 0 ? 0.0 : 1.7;
    for (int c = 0; c < kCombs; ++c)
      comb_[ch][c].setup(
          static_cast<size_t>(sample_rate_ * (kCombMs[c] + offset) * 0.001));
    for (int a = 0; a < 2; ++a)
      ap_[ch][a].setup(
          static_cast<size_t>(sample_rate_ * (kApMs[a] + offset * 0.2) * 0.001));
    svf_lp_[ch] = svf_bp_[ch] = 0.0f;
    for (int c = 0; c < kCombs; ++c) damp_state_[ch][c] = 0.0f;
  }

  for (int v = 0; v < kVoices; ++v) {
    Voice& voice = voices_[static_cast<size_t>(v)];
    // A different starting phase per string and per side, so six strings do
    // not all cross zero together on the first sample and stack a click.
    voice.phase[0] = 0.61 * v;
    voice.phase[0] -= std::floor(voice.phase[0]);
    voice.phase[1] = 0.37 * (v + 1);
    voice.phase[1] -= std::floor(voice.phase[1]);
    voice.gain = 0.0f;
    const uint32_t base = static_cast<uint32_t>(v) * kVoiceStride;
    const bool just = params_[Just].load(std::memory_order_relaxed) >= 0.5;
    const int interval = static_cast<int>(
        std::lround(params_[base + Interval].load(std::memory_order_relaxed)));
    voice.pitch = 12.0 * std::log2(interval_ratio(interval, just)) +
                  params_[base + Detune].load(std::memory_order_relaxed) / 100.0;
    voice.pitch_walk = {};
    voice.level_walk = {};
    shape_smooth_[v] =
        static_cast<float>(params_[base + Shape].load(std::memory_order_relaxed));
  }

  env_ = 0.0f;
  root_ = params_[Root].load(std::memory_order_relaxed);
  cutoff_log_ = static_cast<float>(
      std::log2(cutoff_hz(params_[Cutoff].load(std::memory_order_relaxed))));
  res_ = static_cast<float>(params_[Resonance].load(std::memory_order_relaxed));
  grit_ = static_cast<float>(params_[Grit].load(std::memory_order_relaxed));
  space_ = static_cast<float>(params_[Space].load(std::memory_order_relaxed));
  width_ = static_cast<float>(params_[Width].load(std::memory_order_relaxed));
  motion_ = static_cast<float>(params_[Motion].load(std::memory_order_relaxed));
  breath_phase_ = 0.0;
  breath_walk_ = {};
  rng_ = 0x9e3779b9u;
  return true;
}

// --- MIDI ----------------------------------------------------------------------

void DroneInstance::queue_midi(const MidiEvent& event) {
  // A note re-roots the drone; nothing else about the message matters. The
  // strings glide there at their own pace, so a chord played in a hurry
  // arrives as a slow slide to whichever key came last - which is the right
  // thing for a drone and would be the wrong thing for anything else.
  if (event.size < 3) return;
  const uint8_t status = event.data[0] & 0xf0u;
  if (status != 0x90u || event.data[2] == 0) return;
  const double note = std::clamp(static_cast<double>(event.data[1]),
                                 param_min(Root), param_max(Root));
  params_[Root].store(note, std::memory_order_relaxed);
}

// --- the audio -------------------------------------------------------------------

uint32_t DroneInstance::rng_next() {
  // xorshift32. Not for anything but a slow wander.
  uint32_t x = rng_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_ = x;
  return x;
}

void DroneInstance::step_walker(Walker& w, float seconds, float period,
                                float coeff) {
  w.countdown -= seconds;
  if (w.countdown <= 0.0f) {
    const float r1 = static_cast<float>(rng_next() >> 8) / 16777216.0f;
    const float r2 = static_cast<float>(rng_next() >> 8) / 16777216.0f;
    w.target = r1 * 2.0f - 1.0f;
    // Around the period rather than exactly on it, so six strings do not
    // all change their minds on the same beat.
    w.countdown = period * (0.5f + r2);
  }
  w.value += coeff * (w.target - w.value);
}

float DroneInstance::osc(double phase, double inc, float shape) {
  // shape 0 is a sine, 0.5 a triangle, 1 a saw; in between is a crossfade.
  // A drone lives on its overtones, and this is the one knob that says how
  // many there are.
  const float sine = static_cast<float>(std::sin(kTwoPi * phase));
  const float tri = static_cast<float>(4.0 * std::fabs(phase - 0.5) - 1.0);
  const float saw =
      static_cast<float>(2.0 * phase - 1.0) - polyblep(phase, inc);
  if (shape < 0.5f) {
    const float t = shape * 2.0f;
    return sine + t * (tri - sine);
  }
  const float t = (shape - 0.5f) * 2.0f;
  return tri + t * (saw - tri);
}

float DroneInstance::reverb(int ch, float in) {
  // Eight damped combs in parallel into two allpasses in series - Schroeder,
  // by way of freeverb. Feedback goes up to 0.98, which on a 70 ms comb is a
  // tail the better part of half a minute long; that is not a bug, it is
  // what Space at the top is for.
  const float fb = 0.70f + 0.28f * space_;
  const float damp = 0.35f;
  float acc = 0.0f;
  for (int c = 0; c < kCombs; ++c) {
    Comb& comb = comb_[ch][c];
    const float out = comb.data[comb.w];
    float& lp = damp_state_[ch][c];
    lp = out + damp * (lp - out);
    comb.data[comb.w] = in + lp * fb + 1e-20f;  // the 1e-20 keeps denormals away
    comb.w = (comb.w + 1) % comb.data.size();
    acc += out;
  }
  acc *= 0.25f;
  for (int a = 0; a < 2; ++a) {
    Allpass& ap = ap_[ch][a];
    const float d = ap.data[ap.w];
    const float y = acc - 0.5f * d;
    ap.data[ap.w] = y + 1e-20f;
    ap.w = (ap.w + 1) % ap.data.size();
    acc = d + 0.5f * y;
  }
  return acc;
}

void DroneInstance::process(const float* const*, float* const* outputs,
                            uint32_t frames) {
  const int width = std::min(channels_, 2);
  if (width < 1 || frames == 0) return;
  const double sr = sample_rate_;
  const double block_seconds = frames / sr;
  const double sample_seconds = 1.0 / sr;

  // --- read the parameters once per block --------------------------------------
  const bool just = params_[Just].load(std::memory_order_relaxed) >= 0.5;
  const double root_target = params_[Root].load(std::memory_order_relaxed);
  const float swell_target =
      static_cast<float>(params_[Swell].load(std::memory_order_relaxed));
  const double rise =
      std::max(0.01, params_[Rise].load(std::memory_order_relaxed));
  const float drift =
      static_cast<float>(params_[Drift].load(std::memory_order_relaxed));
  const double tide = params_[Tide].load(std::memory_order_relaxed);
  const double glide = glide_seconds(params_[Glide].load(std::memory_order_relaxed));

  // Slow ones step once per block toward where the knob is. Sixty
  // milliseconds is quick enough to follow a drag and slow enough that a
  // filter coefficient recomputed per block does not step audibly.
  const float slow = coeff_for(0.06, block_seconds);
  const float cutoff_target = static_cast<float>(
      std::log2(cutoff_hz(params_[Cutoff].load(std::memory_order_relaxed))));
  cutoff_log_ += slow * (cutoff_target - cutoff_log_);
  res_ += slow * (static_cast<float>(
                      params_[Resonance].load(std::memory_order_relaxed)) -
                  res_);
  grit_ += slow * (static_cast<float>(params_[Grit].load(std::memory_order_relaxed)) - grit_);
  space_ += slow * (static_cast<float>(params_[Space].load(std::memory_order_relaxed)) - space_);
  width_ += slow * (static_cast<float>(params_[Width].load(std::memory_order_relaxed)) - width_);
  motion_ += slow * (static_cast<float>(params_[Motion].load(std::memory_order_relaxed)) - motion_);

  // --- the tide: everything that wanders, wanders at this rate -------------------
  const float period = static_cast<float>(1.0 / tide_hz(tide));
  const float walk_coeff = coeff_for(period * 0.5, block_seconds);
  step_walker(breath_walk_, static_cast<float>(block_seconds), period, walk_coeff);
  breath_phase_ += tide_hz(tide) * block_seconds;
  breath_phase_ -= std::floor(breath_phase_);
  const float breath = 0.7f * static_cast<float>(std::sin(kTwoPi * breath_phase_)) +
                       0.3f * breath_walk_.value;
  breath_out_.store(breath, std::memory_order_relaxed);

  // --- filter coefficients, per block (TPT state-variable, stable for any cutoff)
  const double fc = std::clamp(
      static_cast<double>(std::exp2(cutoff_log_ + motion_ * breath * 2.5f)),
      20.0, 0.45 * sr);
  const float g = static_cast<float>(std::tan(kPi * fc / sr));
  const float k = 2.0f - 1.9f * std::clamp(res_, 0.0f, 1.0f);
  const float a1 = 1.0f / (1.0f + g * (g + k));
  const float a2 = g * a1;
  const float a3 = g * a2;

  // --- per-voice targets ---------------------------------------------------------
  float pitch_target[kVoices];
  float level_target[kVoices];
  float pan_l[kVoices], pan_r[kVoices];
  double spread[kVoices];
  for (int v = 0; v < kVoices; ++v) {
    Voice& voice = voices_[static_cast<size_t>(v)];
    const uint32_t base = static_cast<uint32_t>(v) * kVoiceStride;
    step_walker(voice.pitch_walk, static_cast<float>(block_seconds), period,
                walk_coeff);
    step_walker(voice.level_walk, static_cast<float>(block_seconds),
                period * 1.3f, walk_coeff);
    const int interval = static_cast<int>(
        std::lround(params_[base + Interval].load(std::memory_order_relaxed)));
    const double detune = params_[base + Detune].load(std::memory_order_relaxed);
    pitch_target[v] = static_cast<float>(
        12.0 * std::log2(interval_ratio(interval, just)) + detune / 100.0 +
        drift * 0.12 * voice.pitch_walk.value);
    const float level =
        static_cast<float>(params_[base + Level].load(std::memory_order_relaxed));
    level_target[v] = std::max(0.0f, level * (1.0f + drift * 0.4f * voice.level_walk.value));
    shape_smooth_[v] += slow * (static_cast<float>(params_[base + Shape].load(
                                    std::memory_order_relaxed)) -
                                shape_smooth_[v]);
    // Width: left and right a few cents apart, in opposite directions on
    // alternate strings, and the strings leaned a little off centre. The
    // root stays in the middle so the drone has somewhere to hang from.
    const double sign = (v % 2 == 0) ? 1.0 : -1.0;
    spread[v] = std::exp2(sign * width_ * 5.0 / 1200.0);
    const float pan = v == 0 ? 0.0f : static_cast<float>(sign) * width_ * 0.45f;
    pan_l[v] = std::sqrt(0.5f - pan * 0.5f);
    pan_r[v] = std::sqrt(0.5f + pan * 0.5f);
  }

  // --- per-sample coefficients -----------------------------------------------------
  const float glide_coeff = coeff_for(glide, sample_seconds);
  const float gain_coeff = coeff_for(0.04, sample_seconds);
  const float env_step = static_cast<float>(sample_seconds / rise);
  const float drive = 1.0f + grit_ * 8.0f;
  const float drive_norm = 1.0f / std::sqrt(drive);
  const float wet = space_ * 0.9f;
  const float dry = 1.0f - 0.5f * wet;

  float peak = 0.0f;
  for (uint32_t i = 0; i < frames; ++i) {
    root_ += glide_coeff * (root_target - root_);
    const double root_hz = midi_to_hz(root_);

    // The swell ramps straight, then the gain is its square: a line sounds
    // like it arrives all at once, a square rises out of nothing.
    if (env_ < swell_target)
      env_ = std::min(swell_target, env_ + env_step);
    else if (env_ > swell_target)
      env_ = std::max(swell_target, env_ - env_step);
    const float swell_gain = env_ * env_;

    float mix[2] = {0.0f, 0.0f};
    for (int v = 0; v < kVoices; ++v) {
      Voice& voice = voices_[static_cast<size_t>(v)];
      voice.pitch += glide_coeff * (pitch_target[v] - voice.pitch);
      voice.gain += gain_coeff * (level_target[v] - voice.gain);
      if (voice.gain < 1e-5f && level_target[v] < 1e-5f) continue;
      const double hz = root_hz * std::exp2(voice.pitch / 12.0);
      const double inc_l = hz * spread[v] / sr;
      const double inc_r = hz / spread[v] / sr;
      if (inc_l >= 0.5 || inc_r >= 0.5) continue;  // past Nyquist, say nothing
      const float shape = shape_smooth_[v];
      const float l = osc(voice.phase[0], inc_l, shape);
      voice.phase[0] += inc_l;
      voice.phase[0] -= std::floor(voice.phase[0]);
      float r = l;
      if (width > 1) {
        r = osc(voice.phase[1], inc_r, shape);
        voice.phase[1] += inc_r;
        voice.phase[1] -= std::floor(voice.phase[1]);
      }
      mix[0] += l * voice.gain * pan_l[v];
      mix[1] += r * voice.gain * pan_r[v];
    }

    for (int ch = 0; ch < width; ++ch) {
      // Grit, then the filter, then the swell, then the room. The room comes
      // after the swell on purpose: dropping the swell leaves the tail
      // hanging in the air, which is the sound of a drone stopping.
      float s = std::tanh(mix[ch] * 0.4f * drive) * drive_norm;

      const float v3 = s - svf_lp_[ch];
      const float v1 = a1 * svf_bp_[ch] + a2 * v3;
      const float v2 = svf_lp_[ch] + a2 * svf_bp_[ch] + a3 * v3;
      svf_bp_[ch] = 2.0f * v1 - svf_bp_[ch];
      svf_lp_[ch] = 2.0f * v2 - svf_lp_[ch];
      s = v2 * swell_gain;

      const float room = reverb(ch, s);
      const float out = s * dry + room * wet;
      outputs[ch][i] = out;
      peak = std::max(peak, std::fabs(out));
    }
  }

  // --- readouts -----------------------------------------------------------------
  const float swell_gain = env_ * env_;
  for (int v = 0; v < kVoices; ++v)
    gain_out_[static_cast<size_t>(v)].store(
        voices_[static_cast<size_t>(v)].gain * swell_gain,
        std::memory_order_relaxed);
  env_out_.store(env_, std::memory_order_relaxed);
  peak_out_.store(peak, std::memory_order_relaxed);
  root_out_.store(root_, std::memory_order_relaxed);
}

// --- parameters ------------------------------------------------------------------

std::vector<ParameterInfo> DroneInstance::parameters() const {
  std::vector<ParameterInfo> info;
  info.reserve(kParamCount);
  for (uint32_t id = 0; id < kParamCount; ++id)
    info.push_back({id, param_name(id), param_min(id), param_max(id),
                    param_default(id)});
  return info;
}

double DroneInstance::parameter_value(uint32_t id) const {
  if (id >= kParamCount) return 0.0;
  return params_[id].load(std::memory_order_relaxed);
}

void DroneInstance::set_parameter(uint32_t id, double value) {
  if (id >= kParamCount) return;
  if (!std::isfinite(value)) return;
  value = std::clamp(value, param_min(id), param_max(id));
  const bool integer = (id < Swell && id % kVoiceStride == Interval) ||
                       id == Root || id == Just;
  if (integer) value = std::round(value);
  params_[id].store(value, std::memory_order_relaxed);
}

float DroneInstance::voice_gain(int voice) const {
  if (voice < 0 || voice >= kVoices) return 0.0f;
  return gain_out_[static_cast<size_t>(voice)].load(std::memory_order_relaxed);
}

// --- state -------------------------------------------------------------------------

std::vector<uint8_t> DroneInstance::save_state() const {
  // One "id value" per line under a header that names the format. Ids rather
  // than positions so a parameter added later lands at the end and an old
  // blob still reads; the header so a blob from some other plugin that
  // happens to be numbers is refused rather than loaded as a drone.
  std::string text = "drone 1\n";
  for (uint32_t id = 0; id < kParamCount; ++id) {
    text += std::to_string(id);
    text += ' ';
    text += format_number(parameter_value(id), 6);
    text += '\n';
  }
  return {text.begin(), text.end()};
}

bool DroneInstance::load_state(const std::vector<uint8_t>& blob) {
  const std::string_view text(reinterpret_cast<const char*>(blob.data()),
                              blob.size());
  if (text.substr(0, 5) != "drone") return false;

  size_t start = text.find('\n');
  if (start == std::string_view::npos) return false;
  ++start;
  int read = 0;
  while (start < text.size()) {
    size_t split = text.find('\n', start);
    if (split == std::string_view::npos) split = text.size();
    const std::string_view line = text.substr(start, split - start);
    start = split + 1;
    const size_t space = line.find(' ');
    if (space == std::string_view::npos) continue;
    double id = 0.0, value = 0.0;
    if (!parse_number(line.substr(0, space), &id)) continue;
    if (!parse_number(line.substr(space + 1), &value)) continue;
    if (id < 0.0 || id >= static_cast<double>(kParamCount)) continue;
    set_parameter(static_cast<uint32_t>(id), value);
    ++read;
  }
  return read > 0;
}

}  // namespace nirbija
