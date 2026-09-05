#include "core/step_sequencer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

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
  kRecordArm = 15,

  // One block of ids per column of the grid, so a step's values are
  // kStepNote + i, kStepVelocity + i, and so on. Always pattern 0 / lane 0
  // / steps 0–15, so a MIDI learn of step 1 does not follow focus.
  kStepNote = 16,
  kStepVelocity = 48,
  kStepActive = 80,
  kStepProb = 112,
  kStepAccent = 144,
  kStepTie = 176,

  // Performance macros and transport-facing scalars. Not per-step, so they
  // stay listed in parameters() even though the shim block above does not.
  kDensity = 192,
  kChaos = 193,
  kRatchetAmount = 194,
  kMasterProb = 195,
  kPattern = 196,
  kNextPattern = 197,  // 0 = none, 1–16 = pattern 0–15
  kFill = 198,
  kFocusedLane = 199,
  kView = 200,
  kMutate = 201,
};

constexpr double kRates[] = {1.0, 2.0, 0.5, 1.0 / 3.0, 1.5};
constexpr int kRateCount = static_cast<int>(std::size(kRates));

constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kNoteOff = 0x80;

constexpr size_t kMaxBlobBytes = 1024 * 1024;
constexpr int kMaxPstepLines =
    StepSequencerInstance::kPatterns * StepSequencerInstance::kLanes *
    StepSequencerInstance::kMaxSteps;
constexpr int kMaxStateLines = kMaxPstepLines + 512;

constexpr int kGmNote[StepSequencerInstance::kLanes] = {57, 38, 42, 46,
                                                        49, 51, 39, 37};

int clamp_int(double value, int low, int high) {
  const int rounded = static_cast<int>(std::lround(value));
  return std::clamp(rounded, low, high);
}

int clamp_note(int note) {
  if (note == StepSequencerInstance::kUnlockedNote) return note;
  return std::clamp(note, 0, 127);
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

bool StepSequencerInstance::cell_in_range(int pattern, int lane,
                                          int step) const {
  return pattern >= 0 && pattern < kPatterns && lane >= 0 && lane < kLanes &&
         step >= 0 && step < kMaxSteps;
}

StepSequencerInstance::StepCell& StepSequencerInstance::cell_at(int pattern,
                                                                int lane,
                                                                int step) {
  return cells_[static_cast<size_t>(pattern)][static_cast<size_t>(lane)]
               [static_cast<size_t>(step)];
}

const StepSequencerInstance::StepCell& StepSequencerInstance::cell_at(
    int pattern, int lane, int step) const {
  return cells_[static_cast<size_t>(pattern)][static_cast<size_t>(lane)]
               [static_cast<size_t>(step)];
}

void StepSequencerInstance::reset_blank() {
  for (int p = 0; p < kPatterns; ++p) {
    for (int l = 0; l < kLanes; ++l) {
      for (int s = 0; s < kMaxSteps; ++s) {
        StepCell& cell = cell_at(p, l, s);
        cell.note.store(kUnlockedNote, std::memory_order_relaxed);
        cell.velocity.store(100, std::memory_order_relaxed);
        cell.active.store(false, std::memory_order_relaxed);
        cell.probability.store(1.0f, std::memory_order_relaxed);
        cell.accent.store(false, std::memory_order_relaxed);
        cell.tie.store(false, std::memory_order_relaxed);
        cell.microtiming.store(0.0f, std::memory_order_relaxed);
        cell.ratchet.store(1, std::memory_order_relaxed);
        cell.condition.store(Always, std::memory_order_relaxed);
        cell.cond_arg.store(0, std::memory_order_relaxed);
      }
    }
  }

  for (int l = 0; l < kLanes; ++l) {
    LaneState& lane = lanes_[static_cast<size_t>(l)];
    lane.note.store(kGmNote[l], std::memory_order_relaxed);
    lane.length.store(kVisibleSteps, std::memory_order_relaxed);
    lane.division.store(2, std::memory_order_relaxed);
    lane.direction.store(Forward, std::memory_order_relaxed);
    lane.channel.store(0, std::memory_order_relaxed);
    lane.mute.store(l != 0, std::memory_order_relaxed);
    lane.gate.store(0.5, std::memory_order_relaxed);
    lane.euclid.store(0, std::memory_order_relaxed);
    head_steps_[static_cast<size_t>(l)].store(-1, std::memory_order_relaxed);
  }

  for (int h = 0; h < kExtraHeads; ++h) {
    ExtraHead& head = extra_heads_[static_cast<size_t>(h)];
    head.lane.store(0, std::memory_order_relaxed);
    head.rate.store(0, std::memory_order_relaxed);
    head.direction.store(Forward, std::memory_order_relaxed);
    head.start.store(0, std::memory_order_relaxed);
    head.length.store(kVisibleSteps, std::memory_order_relaxed);
    head.mute.store(true, std::memory_order_relaxed);
    head.transpose.store(0, std::memory_order_relaxed);
    extra_head_steps_[static_cast<size_t>(h)].store(-1, std::memory_order_relaxed);
  }

  swing_.store(0.0f, std::memory_order_relaxed);
  scale_.store(Chromatic, std::memory_order_relaxed);
  root_.store(0, std::memory_order_relaxed);
  transpose_.store(0, std::memory_order_relaxed);
  pattern_.store(0, std::memory_order_relaxed);
  next_pattern_.store(-1, std::memory_order_relaxed);
  fill_.store(false, std::memory_order_relaxed);
  view_.store(1, std::memory_order_relaxed);
  focus_.store(0, std::memory_order_relaxed);
  macros_[0].store(1.0f, std::memory_order_relaxed);
  macros_[1].store(0.0f, std::memory_order_relaxed);
  macros_[2].store(0.0f, std::memory_order_relaxed);
  macros_[3].store(1.0f, std::memory_order_relaxed);
  record_armed_.store(false, std::memory_order_relaxed);
}

void StepSequencerInstance::paint_constructor_pattern() {
  // A minor pentatonic walk, so a fresh sequencer plays something rather than
  // sixteen copies of the same note.
  static constexpr int kSeed[kVisibleSteps] = {57, 60, 62, 64, 67, 64, 62, 60,
                                               57, 60, 62, 67, 69, 67, 64, 60};
  for (int i = 0; i < kVisibleSteps; ++i) {
    StepCell& cell = cell_at(0, 0, i);
    cell.note.store(kSeed[i], std::memory_order_relaxed);
    cell.velocity.store(100, std::memory_order_relaxed);
    // Every other step, so the default pattern has a pulse instead of being a
    // solid run of sixteenths.
    cell.active.store(i % 2 == 0, std::memory_order_relaxed);
  }

  auto hit = [this](int lane, int step) {
    cell_at(0, lane, step).active.store(true, std::memory_order_relaxed);
  };
  hit(1, 4);
  hit(1, 12);
  for (int i = 0; i < kVisibleSteps; i += 2) hit(2, i);
  hit(3, 14);
  hit(6, 4);
}

StepSequencerInstance::StepSequencerInstance()
    : descriptor_(make_descriptor()) {
  reset_blank();
  paint_constructor_pattern();
  last_step_beat_.fill(-1.0);
}

void StepSequencerInstance::set_cell(int pattern, int lane, int step, int note,
                                     int velocity, bool on, float probability) {
  if (!cell_in_range(pattern, lane, step)) return;
  StepCell& cell = cell_at(pattern, lane, step);
  cell.note.store(clamp_note(note), std::memory_order_relaxed);
  cell.velocity.store(std::clamp(velocity, 1, 127), std::memory_order_relaxed);
  cell.active.store(on, std::memory_order_relaxed);
  cell.probability.store(std::clamp(probability, 0.0f, 1.0f),
                         std::memory_order_relaxed);
}

void StepSequencerInstance::set_cell(int pattern, int lane, int step, int note,
                                     int velocity, bool on, float probability,
                                     bool accent, bool tie) {
  set_cell(pattern, lane, step, note, velocity, on, probability);
  if (!cell_in_range(pattern, lane, step)) return;
  StepCell& cell = cell_at(pattern, lane, step);
  cell.accent.store(accent, std::memory_order_relaxed);
  cell.tie.store(tie, std::memory_order_relaxed);
}

void StepSequencerInstance::set_trig(int pattern, int lane, int step, float micro,
                                     int ratchet, int cond, int cond_arg) {
  if (!cell_in_range(pattern, lane, step)) return;
  StepCell& cell = cell_at(pattern, lane, step);
  cell.microtiming.store(std::clamp(micro, -0.5f, 0.5f),
                         std::memory_order_relaxed);
  cell.ratchet.store(std::clamp(ratchet, 0, 8), std::memory_order_relaxed);
  cell.condition.store(static_cast<uint8_t>(std::clamp(cond, 0, 6)),
                       std::memory_order_relaxed);
  cell.cond_arg.store(static_cast<uint8_t>(std::clamp(cond_arg, 0, 255)),
                      std::memory_order_relaxed);
}

int StepSequencerInstance::cell_note(int pattern, int lane, int step) const {
  if (!cell_in_range(pattern, lane, step)) return 0;
  return cell_at(pattern, lane, step).note.load(std::memory_order_relaxed);
}

int StepSequencerInstance::cell_velocity(int pattern, int lane, int step) const {
  if (!cell_in_range(pattern, lane, step)) return 0;
  return cell_at(pattern, lane, step).velocity.load(std::memory_order_relaxed);
}

bool StepSequencerInstance::cell_active(int pattern, int lane, int step) const {
  if (!cell_in_range(pattern, lane, step)) return false;
  return cell_at(pattern, lane, step).active.load(std::memory_order_relaxed);
}

float StepSequencerInstance::cell_probability(int pattern, int lane,
                                              int step) const {
  if (!cell_in_range(pattern, lane, step)) return 0.0f;
  return cell_at(pattern, lane, step).probability.load(std::memory_order_relaxed);
}

bool StepSequencerInstance::cell_accent(int pattern, int lane, int step) const {
  if (!cell_in_range(pattern, lane, step)) return false;
  return cell_at(pattern, lane, step).accent.load(std::memory_order_relaxed);
}

bool StepSequencerInstance::cell_tie(int pattern, int lane, int step) const {
  if (!cell_in_range(pattern, lane, step)) return false;
  return cell_at(pattern, lane, step).tie.load(std::memory_order_relaxed);
}

float StepSequencerInstance::cell_microtiming(int pattern, int lane,
                                              int step) const {
  if (!cell_in_range(pattern, lane, step)) return 0.0f;
  return cell_at(pattern, lane, step).microtiming.load(std::memory_order_relaxed);
}

int StepSequencerInstance::cell_ratchet(int pattern, int lane, int step) const {
  if (!cell_in_range(pattern, lane, step)) return 0;
  return cell_at(pattern, lane, step).ratchet.load(std::memory_order_relaxed);
}

int StepSequencerInstance::cell_condition(int pattern, int lane, int step) const {
  if (!cell_in_range(pattern, lane, step)) return Always;
  return cell_at(pattern, lane, step).condition.load(std::memory_order_relaxed);
}

int StepSequencerInstance::cell_cond_arg(int pattern, int lane, int step) const {
  if (!cell_in_range(pattern, lane, step)) return 0;
  return cell_at(pattern, lane, step).cond_arg.load(std::memory_order_relaxed);
}

bool StepSequencerInstance::lane_muted(int lane) const {
  if (lane < 0 || lane >= kLanes) return true;
  return lanes_[static_cast<size_t>(lane)].mute.load(std::memory_order_relaxed);
}

void StepSequencerInstance::set_lane_mute(int lane, bool mute) {
  if (lane < 0 || lane >= kLanes) return;
  lanes_[static_cast<size_t>(lane)].mute.store(mute, std::memory_order_relaxed);
}

int StepSequencerInstance::lane_note(int lane) const {
  if (lane < 0 || lane >= kLanes) return 0;
  return lanes_[static_cast<size_t>(lane)].note.load(std::memory_order_relaxed);
}

int StepSequencerInstance::lane_length(int lane) const {
  if (lane < 0 || lane >= kLanes) return 0;
  return lanes_[static_cast<size_t>(lane)].length.load(std::memory_order_relaxed);
}

int StepSequencerInstance::lane_division(int lane) const {
  if (lane < 0 || lane >= kLanes) return 0;
  return lanes_[static_cast<size_t>(lane)].division.load(std::memory_order_relaxed);
}

int StepSequencerInstance::lane_direction(int lane) const {
  if (lane < 0 || lane >= kLanes) return Forward;
  return lanes_[static_cast<size_t>(lane)].direction.load(
      std::memory_order_relaxed);
}

int StepSequencerInstance::lane_channel(int lane) const {
  if (lane < 0 || lane >= kLanes) return 0;
  return lanes_[static_cast<size_t>(lane)].channel.load(std::memory_order_relaxed);
}

double StepSequencerInstance::lane_gate(int lane) const {
  if (lane < 0 || lane >= kLanes) return 0.5;
  return lanes_[static_cast<size_t>(lane)].gate.load(std::memory_order_relaxed);
}

int StepSequencerInstance::lane_euclid(int lane) const {
  if (lane < 0 || lane >= kLanes) return 0;
  return lanes_[static_cast<size_t>(lane)].euclid.load(std::memory_order_relaxed);
}

void StepSequencerInstance::set_lane(int lane, int note, int length, int division,
                                    int direction, int channel, bool mute,
                                    double gate) {
  if (lane < 0 || lane >= kLanes) return;
  LaneState& dest = lanes_[static_cast<size_t>(lane)];
  dest.note.store(std::clamp(note, 0, 127), std::memory_order_relaxed);
  dest.length.store(std::clamp(length, 1, kMaxSteps), std::memory_order_relaxed);
  dest.division.store(std::clamp(division, 0, kDivisionCount - 1),
                      std::memory_order_relaxed);
  dest.direction.store(std::clamp(direction, 0, DirectionCount - 1),
                       std::memory_order_relaxed);
  dest.channel.store(std::clamp(channel, 0, 15), std::memory_order_relaxed);
  dest.mute.store(mute, std::memory_order_relaxed);
  dest.gate.store(std::clamp(gate, 0.05, 1.0), std::memory_order_relaxed);
}

void StepSequencerInstance::set_lane_euclid(int lane, int pulses) {
  if (lane < 0 || lane >= kLanes) return;
  const int n = std::clamp(
      lanes_[static_cast<size_t>(lane)].length.load(std::memory_order_relaxed),
      1, kMaxSteps);
  pulses = std::clamp(pulses, 0, n);
  lanes_[static_cast<size_t>(lane)].euclid.store(pulses,
                                                 std::memory_order_relaxed);
  for (int i = 0; i < kMaxSteps; ++i) {
    const bool on = i < n && pulses > 0 && ((i * pulses) % n) < pulses;
    cell_at(current_pattern(), lane, i)
        .active.store(on, std::memory_order_relaxed);
  }
}

void StepSequencerInstance::set_extra_head(int extra, int lane, int rate,
                                          int direction, int start, int length,
                                          int transpose, bool mute) {
  if (extra < 0 || extra >= kExtraHeads) return;
  ExtraHead& head = extra_heads_[static_cast<size_t>(extra)];
  head.lane.store(std::clamp(lane, 0, kLanes - 1), std::memory_order_relaxed);
  head.rate.store(std::clamp(rate, 0, 4), std::memory_order_relaxed);
  head.direction.store(std::clamp(direction, 0, DirectionCount - 1),
                       std::memory_order_relaxed);
  head.start.store(std::clamp(start, 0, kMaxSteps - 1),
                   std::memory_order_relaxed);
  head.length.store(std::clamp(length, 1, kMaxSteps), std::memory_order_relaxed);
  head.transpose.store(std::clamp(transpose, -24, 24), std::memory_order_relaxed);
  head.mute.store(mute, std::memory_order_relaxed);
}

int StepSequencerInstance::extra_head_lane(int extra) const {
  if (extra < 0 || extra >= kExtraHeads) return 0;
  return extra_heads_[static_cast<size_t>(extra)].lane.load(
      std::memory_order_relaxed);
}

int StepSequencerInstance::extra_head_rate(int extra) const {
  if (extra < 0 || extra >= kExtraHeads) return 0;
  return extra_heads_[static_cast<size_t>(extra)].rate.load(
      std::memory_order_relaxed);
}

int StepSequencerInstance::extra_head_direction(int extra) const {
  if (extra < 0 || extra >= kExtraHeads) return Forward;
  return extra_heads_[static_cast<size_t>(extra)].direction.load(
      std::memory_order_relaxed);
}

int StepSequencerInstance::extra_head_start(int extra) const {
  if (extra < 0 || extra >= kExtraHeads) return 0;
  return extra_heads_[static_cast<size_t>(extra)].start.load(
      std::memory_order_relaxed);
}

int StepSequencerInstance::extra_head_length(int extra) const {
  if (extra < 0 || extra >= kExtraHeads) return 0;
  return extra_heads_[static_cast<size_t>(extra)].length.load(
      std::memory_order_relaxed);
}

int StepSequencerInstance::extra_head_transpose(int extra) const {
  if (extra < 0 || extra >= kExtraHeads) return 0;
  return extra_heads_[static_cast<size_t>(extra)].transpose.load(
      std::memory_order_relaxed);
}

bool StepSequencerInstance::extra_head_muted(int extra) const {
  if (extra < 0 || extra >= kExtraHeads) return true;
  return extra_heads_[static_cast<size_t>(extra)].mute.load(
      std::memory_order_relaxed);
}

int StepSequencerInstance::extra_head_step(int extra) const {
  if (extra < 0 || extra >= kExtraHeads) return -1;
  return extra_head_steps_[static_cast<size_t>(extra)].load(
      std::memory_order_relaxed);
}

int StepSequencerInstance::native_head_step(int lane) const {
  if (lane < 0 || lane >= kLanes) return -1;
  return head_steps_[static_cast<size_t>(lane)].load(std::memory_order_relaxed);
}

int StepSequencerInstance::current_pattern() const {
  return std::clamp(pattern_.load(std::memory_order_relaxed), 0, kPatterns - 1);
}

void StepSequencerInstance::set_focus(int lane) {
  if (lane < 0 || lane >= kLanes) return;
  focus_.store(lane, std::memory_order_relaxed);
}

int StepSequencerInstance::focus() const {
  return focused_index();
}

int StepSequencerInstance::pattern() const {
  return pattern_.load(std::memory_order_relaxed);
}

int StepSequencerInstance::next_pattern() const {
  return next_pattern_.load(std::memory_order_relaxed);
}

bool StepSequencerInstance::fill() const {
  return fill_.load(std::memory_order_relaxed);
}

bool StepSequencerInstance::recording() const {
  return record_armed_.load(std::memory_order_relaxed);
}

int StepSequencerInstance::view() const {
  return view_.load(std::memory_order_relaxed);
}

int StepSequencerInstance::transpose() const {
  return transpose_.load(std::memory_order_relaxed);
}

float StepSequencerInstance::swing() const {
  return swing_.load(std::memory_order_relaxed);
}

int StepSequencerInstance::scale() const {
  return scale_.load(std::memory_order_relaxed);
}

int StepSequencerInstance::root() const {
  return root_.load(std::memory_order_relaxed);
}

float StepSequencerInstance::macro(int index) const {
  if (index < 0 || index >= static_cast<int>(macros_.size())) return 0.0f;
  return macros_[static_cast<size_t>(index)].load(std::memory_order_relaxed);
}

int StepSequencerInstance::focused_index() const {
  return std::clamp(focus_.load(std::memory_order_relaxed), 0, kLanes - 1);
}

bool StepSequencerInstance::activate(double sample_rate, uint32_t) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  voices_ = {};
  last_tied_ = {};
  last_step_beat_.fill(-1.0);
  last_fired_ = {};
  last_on_ = {};
  last_bar_ = -1;
  event_count_ = 0;
  for (int h = 0; h < kLanes; ++h)
    head_steps_[static_cast<size_t>(h)].store(-1, std::memory_order_relaxed);
  for (int h = 0; h < kExtraHeads; ++h)
    extra_head_steps_[static_cast<size_t>(h)].store(-1,
                                                    std::memory_order_relaxed);
  return true;
}

void StepSequencerInstance::deactivate() {
  voices_ = {};
  last_tied_ = {};
  last_step_beat_.fill(-1.0);
  event_count_ = 0;
}

double StepSequencerInstance::beat_of(double index, double step_beats) const {
  double beat = index * step_beats;
  const float swing =
      std::clamp(swing_.load(std::memory_order_relaxed), 0.0f, 1.0f);
  // Odd steps lean late, up to half a step. Even ones stay on the grid, so
  // the pulse is still the pulse.
  if (swing > 0.0f && static_cast<int>(std::floor(index)) % 2 != 0)
    beat += static_cast<double>(swing) * step_beats * 0.5;
  return beat;
}

int StepSequencerInstance::map_step(int index, int length, int direction) {
  if (length <= 1) return 0;
  const int dir = std::clamp(direction, 0, DirectionCount - 1);
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
  MidiEvent event;
  event.frame = frame;
  event.size = 3;
  event.data[0] = status;
  event.data[1] = data1;
  event.data[2] = data2;
  if (!midi_queue_admits(event_count_, kMaxEvents, event)) return;
  events_[event_count_++] = event;
}

void StepSequencerInstance::stop_sounding(int head, uint32_t frame) {
  if (head < 0 || head >= kVoiceSlots) return;
  HeadVoice& voice = voices_[static_cast<size_t>(head)];
  if (voice.pitch < 0) return;
  emit(frame, static_cast<uint8_t>(kNoteOff | voice.channel),
       static_cast<uint8_t>(voice.pitch), 0);
  voice.pitch = -1;
  last_tied_[static_cast<size_t>(head)] = false;
}

void StepSequencerInstance::cancel_scheduler(int head, uint32_t frame) {
  stop_sounding(head, frame);
  if (head < 0 || head >= kVoiceSlots) return;
  voices_[static_cast<size_t>(head)].ratchet_left = 0;
}

void StepSequencerInstance::queue_midi(const MidiEvent& event) {
  // Passed along untouched: the sequencer adds to the chain, it does not own it.
  if (!midi_queue_admits(event_count_, kMaxEvents, event)) return;
  events_[event_count_++] = event;
}

void StepSequencerInstance::process(const float* const*, float* const*,
                                    uint32_t frames) {
  if (frames == 0) return;

  const size_t incoming_count = event_count_;
  const bool armed = record_armed_.load(std::memory_order_relaxed);
  if (armed != record_active_) {
    if (armed) {
      for (int h = 0; h < kVoiceSlots; ++h) cancel_scheduler(h, 0);
      for (Capture& cap : captures_) cap.open = false;
    }
    record_active_ = armed;
  }

  const bool run = transport_.playing;
  if (!run || transport_.changed) {
    for (int h = 0; h < kVoiceSlots; ++h) {
      cancel_scheduler(h, 0);
      last_step_beat_[static_cast<size_t>(h)] = -1.0;
    }
    if (!run) {
      playhead_.store(-1, std::memory_order_relaxed);
      for (int h = 0; h < kLanes; ++h)
        head_steps_[static_cast<size_t>(h)].store(-1, std::memory_order_relaxed);
      for (int h = 0; h < kExtraHeads; ++h)
        extra_head_steps_[static_cast<size_t>(h)].store(
            -1, std::memory_order_relaxed);
      last_bar_ = -1;
      return;
    }
    const int numerator =
        transport_.numerator > 0 ? transport_.numerator : 4;
    last_bar_ = static_cast<int>(std::floor(transport_.beats / numerator));
  }

  const double tempo = transport_.tempo_bpm > 0.0 ? transport_.tempo_bpm : 120.0;
  const double block_beats =
      static_cast<double>(frames) / sample_rate_ * tempo / 60.0;
  if (block_beats <= 0.0) return;

  const double start_beat = transport_.beats;
  const double end_beat = start_beat + block_beats;
  const int scale = scale_.load(std::memory_order_relaxed);
  const int root = root_.load(std::memory_order_relaxed);
  const int transpose = transpose_.load(std::memory_order_relaxed);
  const int focused = focused_index();
  const bool fill = fill_.load(std::memory_order_relaxed);
  const float density = macros_[0].load(std::memory_order_relaxed);
  const float chaos = macros_[1].load(std::memory_order_relaxed);
  const float ratchet_macro = macros_[2].load(std::memory_order_relaxed);
  const float master_prob = macros_[3].load(std::memory_order_relaxed);

  const int numerator = transport_.numerator > 0 ? transport_.numerator : 4;
  const int bar = static_cast<int>(std::floor(start_beat / numerator));
  if (last_bar_ >= 0 && bar > last_bar_) {
    const int next = next_pattern_.load(std::memory_order_relaxed);
    if (next >= 0 && next < kPatterns) {
      pattern_.store(next, std::memory_order_relaxed);
      next_pattern_.store(-1, std::memory_order_relaxed);
    }
  }
  last_bar_ = bar;
  const int play_pattern = current_pattern();

  if (armed) capture_events(incoming_count, start_beat, block_beats, frames);

  const auto frame_for = [&](double beat) {
    const double offset = (beat - start_beat) / block_beats * frames;
    return static_cast<uint32_t>(
        std::clamp(offset, 0.0, static_cast<double>(frames - 1)));
  };

  bool snap_fired[kVoiceSlots];
  bool snap_on[kLanes][kMaxSteps];
  for (int h = 0; h < kVoiceSlots; ++h)
    snap_fired[h] = last_fired_[static_cast<size_t>(h)];
  for (int l = 0; l < kLanes; ++l)
    for (int s = 0; s < kMaxSteps; ++s)
      snap_on[l][s] = last_on_[static_cast<size_t>(l)][static_cast<size_t>(s)];

  const auto drain_ratchet = [&](int voice, bool muted) {
    if (armed || muted) return;
    HeadVoice& v = voices_[static_cast<size_t>(voice)];
    // Not v.pitch: gate closes that between pulses on purpose (see the
    // field comment on ratchet_pitch), and treating "off right now" as
    // "cancelled" used to kill every roll but the last pulse whenever gate
    // was under 1.0 — which is most of the time.
    const int held = v.ratchet_pitch;
    const uint8_t ch = v.ratchet_channel;
    while (v.ratchet_left > 0) {
      if (v.next_pulse_beat < start_beat) {
        --v.ratchet_left;
        v.next_pulse_beat += v.pulse_spacing;
        continue;
      }
      if (v.next_pulse_beat >= end_beat) break;
      const uint32_t frame = frame_for(v.next_pulse_beat);
      stop_sounding(voice, frame);
      emit(frame, static_cast<uint8_t>(kNoteOn | ch),
           static_cast<uint8_t>(held), static_cast<uint8_t>(v.pulse_vel));
      v.pitch = held;
      v.channel = ch;
      v.off_beat = std::min(v.next_pulse_beat + v.pulse_dur, v.step_end_beat);
      --v.ratchet_left;
      v.next_pulse_beat += v.pulse_spacing;
    }
  };

  const auto cond_ok = [&](int voice, int lane, int step, int cond,
                           uint8_t arg, int cycle) {
    switch (cond) {
      case Fill:
        return fill;
      case NotFill:
        return !fill;
      case Pre:
        return snap_fired[voice];
      case NotPre:
        return !snap_fired[voice];
      case Nei: {
        const int neighbor = (lane + kLanes - 1) % kLanes;
        return snap_on[neighbor][step];
      }
      case AOverB: {
        const int a = (arg >> 4) & 0x0f;
        const int b = arg & 0x0f;
        if (b <= 0 || a <= 0) return true;
        return (cycle % b) == (a - 1);
      }
      default:
        return true;
    }
  };

  int focused_playhead = playhead_.load(std::memory_order_relaxed);

  const auto walk = [&](int voice, int lane, bool extra, int extra_idx,
                        double rate, int window_length, int window_start,
                        int direction, int extra_transpose, bool extra_mute) {
    LaneState& lane_state = lanes_[static_cast<size_t>(lane)];
    const int div = std::clamp(
        lane_state.division.load(std::memory_order_relaxed), 0,
        kDivisionCount - 1);
    const double step_beats = kDivisions[div] / rate;
    const int length =
        std::clamp(lane_state.length.load(std::memory_order_relaxed), 1,
                   kMaxSteps);
    const uint8_t channel = static_cast<uint8_t>(
        std::clamp(lane_state.channel.load(std::memory_order_relaxed), 0, 15));
    const double gate =
        std::clamp(lane_state.gate.load(std::memory_order_relaxed), 0.05, 1.0);
    const bool lane_mute =
        lane_state.mute.load(std::memory_order_relaxed);
    const bool muted = extra_mute || lane_mute;
    HeadVoice& voice_state = voices_[static_cast<size_t>(voice)];

    if (extra_mute) {
      cancel_scheduler(voice, 0);
      extra_head_steps_[static_cast<size_t>(extra_idx)].store(
          -1, std::memory_order_relaxed);
      return;
    }

    if (lane_mute) cancel_scheduler(voice, 0);

    if (!armed) drain_ratchet(voice, muted);

    double index = std::floor(start_beat / step_beats) - 1.0;
    if (index < 0.0) index = 0.0;
    int guard = 0;
    for (; guard < 64; ++guard, index += 1.0) {
      const int idx = static_cast<int>(std::floor(index));
      // The cursor is the unswung-odd-swung grid, shared by every lane on
      // the same length/division. Micro and chaos only move the *note*,
      // otherwise Chaos=1 makes each row's playhead wander on its own.
      const double grid_beat = beat_of(index, step_beats);
      if (grid_beat >= end_beat) break;
      if (grid_beat < start_beat) continue;
      if (grid_beat <= last_step_beat_[static_cast<size_t>(voice)]) continue;
      last_step_beat_[static_cast<size_t>(voice)] = grid_beat;

      const int window = map_step(idx, window_length, direction);
      int start = window_start;
      if (start >= length) start %= length;
      const int step = extra ? (start + window) % length : window;
      const StepCell& cell = cell_at(play_pattern, lane, step);
      const float cell_micro = std::clamp(
          cell.microtiming.load(std::memory_order_relaxed), -0.5f, 0.5f);
      double sound_beat = grid_beat + cell_micro * step_beats;
      if (chaos > 0.0f) {
        const float u = next_rng() / 4294967295.0f;
        sound_beat += (u * 2.0f - 1.0f) * chaos * 0.25 * step_beats;
      }
      sound_beat = std::clamp(sound_beat, start_beat,
                              std::nextafter(end_beat, start_beat));

      const uint32_t frame = frame_for(sound_beat);
      if (extra) {
        extra_head_steps_[static_cast<size_t>(extra_idx)].store(
            step, std::memory_order_relaxed);
      } else {
        head_steps_[static_cast<size_t>(lane)].store(
            step, std::memory_order_relaxed);
        if (lane == focused) focused_playhead = step;
      }

      bool fired = false;
      if (armed || muted) {
        // Light walks; no emit, no visit for Pre/Nei while Rec is the source.
      } else {
        const bool on = cell.active.load(std::memory_order_relaxed);
        const float p_step = std::clamp(
            cell.probability.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const int cond =
            cell.condition.load(std::memory_order_relaxed);
        const uint8_t arg = cell.cond_arg.load(std::memory_order_relaxed);
        const int cycle_n = window_length > 0 ? idx / window_length : 0;

        bool ghost = false;
        bool hits = false;
        if (on) {
          hits = true;
        } else if (density > 1.0f) {
          const float ghost_chance =
              std::clamp(density - 1.0f, 0.0f, 1.0f) * master_prob;
          hits = ghost = ghost_chance >= 0.999f ||
                         (next_rng() / 4294967295.0f) < ghost_chance;
        }
        if (hits && !ghost && !cond_ok(voice, lane, step, cond, arg, cycle_n))
          hits = false;
        if (hits && on) {
          const float p_eff =
              std::clamp(p_step * std::min(density, 1.0f), 0.0f, 1.0f) *
              master_prob;
          hits = p_eff >= 0.999f || (next_rng() / 4294967295.0f) < p_eff;
        }
        if (hits && chaos > 0.0f &&
            (next_rng() / 4294967295.0f) < chaos * 0.25f)
          hits = !hits;

        if (!hits) {
          cancel_scheduler(voice, frame);
        } else {
          const int cell_note = cell.note.load(std::memory_order_relaxed);
          const int lane_note =
              lane_state.note.load(std::memory_order_relaxed);
          const int raw = (cell_note == kUnlockedNote ? lane_note : cell_note) +
                          transpose + extra_transpose;
          const int pitch =
              std::clamp(snap_to_scale(raw, scale, root), 0, 127);
          int velocity = std::clamp(
              cell.velocity.load(std::memory_order_relaxed), 1, 127);
          if (!ghost && cell.accent.load(std::memory_order_relaxed))
            velocity = std::min(127, velocity + 27);
          if (ghost) velocity = std::max(1, static_cast<int>(velocity * 0.7f));
          const bool tie = cell.tie.load(std::memory_order_relaxed);
          const bool legato =
              last_tied_[static_cast<size_t>(voice)] &&
              voice_state.pitch == pitch;

          int ratchet = cell.ratchet.load(std::memory_order_relaxed);
          if (ratchet < 1) ratchet = 1;
          int n = 1 + static_cast<int>(
                          std::lround((ratchet - 1) * ratchet_macro));
          if (n < 1) n = 1;
          if (tie || ghost) n = 1;

          voice_state.ratchet_left = 0;
          if (!legato) {
            stop_sounding(voice, frame);
            emit(frame, static_cast<uint8_t>(kNoteOn | channel),
                 static_cast<uint8_t>(pitch),
                 static_cast<uint8_t>(velocity));
            voice_state.pitch = pitch;
            voice_state.channel = channel;
          }
          const double next = beat_of(index + 1.0, step_beats);
          const double dur = std::max(1e-6, next - sound_beat);
          if (n <= 1) {
            voice_state.off_beat = sound_beat + dur * (tie ? 1.0 : gate);
            last_tied_[static_cast<size_t>(voice)] = tie;
          } else {
            voice_state.pulse_spacing = step_beats / n;
            voice_state.pulse_dur = gate * voice_state.pulse_spacing;
            voice_state.step_end_beat = next;
            voice_state.pulse_vel = velocity;
            voice_state.next_pulse_beat = sound_beat + voice_state.pulse_spacing;
            voice_state.ratchet_left = n - 1;
            voice_state.ratchet_pitch = pitch;
            voice_state.ratchet_channel = channel;
            voice_state.off_beat =
                std::min(sound_beat + voice_state.pulse_dur, next);
            last_tied_[static_cast<size_t>(voice)] = false;
            drain_ratchet(voice, muted);
          }
          fired = true;
        }
      }

      if (!armed) {
        last_fired_[static_cast<size_t>(voice)] = fired;
        if (!extra)
          last_on_[static_cast<size_t>(lane)][static_cast<size_t>(step)] =
              fired;
      }
    }

    if (!armed && !muted && voice_state.pitch >= 0 &&
        voice_state.off_beat < end_beat)
      stop_sounding(voice,
                    frame_for(std::max(voice_state.off_beat, start_beat)));
  };

  for (int h = 0; h < kLanes; ++h) {
    const int length = std::clamp(
        lanes_[static_cast<size_t>(h)].length.load(std::memory_order_relaxed),
        1, kMaxSteps);
    const int direction =
        lanes_[static_cast<size_t>(h)].direction.load(std::memory_order_relaxed);
    walk(h, h, false, 0, 1.0, length, 0, direction, 0, false);
  }
  for (int e = 0; e < kExtraHeads; ++e) {
    const ExtraHead& head = extra_heads_[static_cast<size_t>(e)];
    const bool extra_mute = head.mute.load(std::memory_order_relaxed);
    const int lane = std::clamp(head.lane.load(std::memory_order_relaxed), 0,
                                kLanes - 1);
    const int rate_i =
        std::clamp(head.rate.load(std::memory_order_relaxed), 0, kRateCount - 1);
    const int dir = head.direction.load(std::memory_order_relaxed);
    const int start = head.start.load(std::memory_order_relaxed);
    const int win = std::clamp(head.length.load(std::memory_order_relaxed), 1,
                               kMaxSteps);
    const int xpose = head.transpose.load(std::memory_order_relaxed);
    walk(kLanes + e, lane, true, e, kRates[rate_i], win, start, dir, xpose,
         extra_mute);
  }

  playhead_.store(focused_playhead, std::memory_order_relaxed);
}

void StepSequencerInstance::capture_events(size_t incoming_count,
                                           double start_beat,
                                           double block_beats, uint32_t frames) {
  const int pattern = current_pattern();
  const int focused = focused_index();

  for (size_t i = 0; i < incoming_count; ++i) {
    const MidiEvent& event = events_[i];
    if (event.size < 3) continue;
    const uint8_t status = event.data[0] & 0xF0u;
    const int note = event.data[1];
    const int velocity = event.data[2];
    const bool is_on = status == kNoteOn && velocity > 0;
    const bool is_off = status == kNoteOff || (status == kNoteOn && velocity == 0);
    if (!is_on && !is_off) continue;

    if (is_off) {
      for (Capture& cap : captures_) {
        if (cap.open && cap.pitch == note) {
          const int length = std::clamp(
              lanes_[static_cast<size_t>(cap.lane)].length.load(
                  std::memory_order_relaxed),
              1, kMaxSteps);
          const double event_beat =
              start_beat +
              static_cast<double>(event.frame) / frames * block_beats;
          const int div = std::clamp(
              lanes_[static_cast<size_t>(cap.lane)].division.load(
                  std::memory_order_relaxed),
              0, kDivisionCount - 1);
          const int64_t raw_index = static_cast<int64_t>(
              std::llround(event_beat / kDivisions[div]));
          close_capture(cap, raw_index, length);
          break;
        }
      }
      continue;
    }

    int dest = focused;
    for (int l = 0; l < kLanes; ++l) {
      if (lanes_[static_cast<size_t>(l)].note.load(std::memory_order_relaxed) ==
          note) {
        dest = l;
        break;
      }
    }

    const int div = std::clamp(
        lanes_[static_cast<size_t>(dest)].division.load(
            std::memory_order_relaxed),
        0, kDivisionCount - 1);
    const int length = std::clamp(
        lanes_[static_cast<size_t>(dest)].length.load(std::memory_order_relaxed),
        1, kMaxSteps);
    const double event_beat =
        start_beat + static_cast<double>(event.frame) / frames * block_beats;
    const int64_t raw_index =
        static_cast<int64_t>(std::llround(event_beat / kDivisions[div]));
    const int step =
        static_cast<int>(((raw_index % length) + length) % length);

    Capture& cap = captures_[static_cast<size_t>(dest)];
    if (cap.open) close_capture(cap, raw_index, length);

    StepCell& cell = cell_at(pattern, dest, step);
    const bool locked =
        cell.note.load(std::memory_order_relaxed) != kUnlockedNote;
    if (locked) cell.note.store(note, std::memory_order_relaxed);
    cell.velocity.store(std::clamp(velocity, 1, 127),
                        std::memory_order_relaxed);
    cell.active.store(true, std::memory_order_relaxed);
    cell.probability.store(1.0f, std::memory_order_relaxed);
    cell.accent.store(false, std::memory_order_relaxed);
    cell.tie.store(false, std::memory_order_relaxed);
    cap.open = true;
    cap.index = raw_index;
    cap.pitch = note;
    cap.velocity = std::clamp(velocity, 1, 127);
    cap.lane = dest;
    cap.pattern = pattern;
    cap.locked = locked;
  }
}

// Fills every step the open note actually rang through with a tie, so
// playback holds the same length rather than retriggering at each one — a
// hit that never made it past its own step is left with no tie at all, which
// is what leaves the gate knob free to shape it as usual.
void StepSequencerInstance::close_capture(Capture& cap, int64_t release_index,
                                          int length) {
  int64_t span = release_index - cap.index;
  if (span > 0) {
    span = std::min<int64_t>(span, length);
    for (int64_t i = 0; i < span; ++i) {
      const int step = static_cast<int>(
          ((cap.index + i) % length + length) % length);
      StepCell& cell = cell_at(cap.pattern, cap.lane, step);
      if (i > 0) {
        if (cap.locked)
          cell.note.store(cap.pitch, std::memory_order_relaxed);
        cell.velocity.store(cap.velocity, std::memory_order_relaxed);
        cell.active.store(true, std::memory_order_relaxed);
        cell.probability.store(1.0f, std::memory_order_relaxed);
        cell.accent.store(false, std::memory_order_relaxed);
      }
      cell.tie.store(true, std::memory_order_relaxed);
    }
  }
  cap.open = false;
}

size_t StepSequencerInstance::take_midi_output(MidiEvent* out, size_t capacity) {
  std::sort(events_.begin(), events_.begin() + event_count_,
            [](const MidiEvent& a, const MidiEvent& b) {
              if (a.frame != b.frame) return a.frame < b.frame;
              // Off before on when a new step cuts the previous on this frame.
              return (a.data[0] & 0xf0) < (b.data[0] & 0xf0);
            });
  const size_t count = std::min(event_count_, capacity);
  std::copy_n(events_.begin(), count, out);
  event_count_ = 0;
  return count;
}

void StepSequencerInstance::rotate_steps(int delta) {
  const int lane = focused_index();
  const int pattern = current_pattern();
  const int n = std::clamp(
      lanes_[static_cast<size_t>(lane)].length.load(std::memory_order_relaxed),
      1, kMaxSteps);
  if (n <= 1 || delta == 0) return;
  const int shift = ((delta % n) + n) % n;
  int notes[kMaxSteps], vels[kMaxSteps], ratchets[kMaxSteps];
  bool ons[kMaxSteps], acc[kMaxSteps], ties[kMaxSteps];
  float probs[kMaxSteps], micros[kMaxSteps];
  uint8_t conds[kMaxSteps], args[kMaxSteps];
  for (int i = 0; i < n; ++i) {
    const StepCell& cell = cell_at(pattern, lane, i);
    notes[i] = cell.note.load(std::memory_order_relaxed);
    vels[i] = cell.velocity.load(std::memory_order_relaxed);
    ons[i] = cell.active.load(std::memory_order_relaxed);
    probs[i] = cell.probability.load(std::memory_order_relaxed);
    acc[i] = cell.accent.load(std::memory_order_relaxed);
    ties[i] = cell.tie.load(std::memory_order_relaxed);
    micros[i] = cell.microtiming.load(std::memory_order_relaxed);
    ratchets[i] = cell.ratchet.load(std::memory_order_relaxed);
    conds[i] = cell.condition.load(std::memory_order_relaxed);
    args[i] = cell.cond_arg.load(std::memory_order_relaxed);
  }
  for (int i = 0; i < n; ++i) {
    const int src = (i - shift + n) % n;
    StepCell& cell = cell_at(pattern, lane, i);
    cell.note.store(notes[src], std::memory_order_relaxed);
    cell.velocity.store(vels[src], std::memory_order_relaxed);
    cell.active.store(ons[src], std::memory_order_relaxed);
    cell.probability.store(probs[src], std::memory_order_relaxed);
    cell.accent.store(acc[src], std::memory_order_relaxed);
    cell.tie.store(ties[src], std::memory_order_relaxed);
    cell.microtiming.store(micros[src], std::memory_order_relaxed);
    cell.ratchet.store(ratchets[src], std::memory_order_relaxed);
    cell.condition.store(conds[src], std::memory_order_relaxed);
    cell.cond_arg.store(args[src], std::memory_order_relaxed);
  }
}

void StepSequencerInstance::randomize_hits() {
  const int lane = focused_index();
  const int pattern = current_pattern();
  const int n = std::clamp(
      lanes_[static_cast<size_t>(lane)].length.load(std::memory_order_relaxed),
      1, kMaxSteps);
  for (int i = 0; i < n; ++i)
    cell_at(pattern, lane, i).active.store((next_ui_rng() & 1u) != 0,
                                          std::memory_order_relaxed);
}

void StepSequencerInstance::randomize_notes() {
  const int lane = focused_index();
  const int pattern = current_pattern();
  const int n = std::clamp(
      lanes_[static_cast<size_t>(lane)].length.load(std::memory_order_relaxed),
      1, kMaxSteps);
  const int scale = scale_.load(std::memory_order_relaxed);
  const int root = root_.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i) {
    StepCell& cell = cell_at(pattern, lane, i);
    if (cell.note.load(std::memory_order_relaxed) == kUnlockedNote) continue;
    const int raw = 48 + static_cast<int>(next_ui_rng() % 25u);
    cell.note.store(snap_to_scale(raw, scale, root), std::memory_order_relaxed);
  }
}

void StepSequencerInstance::clear_hits() {
  const int lane = focused_index();
  const int pattern = current_pattern();
  for (int i = 0; i < kMaxSteps; ++i)
    cell_at(pattern, lane, i).active.store(false, std::memory_order_relaxed);
}

void StepSequencerInstance::mutate_pattern() {
  const int pattern = current_pattern();
  const int scale = scale_.load(std::memory_order_relaxed);
  const int root = root_.load(std::memory_order_relaxed);
  for (int lane = 0; lane < kLanes; ++lane) {
    const int n = std::clamp(
        lanes_[static_cast<size_t>(lane)].length.load(std::memory_order_relaxed),
        1, kMaxSteps);
    for (int i = 0; i < n; ++i) {
      StepCell& cell = cell_at(pattern, lane, i);
      const bool on = cell.active.load(std::memory_order_relaxed);
      const uint32_t roll = next_ui_rng();
      if (on && (roll % 8u) == 0)
        cell.active.store(false, std::memory_order_relaxed);
      else if (!on && (roll % 16u) == 0)
        cell.active.store(true, std::memory_order_relaxed);
      const int note = cell.note.load(std::memory_order_relaxed);
      if (note == kUnlockedNote) continue;
      const int delta = (next_ui_rng() & 1u) ? 1 : -1;
      cell.note.store(snap_to_scale(note + delta, scale, root),
                      std::memory_order_relaxed);
    }
  }
}

void StepSequencerInstance::fill_euclidean(int pulses) {
  set_lane_euclid(focused_index(), pulses);
}

std::vector<ParameterInfo> StepSequencerInstance::parameters() const {
  // IDs 16-191 (the per-step shim) are deliberately not listed: an 8x64 grid
  // does not fit a generic slider wall, and the grid talks to the Mixer
  // snapshot instead (see mixer_model.cpp). set_parameter/parameter_value
  // still honor those IDs below, for an old MIDI learn map.
  return {
      {kDivision, "Division (0 1/4, 1 1/8, 2 1/16, 3 1/32, 4 1/4T, 5 1/8T)", 0.0,
       kDivisionCount - 1.0, 2.0},
      {kLength, "Steps", 1.0, kMaxSteps, kVisibleSteps},
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
      {kEuclid, "Euclid hits", 0.0, kMaxSteps, 0.0},
      {kRecordArm, "Record (capture live input into steps)", 0.0, 1.0, 0.0},
      {kDensity, "Density", 0.0, 2.0, 1.0},
      {kChaos, "Chaos", 0.0, 1.0, 0.0},
      {kRatchetAmount, "Ratchet amount", 0.0, 1.0, 0.0},
      {kMasterProb, "Master probability", 0.0, 1.0, 1.0},
      {kPattern, "Pattern", 0.0, kPatterns - 1.0, 0.0},
      {kNextPattern, "Next pattern (0 none, 1–16)", 0.0, kPatterns, 0.0},
      {kFill, "Fill", 0.0, 1.0, 0.0},
      {kFocusedLane, "Focused lane", 0.0, kLanes - 1.0, 0.0},
      {kView, "View (0 skyline, 1 grid)", 0.0, 1.0, 1.0},
      {kMutate, "Mutate", 0.0, 1.0, 0.0},
  };
}

double StepSequencerInstance::parameter_value(uint32_t id) const {
  const LaneState& lane = lanes_[static_cast<size_t>(focused_index())];
  switch (id) {
    case kDivision: return lane.division.load(std::memory_order_relaxed);
    case kLength: return lane.length.load(std::memory_order_relaxed);
    case kGate: return lane.gate.load(std::memory_order_relaxed);
    case kTranspose: return transpose_.load(std::memory_order_relaxed);
    case kChannel: return lane.channel.load(std::memory_order_relaxed) + 1;
    case kSwing: return swing_.load(std::memory_order_relaxed);
    case kDirection: return lane.direction.load(std::memory_order_relaxed);
    case kScale: return scale_.load(std::memory_order_relaxed);
    case kRoot: return root_.load(std::memory_order_relaxed);
    case kNudgeLeft:
    case kNudgeRight:
    case kRandomHits:
    case kRandomNotes:
    case kClearHits:
    case kMutate:
      return 0.0;
    case kEuclid: return lane.euclid.load(std::memory_order_relaxed);
    case kRecordArm:
      return record_armed_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    case kDensity: return macros_[0].load(std::memory_order_relaxed);
    case kChaos: return macros_[1].load(std::memory_order_relaxed);
    case kRatchetAmount: return macros_[2].load(std::memory_order_relaxed);
    case kMasterProb: return macros_[3].load(std::memory_order_relaxed);
    case kPattern: return pattern_.load(std::memory_order_relaxed);
    case kNextPattern: {
      const int next = next_pattern_.load(std::memory_order_relaxed);
      return next < 0 ? 0.0 : next + 1;
    }
    case kFill: return fill_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    case kFocusedLane: return focused_index();
    case kView: return view_.load(std::memory_order_relaxed);
    default: break;
  }
  if (id >= kStepNote && id < kStepNote + kVisibleSteps)
    return cell_at(0, 0, static_cast<int>(id - kStepNote))
        .note.load(std::memory_order_relaxed);
  if (id >= kStepVelocity && id < kStepVelocity + kVisibleSteps)
    return cell_at(0, 0, static_cast<int>(id - kStepVelocity))
        .velocity.load(std::memory_order_relaxed);
  if (id >= kStepActive && id < kStepActive + kVisibleSteps)
    return cell_at(0, 0, static_cast<int>(id - kStepActive))
                   .active.load(std::memory_order_relaxed)
               ? 1.0
               : 0.0;
  if (id >= kStepProb && id < kStepProb + kVisibleSteps)
    return cell_at(0, 0, static_cast<int>(id - kStepProb))
        .probability.load(std::memory_order_relaxed);
  if (id >= kStepAccent && id < kStepAccent + kVisibleSteps)
    return cell_at(0, 0, static_cast<int>(id - kStepAccent))
                   .accent.load(std::memory_order_relaxed)
               ? 1.0
               : 0.0;
  if (id >= kStepTie && id < kStepTie + kVisibleSteps)
    return cell_at(0, 0, static_cast<int>(id - kStepTie))
                   .tie.load(std::memory_order_relaxed)
               ? 1.0
               : 0.0;
  return 0.0;
}

void StepSequencerInstance::set_parameter(uint32_t id, double value) {
  LaneState& lane = lanes_[static_cast<size_t>(focused_index())];
  switch (id) {
    case kDivision:
      lane.division.store(clamp_int(value, 0, kDivisionCount - 1),
                          std::memory_order_relaxed);
      return;
    case kLength:
      lane.length.store(clamp_int(value, 1, kMaxSteps),
                        std::memory_order_relaxed);
      return;
    case kGate:
      lane.gate.store(std::clamp(value, 0.05, 1.0), std::memory_order_relaxed);
      return;
    case kTranspose:
      transpose_.store(clamp_int(value, -24, 24), std::memory_order_relaxed);
      return;
    case kChannel:
      lane.channel.store(clamp_int(value, 1, 16) - 1, std::memory_order_relaxed);
      return;
    case kSwing:
      swing_.store(static_cast<float>(std::clamp(value, 0.0, 1.0)),
                   std::memory_order_relaxed);
      return;
    case kDirection:
      lane.direction.store(clamp_int(value, 0, DirectionCount - 1),
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
      fill_euclidean(clamp_int(value, 0, kMaxSteps));
      return;
    case kRecordArm:
      record_armed_.store(value >= 0.5, std::memory_order_relaxed);
      return;
    case kDensity:
      macros_[0].store(static_cast<float>(std::clamp(value, 0.0, 2.0)),
                       std::memory_order_relaxed);
      return;
    case kChaos:
      macros_[1].store(static_cast<float>(std::clamp(value, 0.0, 1.0)),
                       std::memory_order_relaxed);
      return;
    case kRatchetAmount:
      macros_[2].store(static_cast<float>(std::clamp(value, 0.0, 1.0)),
                       std::memory_order_relaxed);
      return;
    case kMasterProb:
      macros_[3].store(static_cast<float>(std::clamp(value, 0.0, 1.0)),
                       std::memory_order_relaxed);
      return;
    case kPattern:
      pattern_.store(clamp_int(value, 0, kPatterns - 1),
                     std::memory_order_relaxed);
      return;
    case kNextPattern: {
      const int n = clamp_int(value, 0, kPatterns);
      next_pattern_.store(n <= 0 ? -1 : n - 1, std::memory_order_relaxed);
      return;
    }
    case kMutate:
      if (value >= 0.5) mutate_pattern();
      return;
    case kFill:
      fill_.store(value >= 0.5, std::memory_order_relaxed);
      return;
    case kFocusedLane:
      focus_.store(clamp_int(value, 0, kLanes - 1), std::memory_order_relaxed);
      return;
    case kView:
      view_.store(clamp_int(value, 0, 1), std::memory_order_relaxed);
      return;
    default:
      break;
  }
  if (id >= kStepNote && id < kStepNote + kVisibleSteps) {
    cell_at(0, 0, static_cast<int>(id - kStepNote))
        .note.store(clamp_int(value, 0, 127), std::memory_order_relaxed);
  } else if (id >= kStepVelocity && id < kStepVelocity + kVisibleSteps) {
    cell_at(0, 0, static_cast<int>(id - kStepVelocity))
        .velocity.store(clamp_int(value, 1, 127), std::memory_order_relaxed);
  } else if (id >= kStepActive && id < kStepActive + kVisibleSteps) {
    cell_at(0, 0, static_cast<int>(id - kStepActive))
        .active.store(value >= 0.5, std::memory_order_relaxed);
  } else if (id >= kStepProb && id < kStepProb + kVisibleSteps) {
    cell_at(0, 0, static_cast<int>(id - kStepProb))
        .probability.store(static_cast<float>(std::clamp(value, 0.0, 1.0)),
                           std::memory_order_relaxed);
  } else if (id >= kStepAccent && id < kStepAccent + kVisibleSteps) {
    cell_at(0, 0, static_cast<int>(id - kStepAccent))
        .accent.store(value >= 0.5, std::memory_order_relaxed);
  } else if (id >= kStepTie && id < kStepTie + kVisibleSteps) {
    cell_at(0, 0, static_cast<int>(id - kStepTie))
        .tie.store(value >= 0.5, std::memory_order_relaxed);
  }
}

std::vector<uint8_t> StepSequencerInstance::save_state() const {
  // One line per field, the same shape the other built-in plugins use, so a
  // session stays something a person can read and repair. Always v2: writing
  // v1 would drop lanes the next load could not restore.
  std::string text;
  text.reserve(512 * 1024);
  char line[128];

  std::snprintf(line, sizeof(line), "version 2\n");
  text += line;
  std::snprintf(line, sizeof(line), "swing %s\ndirection %d\nscale %d\nroot %d\n",
                format_number(swing_.load(std::memory_order_relaxed), 4).c_str(),
                lanes_[0].direction.load(std::memory_order_relaxed),
                scale_.load(std::memory_order_relaxed),
                root_.load(std::memory_order_relaxed));
  text += line;
  std::snprintf(line, sizeof(line), "transpose %d\npattern %d\nnext %d\n",
                transpose_.load(std::memory_order_relaxed),
                pattern_.load(std::memory_order_relaxed),
                next_pattern_.load(std::memory_order_relaxed));
  text += line;
  std::snprintf(line, sizeof(line), "fill %d\nview %d\nfocus %d\n",
                fill_.load(std::memory_order_relaxed) ? 1 : 0,
                view_.load(std::memory_order_relaxed),
                focus_.load(std::memory_order_relaxed));
  text += line;
  std::snprintf(line, sizeof(line), "macro %s %s %s %s\n",
                format_number(macros_[0].load(std::memory_order_relaxed), 4).c_str(),
                format_number(macros_[1].load(std::memory_order_relaxed), 4).c_str(),
                format_number(macros_[2].load(std::memory_order_relaxed), 4).c_str(),
                format_number(macros_[3].load(std::memory_order_relaxed), 4).c_str());
  text += line;

  for (int l = 0; l < kLanes; ++l) {
    const LaneState& lane = lanes_[static_cast<size_t>(l)];
    // Channel is 0-based on disk, matching v1's `channel` key.
    std::snprintf(line, sizeof(line), "lane %d %d %d %d %d %d %d %s %d\n", l,
                  lane.note.load(std::memory_order_relaxed),
                  lane.length.load(std::memory_order_relaxed),
                  lane.division.load(std::memory_order_relaxed),
                  lane.direction.load(std::memory_order_relaxed),
                  lane.channel.load(std::memory_order_relaxed),
                  lane.mute.load(std::memory_order_relaxed) ? 1 : 0,
                  format_number(lane.gate.load(std::memory_order_relaxed), 4).c_str(),
                  lane.euclid.load(std::memory_order_relaxed));
    text += line;
  }

  for (int h = 0; h < kExtraHeads; ++h) {
    const ExtraHead& head = extra_heads_[static_cast<size_t>(h)];
    std::snprintf(line, sizeof(line), "head %d %d %d %d %d %d %d %d\n", h,
                  head.lane.load(std::memory_order_relaxed),
                  head.rate.load(std::memory_order_relaxed),
                  head.direction.load(std::memory_order_relaxed),
                  head.start.load(std::memory_order_relaxed),
                  head.length.load(std::memory_order_relaxed),
                  head.mute.load(std::memory_order_relaxed) ? 1 : 0,
                  head.transpose.load(std::memory_order_relaxed));
    text += line;
  }

  for (int p = 0; p < kPatterns; ++p) {
    for (int l = 0; l < kLanes; ++l) {
      for (int s = 0; s < kMaxSteps; ++s) {
        const StepCell& cell = cell_at(p, l, s);
        const int note = cell.note.load(std::memory_order_relaxed);
        const int vel = cell.velocity.load(std::memory_order_relaxed);
        const bool on = cell.active.load(std::memory_order_relaxed);
        const float prob = cell.probability.load(std::memory_order_relaxed);
        const bool acc = cell.accent.load(std::memory_order_relaxed);
        const bool tie = cell.tie.load(std::memory_order_relaxed);
        const float micro = cell.microtiming.load(std::memory_order_relaxed);
        const int ratchet = cell.ratchet.load(std::memory_order_relaxed);
        const int cond =
            static_cast<int>(cell.condition.load(std::memory_order_relaxed));
        const int arg =
            static_cast<int>(cell.cond_arg.load(std::memory_order_relaxed));
        // Skip a blank cell so a saved strip is not 8192 empty lines.
        if (!on && note == kUnlockedNote && vel == 100 && prob >= 0.999f &&
            !acc && !tie && micro == 0.0f && ratchet <= 1 && cond == 0 &&
            arg == 0)
          continue;
        std::snprintf(line, sizeof(line),
                      "pstep %d %d %d %d %d %d %s %d %d %s %d %d %d\n", p,
                      l, s, note, vel, on ? 1 : 0,
                      format_number(prob, 4).c_str(), acc ? 1 : 0,
                      tie ? 1 : 0, format_number(micro, 4).c_str(), ratchet,
                      cond, arg);
        text += line;
      }
    }
  }
  return {text.begin(), text.end()};
}

bool StepSequencerInstance::load_state(const std::vector<uint8_t>& blob) {
  // A 2 MiB junk file is not a session: leave the constructor seed alone
  // rather than walking it. Empty is the same — nothing to overlay.
  if (blob.empty() || blob.size() > kMaxBlobBytes) return true;

  reset_blank();

  const std::string text(blob.begin(), blob.end());
  size_t position = 0;
  int step = 0;
  int version = 1;
  int line_count = 0;
  int pstep_count = 0;

  while (position < text.size()) {
    if (line_count >= kMaxStateLines) break;
    ++line_count;

    const size_t end = text.find('\n', position);
    std::string_view line(
        text.data() + position,
        (end == std::string::npos ? text.size() : end) - position);
    position = (end == std::string::npos) ? text.size() : end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line.front() == '#') continue;

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
    if (key == "version" && number(&value)) {
      version = static_cast<int>(value);
      continue;
    }

    if (version >= 2 && key == "step") continue;

    if (key == "division" && version < 2 && number(&value)) {
      set_parameter(kDivision, value);
    } else if (key == "length" && version < 2 && number(&value)) {
      set_parameter(kLength, value);
    } else if (key == "gate" && version < 2 && number(&value)) {
      set_parameter(kGate, value);
    } else if (key == "transpose" && number(&value)) {
      set_parameter(kTranspose, value);
    } else if (key == "channel" && version < 2 && number(&value)) {
      set_parameter(kChannel, value + 1);
    } else if (key == "swing" && number(&value)) {
      set_parameter(kSwing, value);
    } else if (key == "direction" && number(&value)) {
      set_parameter(kDirection, value);
    } else if (key == "scale" && number(&value)) {
      set_parameter(kScale, value);
    } else if (key == "root" && number(&value)) {
      set_parameter(kRoot, value);
    } else if (key == "pattern" && number(&value)) {
      pattern_.store(clamp_int(value, 0, kPatterns - 1),
                     std::memory_order_relaxed);
    } else if (key == "next" && number(&value)) {
      const int next = static_cast<int>(std::lround(value));
      next_pattern_.store(next < 0 ? -1 : std::clamp(next, 0, kPatterns - 1),
                          std::memory_order_relaxed);
    } else if (key == "fill" && number(&value)) {
      fill_.store(value >= 0.5, std::memory_order_relaxed);
    } else if (key == "view" && number(&value)) {
      view_.store(clamp_int(value, 0, 1), std::memory_order_relaxed);
    } else if (key == "focus" && number(&value)) {
      focus_.store(clamp_int(value, 0, kLanes - 1), std::memory_order_relaxed);
    } else if (key == "macro") {
      double m0 = 1.0, m1 = 0.0, m2 = 0.0, m3 = 1.0;
      if (number(&m0)) macros_[0].store(static_cast<float>(std::clamp(m0, 0.0, 2.0)),
                                        std::memory_order_relaxed);
      if (number(&m1)) macros_[1].store(static_cast<float>(std::clamp(m1, 0.0, 1.0)),
                                        std::memory_order_relaxed);
      if (number(&m2)) macros_[2].store(static_cast<float>(std::clamp(m2, 0.0, 1.0)),
                                        std::memory_order_relaxed);
      if (number(&m3)) macros_[3].store(static_cast<float>(std::clamp(m3, 0.0, 1.0)),
                                        std::memory_order_relaxed);
    } else if (key == "lane") {
      double idx = 0.0;
      if (!number(&idx)) continue;
      const int l = static_cast<int>(std::lround(idx));
      if (l < 0 || l >= kLanes) continue;
      LaneState& lane = lanes_[static_cast<size_t>(l)];
      double note = 60, length = kVisibleSteps, div = 2, dir = 0, ch = 0,
             mute = 0, gate = 0.5, euclid = 0;
      if (number(&note))
        lane.note.store(clamp_int(note, 0, 127), std::memory_order_relaxed);
      if (number(&length))
        lane.length.store(clamp_int(length, 1, kMaxSteps),
                          std::memory_order_relaxed);
      if (number(&div))
        lane.division.store(clamp_int(div, 0, kDivisionCount - 1),
                            std::memory_order_relaxed);
      if (number(&dir))
        lane.direction.store(clamp_int(dir, 0, DirectionCount - 1),
                             std::memory_order_relaxed);
      if (number(&ch))
        lane.channel.store(clamp_int(ch, 0, 15), std::memory_order_relaxed);
      if (number(&mute))
        lane.mute.store(mute >= 0.5, std::memory_order_relaxed);
      if (number(&gate))
        lane.gate.store(std::clamp(gate, 0.05, 1.0), std::memory_order_relaxed);
      if (number(&euclid))
        lane.euclid.store(clamp_int(euclid, 0, kMaxSteps),
                          std::memory_order_relaxed);
    } else if (key == "head") {
      double idx = 0.0;
      if (!number(&idx)) continue;
      const int h = static_cast<int>(std::lround(idx));
      if (h < 0 || h >= kExtraHeads) continue;
      ExtraHead& head = extra_heads_[static_cast<size_t>(h)];
      double lane = 0, rate = 0, dir = 0, start = 0, length = kVisibleSteps,
             mute = 1, transpose = 0;
      if (number(&lane))
        head.lane.store(clamp_int(lane, 0, kLanes - 1),
                        std::memory_order_relaxed);
      if (number(&rate))
        head.rate.store(clamp_int(rate, 0, 4), std::memory_order_relaxed);
      if (number(&dir))
        head.direction.store(clamp_int(dir, 0, DirectionCount - 1),
                             std::memory_order_relaxed);
      if (number(&start))
        head.start.store(clamp_int(start, 0, kMaxSteps - 1),
                         std::memory_order_relaxed);
      if (number(&length))
        head.length.store(clamp_int(length, 1, kMaxSteps),
                          std::memory_order_relaxed);
      if (number(&mute))
        head.mute.store(mute >= 0.5, std::memory_order_relaxed);
      if (number(&transpose))
        head.transpose.store(clamp_int(transpose, -24, 24),
                             std::memory_order_relaxed);
    } else if (key == "pstep") {
      if (pstep_count >= kMaxPstepLines) continue;
      ++pstep_count;
      double pat = 0, lane = 0, idx = 0, note = 60, vel = 100, on = 0,
             prob = 1.0, acc = 0, tie = 0, micro = 0, ratchet = 1, cond = 0,
             arg = 0;
      if (!(number(&pat) && number(&lane) && number(&idx))) continue;
      const int p = static_cast<int>(std::lround(pat));
      const int l = static_cast<int>(std::lround(lane));
      const int s = static_cast<int>(std::lround(idx));
      if (!cell_in_range(p, l, s)) continue;
      StepCell& cell = cell_at(p, l, s);
      if (number(&note))
        cell.note.store(clamp_note(static_cast<int>(std::lround(note))),
                        std::memory_order_relaxed);
      if (number(&vel))
        cell.velocity.store(clamp_int(vel, 1, 127), std::memory_order_relaxed);
      if (number(&on))
        cell.active.store(on >= 0.5, std::memory_order_relaxed);
      if (number(&prob))
        cell.probability.store(static_cast<float>(std::clamp(prob, 0.0, 1.0)),
                               std::memory_order_relaxed);
      if (number(&acc))
        cell.accent.store(acc >= 0.5, std::memory_order_relaxed);
      if (number(&tie))
        cell.tie.store(tie >= 0.5, std::memory_order_relaxed);
      if (number(&micro))
        cell.microtiming.store(
            static_cast<float>(std::clamp(micro, -0.5, 0.5)),
            std::memory_order_relaxed);
      if (number(&ratchet))
        cell.ratchet.store(clamp_int(ratchet, 0, 8), std::memory_order_relaxed);
      if (number(&cond))
        cell.condition.store(static_cast<uint8_t>(clamp_int(cond, 0, 6)),
                             std::memory_order_relaxed);
      if (number(&arg))
        cell.cond_arg.store(static_cast<uint8_t>(clamp_int(arg, 0, 255)),
                            std::memory_order_relaxed);
    } else if (key == "step" && version < 2 && step < kMaxSteps) {
      double velocity = 0.0;
      double on = 0.0;
      if (number(&value) && number(&velocity) && number(&on)) {
        StepCell& cell = cell_at(0, 0, step);
        cell.note.store(clamp_int(value, 0, 127), std::memory_order_relaxed);
        cell.velocity.store(clamp_int(velocity, 1, 127),
                            std::memory_order_relaxed);
        cell.active.store(on >= 0.5, std::memory_order_relaxed);
        double prob = 1.0, acc = 0.0, tie = 0.0;
        if (number(&prob))
          cell.probability.store(static_cast<float>(std::clamp(prob, 0.0, 1.0)),
                                 std::memory_order_relaxed);
        if (number(&acc))
          cell.accent.store(acc >= 0.5, std::memory_order_relaxed);
        if (number(&tie))
          cell.tie.store(tie >= 0.5, std::memory_order_relaxed);
      }
      ++step;
    }
  }
  return true;
}

}  // namespace nirbija
