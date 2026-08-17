#include "core/step_sequencer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace nirbija {
namespace {

// Beats per step, in quarter notes. The index is the `division` parameter.
constexpr double kDivisions[] = {
    1.0,        // 1/4
    0.5,        // 1/8
    0.25,       // 1/16
    0.125,      // 1/32
    1.0 / 3.0,  // 1/4 triplet
    1.0 / 6.0,  // 1/8 triplet
};
constexpr int kDivisionCount = static_cast<int>(std::size(kDivisions));

enum Params : uint32_t {
  kDivision = 0,
  kLength = 1,
  kGate = 2,
  kTranspose = 3,
  kChannel = 4,
  kSwing = 5,
  kDirection = 6,
  kScale = 7,
  kRoot = 8,
  kNudgeLeft = 9,
  kNudgeRight = 10,
  kRandomHits = 11,
  kRandomNotes = 12,
  kClearHits = 13,
  kEuclid = 14,

  // One block of ids per column of the grid, so a step's values are
  // kStepNote + i, kStepVelocity + i, and so on.
  kStepNote = 16,
  kStepVelocity = 48,
  kStepActive = 80,
  kStepProb = 112,
  kStepAccent = 144,
  kStepTie = 176,
};

constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kNoteOff = 0x80;

int clamp_int(double value, int low, int high) {
  const int rounded = static_cast<int>(std::lround(value));
  return std::clamp(rounded, low, high);
}

bool in_scale(int pc, const int* degrees) {
  for (int i = 0; degrees[i] >= 0; ++i)
    if (degrees[i] == pc) return true;
  return false;
}

const int* scale_degrees(int scale) {
  static constexpr int kMajor[] = {0, 2, 4, 5, 7, 9, 11, -1};
  static constexpr int kMinor[] = {0, 2, 3, 5, 7, 8, 10, -1};
  static constexpr int kDorian[] = {0, 2, 3, 5, 7, 9, 10, -1};
  static constexpr int kMixo[] = {0, 2, 4, 5, 7, 9, 10, -1};
  static constexpr int kPentaMin[] = {0, 3, 5, 7, 10, -1};
  static constexpr int kPentaMaj[] = {0, 2, 4, 7, 9, -1};
  static constexpr int kBlues[] = {0, 3, 5, 6, 7, 10, -1};
  switch (scale) {
    case StepSequencerInstance::Major: return kMajor;
    case StepSequencerInstance::Minor: return kMinor;
    case StepSequencerInstance::Dorian: return kDorian;
    case StepSequencerInstance::Mixolydian: return kMixo;
    case StepSequencerInstance::PentaMinor: return kPentaMin;
    case StepSequencerInstance::PentaMajor: return kPentaMaj;
    case StepSequencerInstance::Blues: return kBlues;
    default: return nullptr;
  }
}

}  // namespace

int StepSequencerInstance::snap_to_scale(int note, int scale, int root) {
  note = std::clamp(note, 0, 127);
  const int* degrees = scale_degrees(scale);
  if (degrees == nullptr) return note;
  root = ((root % 12) + 12) % 12;

  int best = note;
  int best_d = 128;
  for (int n = note - 6; n <= note + 6; ++n) {
    if (n < 0 || n > 127) continue;
    const int pc = ((n - root) % 12 + 12) % 12;
    if (!in_scale(pc, degrees)) continue;
    const int d = std::abs(n - note);
    if (d < best_d) {
      best_d = d;
      best = n;
    }
  }
  return best;
}

PluginDescriptor StepSequencerInstance::make_descriptor() {
  PluginDescriptor descriptor;
  descriptor.format = PluginFormat::Internal;
  descriptor.uid = "nirbija.stepseq";
  descriptor.name = "Step Sequencer";
  descriptor.vendor = "Nirbija";
  // No audio at all: this makes notes and nothing else, which is also what
  // puts it in the midi bucket of the picker without being told to.
  descriptor.audio_inputs = 0;
  descriptor.audio_outputs = 0;
  descriptor.has_midi_input = true;
  descriptor.category = "Sequencer";
  descriptor.kind = PluginKind::MidiEffect;
  return descriptor;
}

StepSequencerInstance::StepSequencerInstance()
    : descriptor_(make_descriptor()) {
  // A minor pentatonic walk, so a fresh sequencer plays something rather than
  // sixteen copies of the same note.
  static constexpr int kSeed[kSteps] = {57, 60, 62, 64, 67, 64, 62, 60,
                                        57, 60, 62, 67, 69, 67, 64, 60};
  for (int i = 0; i < kSteps; ++i) {
    note_[i].store(kSeed[i], std::memory_order_relaxed);
    velocity_[i].store(100, std::memory_order_relaxed);
    // Every other step, so the default pattern has a pulse instead of being a
    // solid run of sixteenths.
    active_[i].store(i % 2 == 0, std::memory_order_relaxed);
    probability_[i].store(1.0f, std::memory_order_relaxed);
    accent_[i].store(false, std::memory_order_relaxed);
    tie_[i].store(false, std::memory_order_relaxed);
  }
}

bool StepSequencerInstance::activate(double sample_rate, uint32_t) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  sounding_note_ = -1;
  last_tied_ = false;
  last_step_beat_ = -1.0;
  event_count_ = 0;
  return true;
}

void StepSequencerInstance::deactivate() {
  sounding_note_ = -1;
  last_tied_ = false;
  last_step_beat_ = -1.0;
  event_count_ = 0;
}

double StepSequencerInstance::beats_per_step() const {
  const int index =
      std::clamp(division_.load(std::memory_order_relaxed), 0, kDivisionCount - 1);
  return kDivisions[index];
}

double StepSequencerInstance::beat_of(double index) const {
  const double step = beats_per_step();
  double beat = index * step;
  const float swing =
      std::clamp(swing_.load(std::memory_order_relaxed), 0.0f, 1.0f);
  // Odd steps lean late, up to half a step. Even ones stay on the grid, so
  // the pulse is still the pulse.
  if (swing > 0.0f && static_cast<int>(std::floor(index)) % 2 != 0)
    beat += static_cast<double>(swing) * step * 0.5;
  return beat;
}

int StepSequencerInstance::map_step(int index, int length) {
  if (length <= 1) return 0;
  const int dir = std::clamp(direction_.load(std::memory_order_relaxed), 0,
                             DirectionCount - 1);
  const int wrapped = ((index % length) + length) % length;
  switch (dir) {
    case Reverse:
      return length - 1 - wrapped;
    case Pendulum: {
      const int span = 2 * (length - 1);
      const int p = ((index % span) + span) % span;
      return p < length ? p : span - p;
    }
    case Random:
      return static_cast<int>(next_rng() % static_cast<uint32_t>(length));
    default:
      return wrapped;
  }
}

uint32_t StepSequencerInstance::next_rng() {
  rng_ = rng_ * 1664525u + 1013904223u;
  return rng_;
}

uint32_t StepSequencerInstance::next_ui_rng() {
  ui_rng_ = ui_rng_ * 1664525u + 1013904223u;
  return ui_rng_;
}

void StepSequencerInstance::emit(uint32_t frame, uint8_t status, uint8_t data1,
                                 uint8_t data2) {
  if (event_count_ >= kMaxEvents) return;
  MidiEvent& event = events_[event_count_++];
  event.frame = frame;
  event.size = 3;
  event.data[0] = status;
  event.data[1] = data1;
  event.data[2] = data2;
}

void StepSequencerInstance::stop_sounding(uint32_t frame) {
  if (sounding_note_ < 0) return;
  const uint8_t channel =
      static_cast<uint8_t>(std::clamp(channel_.load(std::memory_order_relaxed), 0, 15));
  emit(frame, static_cast<uint8_t>(kNoteOff | channel),
       static_cast<uint8_t>(sounding_note_), 0);
  sounding_note_ = -1;
  last_tied_ = false;
}

void StepSequencerInstance::queue_midi(const MidiEvent& event) {
  // Passed along untouched: the sequencer adds to the chain, it does not own it.
  if (event_count_ >= kMaxEvents) return;
  events_[event_count_++] = event;
}

void StepSequencerInstance::process(const float* const*, float* const*,
                                    uint32_t frames) {
  if (frames == 0) return;

  // Play starts the figure. The metronome may be walking the same grid so a
  // looper can punch to the click, but that is not a reason to fire notes —
  // turning the click on used to start the sequencer as if Play had been hit.
  const bool run = transport_.playing;
  if (!run || transport_.changed) {
    stop_sounding(0);
    last_step_beat_ = -1.0;
    if (!run) {
      playhead_.store(-1, std::memory_order_relaxed);
      return;
    }
  }

  const double tempo = transport_.tempo_bpm > 0.0 ? transport_.tempo_bpm : 120.0;
  const double block_beats =
      static_cast<double>(frames) / sample_rate_ * tempo / 60.0;
  if (block_beats <= 0.0) return;

  const double start_beat = transport_.beats;
  const double end_beat = start_beat + block_beats;
  const double step_beats = beats_per_step();
  const int length = std::clamp(length_.load(std::memory_order_relaxed), 1, kSteps);
  const uint8_t channel =
      static_cast<uint8_t>(std::clamp(channel_.load(std::memory_order_relaxed), 0, 15));
  const int scale = scale_.load(std::memory_order_relaxed);
  const int root = root_.load(std::memory_order_relaxed);
  const int transpose = transpose_.load(std::memory_order_relaxed);
  const double gate =
      std::clamp(gate_.load(std::memory_order_relaxed), 0.05, 1.0);

  const auto frame_for = [&](double beat) {
    const double offset = (beat - start_beat) / block_beats * frames;
    return static_cast<uint32_t>(
        std::clamp(offset, 0.0, static_cast<double>(frames - 1)));
  };

  // Gate-off is applied after the step walk: a tie has to see the next
  // step before anyone releases, or a held pitch retriggers every column.

  // Walk one extra index behind so a swung odd step that landed late in this
  // block is not skipped because its unswung time was in the previous one.
  double index = std::floor(start_beat / step_beats) - 1.0;
  if (index < 0.0) index = 0.0;
  int guard = 0;
  for (; guard < 64; ++guard, index += 1.0) {
    const double beat = beat_of(index);
    if (beat >= end_beat) break;
    if (beat < start_beat) continue;
    if (beat <= last_step_beat_) continue;
    last_step_beat_ = beat;

    const int step = map_step(static_cast<int>(std::floor(index)), length);
    const uint32_t frame = frame_for(beat);
    playhead_.store(step, std::memory_order_relaxed);

    const bool on = active_[step].load(std::memory_order_relaxed);
    const float p = std::clamp(probability_[step].load(std::memory_order_relaxed),
                               0.0f, 1.0f);
    const bool hits = on && (p >= 0.999f || (next_rng() / 4294967295.0f) < p);

    if (!hits) {
      stop_sounding(frame);
      continue;
    }

    const int raw = note_[step].load(std::memory_order_relaxed) + transpose;
    const int pitch = std::clamp(snap_to_scale(raw, scale, root), 0, 127);
    int velocity =
        std::clamp(velocity_[step].load(std::memory_order_relaxed), 1, 127);
    if (accent_[step].load(std::memory_order_relaxed))
      velocity = std::min(127, velocity + 27);
    const bool tie = tie_[step].load(std::memory_order_relaxed);
    const bool legato = last_tied_ && sounding_note_ == pitch;

    if (!legato) {
      stop_sounding(frame);
      emit(frame, static_cast<uint8_t>(kNoteOn | channel),
           static_cast<uint8_t>(pitch), static_cast<uint8_t>(velocity));
      sounding_note_ = pitch;
    }

    const double next = beat_of(index + 1.0);
    const double dur = std::max(1e-6, next - beat);
    sounding_off_ = beat + dur * (tie ? 1.0 : gate);
    last_tied_ = tie;
  }

  if (sounding_note_ >= 0 && sounding_off_ < end_beat)
    stop_sounding(frame_for(std::max(sounding_off_, start_beat)));
}

size_t StepSequencerInstance::take_midi_output(MidiEvent* out, size_t capacity) {
  std::sort(events_.begin(), events_.begin() + event_count_,
            [](const MidiEvent& a, const MidiEvent& b) {
              return a.frame < b.frame;
            });
  const size_t count = std::min(event_count_, capacity);
  std::copy_n(events_.begin(), count, out);
  event_count_ = 0;
  return count;
}

void StepSequencerInstance::rotate_steps(int delta) {
  const int n = std::clamp(length_.load(std::memory_order_relaxed), 1, kSteps);
  if (n <= 1 || delta == 0) return;
  const int shift = ((delta % n) + n) % n;
  int notes[kSteps], vels[kSteps];
  bool ons[kSteps], acc[kSteps], ties[kSteps];
  float probs[kSteps];
  for (int i = 0; i < n; ++i) {
    notes[i] = note_[i].load(std::memory_order_relaxed);
    vels[i] = velocity_[i].load(std::memory_order_relaxed);
    ons[i] = active_[i].load(std::memory_order_relaxed);
    probs[i] = probability_[i].load(std::memory_order_relaxed);
    acc[i] = accent_[i].load(std::memory_order_relaxed);
    ties[i] = tie_[i].load(std::memory_order_relaxed);
  }
  for (int i = 0; i < n; ++i) {
    const int src = (i - shift + n) % n;
    note_[i].store(notes[src], std::memory_order_relaxed);
    velocity_[i].store(vels[src], std::memory_order_relaxed);
    active_[i].store(ons[src], std::memory_order_relaxed);
    probability_[i].store(probs[src], std::memory_order_relaxed);
    accent_[i].store(acc[src], std::memory_order_relaxed);
    tie_[i].store(ties[src], std::memory_order_relaxed);
  }
}

void StepSequencerInstance::randomize_hits() {
  const int n = std::clamp(length_.load(std::memory_order_relaxed), 1, kSteps);
  for (int i = 0; i < n; ++i)
    active_[i].store((next_ui_rng() & 1u) != 0, std::memory_order_relaxed);
}

void StepSequencerInstance::randomize_notes() {
  const int n = std::clamp(length_.load(std::memory_order_relaxed), 1, kSteps);
  const int scale = scale_.load(std::memory_order_relaxed);
  const int root = root_.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i) {
    const int raw = 48 + static_cast<int>(next_ui_rng() % 25u);
    note_[i].store(snap_to_scale(raw, scale, root), std::memory_order_relaxed);
  }
}

void StepSequencerInstance::clear_hits() {
  for (int i = 0; i < kSteps; ++i)
    active_[i].store(false, std::memory_order_relaxed);
}

void StepSequencerInstance::fill_euclidean(int pulses) {
  const int n = std::clamp(length_.load(std::memory_order_relaxed), 1, kSteps);
  pulses = std::clamp(pulses, 0, n);
  euclid_.store(pulses, std::memory_order_relaxed);
  for (int i = 0; i < kSteps; ++i) {
    const bool on = i < n && pulses > 0 && ((i * pulses) % n) < pulses;
    active_[i].store(on, std::memory_order_relaxed);
  }
}

std::vector<ParameterInfo> StepSequencerInstance::parameters() const {
  std::vector<ParameterInfo> info{
      {kDivision, "Division (0 1/4, 1 1/8, 2 1/16, 3 1/32, 4 1/4T, 5 1/8T)", 0.0,
       kDivisionCount - 1.0, 2.0},
      {kLength, "Steps", 1.0, kSteps, kSteps},
      {kGate, "Gate", 0.05, 1.0, 0.5},
      {kTranspose, "Transpose", -24.0, 24.0, 0.0},
      {kChannel, "MIDI channel", 1.0, 16.0, 1.0},
      {kSwing, "Swing", 0.0, 1.0, 0.0},
      {kDirection, "Direction (0 fwd, 1 rev, 2 pendulum, 3 random)", 0.0,
       static_cast<double>(DirectionCount) - 1.0, 0.0},
      {kScale, "Scale", 0.0, static_cast<double>(ScaleCount) - 1.0, 0.0},
      {kRoot, "Root (0 C .. 11 B)", 0.0, 11.0, 0.0},
      {kNudgeLeft, "Nudge left", 0.0, 1.0, 0.0},
      {kNudgeRight, "Nudge right", 0.0, 1.0, 0.0},
      {kRandomHits, "Randomize hits", 0.0, 1.0, 0.0},
      {kRandomNotes, "Randomize notes", 0.0, 1.0, 0.0},
      {kClearHits, "Clear hits", 0.0, 1.0, 0.0},
      {kEuclid, "Euclid hits", 0.0, kSteps, 0.0},
  };
  info.reserve(info.size() + kSteps * 6);
  for (int i = 0; i < kSteps; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "Step %d note", i + 1);
    info.push_back({kStepNote + i, name, 0.0, 127.0, 60.0});
  }
  for (int i = 0; i < kSteps; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "Step %d level", i + 1);
    info.push_back({kStepVelocity + i, name, 1.0, 127.0, 100.0});
  }
  for (int i = 0; i < kSteps; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "Step %d on", i + 1);
    info.push_back({kStepActive + i, name, 0.0, 1.0, 0.0});
  }
  for (int i = 0; i < kSteps; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "Step %d chance", i + 1);
    info.push_back({kStepProb + i, name, 0.0, 1.0, 1.0});
  }
  for (int i = 0; i < kSteps; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "Step %d accent", i + 1);
    info.push_back({kStepAccent + i, name, 0.0, 1.0, 0.0});
  }
  for (int i = 0; i < kSteps; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "Step %d tie", i + 1);
    info.push_back({kStepTie + i, name, 0.0, 1.0, 0.0});
  }
  return info;
}

double StepSequencerInstance::parameter_value(uint32_t id) const {
  switch (id) {
    case kDivision: return division_.load(std::memory_order_relaxed);
    case kLength: return length_.load(std::memory_order_relaxed);
    case kGate: return gate_.load(std::memory_order_relaxed);
    case kTranspose: return transpose_.load(std::memory_order_relaxed);
    case kChannel: return channel_.load(std::memory_order_relaxed) + 1;
    case kSwing: return swing_.load(std::memory_order_relaxed);
    case kDirection: return direction_.load(std::memory_order_relaxed);
    case kScale: return scale_.load(std::memory_order_relaxed);
    case kRoot: return root_.load(std::memory_order_relaxed);
    case kNudgeLeft:
    case kNudgeRight:
    case kRandomHits:
    case kRandomNotes:
    case kClearHits:
      return 0.0;
    case kEuclid: return euclid_.load(std::memory_order_relaxed);
    default: break;
  }
  if (id >= kStepNote && id < kStepNote + kSteps)
    return note_[id - kStepNote].load(std::memory_order_relaxed);
  if (id >= kStepVelocity && id < kStepVelocity + kSteps)
    return velocity_[id - kStepVelocity].load(std::memory_order_relaxed);
  if (id >= kStepActive && id < kStepActive + kSteps)
    return active_[id - kStepActive].load(std::memory_order_relaxed) ? 1.0 : 0.0;
  if (id >= kStepProb && id < kStepProb + kSteps)
    return probability_[id - kStepProb].load(std::memory_order_relaxed);
  if (id >= kStepAccent && id < kStepAccent + kSteps)
    return accent_[id - kStepAccent].load(std::memory_order_relaxed) ? 1.0 : 0.0;
  if (id >= kStepTie && id < kStepTie + kSteps)
    return tie_[id - kStepTie].load(std::memory_order_relaxed) ? 1.0 : 0.0;
  return 0.0;
}

void StepSequencerInstance::set_parameter(uint32_t id, double value) {
  switch (id) {
    case kDivision:
      division_.store(clamp_int(value, 0, kDivisionCount - 1),
                      std::memory_order_relaxed);
      return;
    case kLength:
      length_.store(clamp_int(value, 1, kSteps), std::memory_order_relaxed);
      return;
    case kGate:
      gate_.store(std::clamp(value, 0.05, 1.0), std::memory_order_relaxed);
      return;
    case kTranspose:
      transpose_.store(clamp_int(value, -24, 24), std::memory_order_relaxed);
      return;
    case kChannel:
      channel_.store(clamp_int(value, 1, 16) - 1, std::memory_order_relaxed);
      return;
    case kSwing:
      swing_.store(static_cast<float>(std::clamp(value, 0.0, 1.0)),
                   std::memory_order_relaxed);
      return;
    case kDirection:
      direction_.store(clamp_int(value, 0, DirectionCount - 1),
                       std::memory_order_relaxed);
      return;
    case kScale:
      scale_.store(clamp_int(value, 0, ScaleCount - 1),
                   std::memory_order_relaxed);
      return;
    case kRoot:
      root_.store(clamp_int(value, 0, 11), std::memory_order_relaxed);
      return;
    case kNudgeLeft:
      if (value >= 0.5) rotate_steps(-1);
      return;
    case kNudgeRight:
      if (value >= 0.5) rotate_steps(1);
      return;
    case kRandomHits:
      if (value >= 0.5) randomize_hits();
      return;
    case kRandomNotes:
      if (value >= 0.5) randomize_notes();
      return;
    case kClearHits:
      if (value >= 0.5) clear_hits();
      return;
    case kEuclid:
      fill_euclidean(clamp_int(value, 0, kSteps));
      return;
    default:
      break;
  }
  if (id >= kStepNote && id < kStepNote + kSteps) {
    note_[id - kStepNote].store(clamp_int(value, 0, 127), std::memory_order_relaxed);
  } else if (id >= kStepVelocity && id < kStepVelocity + kSteps) {
    velocity_[id - kStepVelocity].store(clamp_int(value, 1, 127),
                                        std::memory_order_relaxed);
  } else if (id >= kStepActive && id < kStepActive + kSteps) {
    active_[id - kStepActive].store(value >= 0.5, std::memory_order_relaxed);
  } else if (id >= kStepProb && id < kStepProb + kSteps) {
    probability_[id - kStepProb].store(
        static_cast<float>(std::clamp(value, 0.0, 1.0)),
        std::memory_order_relaxed);
  } else if (id >= kStepAccent && id < kStepAccent + kSteps) {
    accent_[id - kStepAccent].store(value >= 0.5, std::memory_order_relaxed);
  } else if (id >= kStepTie && id < kStepTie + kSteps) {
    tie_[id - kStepTie].store(value >= 0.5, std::memory_order_relaxed);
  }
}

std::vector<uint8_t> StepSequencerInstance::save_state() const {
  // One line per field, the same shape the other built-in plugins use, so a
  // session stays something a person can read and repair. Extra tokens on a
  // step line are newer fields: an old reader stops after on/off.
  std::string text;
  char line[96];
  std::snprintf(line, sizeof(line), "division %d\nlength %d\ngate %.4f\n",
                division_.load(std::memory_order_relaxed),
                length_.load(std::memory_order_relaxed),
                gate_.load(std::memory_order_relaxed));
  text += line;
  std::snprintf(line, sizeof(line), "transpose %d\nchannel %d\n",
                transpose_.load(std::memory_order_relaxed),
                channel_.load(std::memory_order_relaxed));
  text += line;
  std::snprintf(line, sizeof(line), "swing %.4f\ndirection %d\nscale %d\nroot %d\n",
                swing_.load(std::memory_order_relaxed),
                direction_.load(std::memory_order_relaxed),
                scale_.load(std::memory_order_relaxed),
                root_.load(std::memory_order_relaxed));
  text += line;
  for (int i = 0; i < kSteps; ++i) {
    std::snprintf(line, sizeof(line), "step %d %d %d %.4f %d %d\n",
                  note_[i].load(std::memory_order_relaxed),
                  velocity_[i].load(std::memory_order_relaxed),
                  active_[i].load(std::memory_order_relaxed) ? 1 : 0,
                  probability_[i].load(std::memory_order_relaxed),
                  accent_[i].load(std::memory_order_relaxed) ? 1 : 0,
                  tie_[i].load(std::memory_order_relaxed) ? 1 : 0);
    text += line;
  }
  return {text.begin(), text.end()};
}

bool StepSequencerInstance::load_state(const std::vector<uint8_t>& blob) {
  const std::string text(blob.begin(), blob.end());
  size_t position = 0;
  int step = 0;

  while (position < text.size()) {
    const size_t end = text.find('\n', position);
    const std::string_view line(
        text.data() + position,
        (end == std::string::npos ? text.size() : end) - position);
    position = (end == std::string::npos) ? text.size() : end + 1;
    if (line.empty()) continue;

    const size_t space = line.find(' ');
    if (space == std::string_view::npos) continue;
    const std::string_view key = line.substr(0, space);
    std::string_view rest = line.substr(space + 1);

    const auto number = [&rest](double* out) {
      while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
      const size_t next = rest.find(' ');
      const std::string_view token = rest.substr(0, next);
      rest = next == std::string_view::npos ? std::string_view{}
                                            : rest.substr(next + 1);
      return parse_number(token, out);
    };

    double value = 0.0;
    if (key == "division" && number(&value)) {
      set_parameter(kDivision, value);
    } else if (key == "length" && number(&value)) {
      set_parameter(kLength, value);
    } else if (key == "gate" && number(&value)) {
      set_parameter(kGate, value);
    } else if (key == "transpose" && number(&value)) {
      set_parameter(kTranspose, value);
    } else if (key == "channel" && number(&value)) {
      set_parameter(kChannel, value + 1);
    } else if (key == "swing" && number(&value)) {
      set_parameter(kSwing, value);
    } else if (key == "direction" && number(&value)) {
      set_parameter(kDirection, value);
    } else if (key == "scale" && number(&value)) {
      set_parameter(kScale, value);
    } else if (key == "root" && number(&value)) {
      set_parameter(kRoot, value);
    } else if (key == "step" && step < kSteps) {
      double velocity = 0.0;
      double on = 0.0;
      if (number(&value) && number(&velocity) && number(&on)) {
        set_parameter(kStepNote + step, value);
        set_parameter(kStepVelocity + step, velocity);
        set_parameter(kStepActive + step, on);
        double prob = 1.0, acc = 0.0, tie = 0.0;
        if (number(&prob)) set_parameter(kStepProb + step, prob);
        if (number(&acc)) set_parameter(kStepAccent + step, acc);
        if (number(&tie)) set_parameter(kStepTie + step, tie);
      }
      ++step;
    }
  }
  return true;
}

}  // namespace nirbija
