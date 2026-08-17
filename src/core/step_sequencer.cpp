#include "core/step_sequencer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

  // One block of ids per column of the grid, so a step's three values are
  // kStepNote + i, kStepVelocity + i, kStepActive + i.
  kStepNote = 16,
  kStepVelocity = 48,
  kStepActive = 80,
};

constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kNoteOff = 0x80;

int clamp_int(double value, int low, int high) {
  const int rounded = static_cast<int>(std::lround(value));
  return std::clamp(rounded, low, high);
}

}  // namespace

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
  }
}

bool StepSequencerInstance::activate(double sample_rate, uint32_t) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  sounding_note_ = -1;
  last_step_beat_ = -1.0;
  event_count_ = 0;
  return true;
}

void StepSequencerInstance::deactivate() {
  sounding_note_ = -1;
  last_step_beat_ = -1.0;
  event_count_ = 0;
}

double StepSequencerInstance::beats_per_step() const {
  const int index =
      std::clamp(division_.load(std::memory_order_relaxed), 0, kDivisionCount - 1);
  return kDivisions[index];
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
}

void StepSequencerInstance::queue_midi(const MidiEvent& event) {
  // Passed along untouched: the sequencer adds to the chain, it does not own it.
  if (event_count_ >= kMaxEvents) return;
  events_[event_count_++] = event;
}

void StepSequencerInstance::process(const float* const*, float* const*,
                                    uint32_t frames) {
  if (frames == 0) return;

  // Stopped, or the transport jumped: whatever is held has to be let go, or a
  // synth downstream is left ringing on a note nobody will ever release.
  if (!transport_.playing || transport_.changed) {
    stop_sounding(0);
    last_step_beat_ = -1.0;
    if (!transport_.playing) {
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

  // Beat to frame within this block. Never past the last frame, since an event
  // landing on the boundary belongs to the next block, not off the end of this
  // one.
  const auto frame_for = [&](double beat) {
    const double offset = (beat - start_beat) / block_beats * frames;
    return static_cast<uint32_t>(
        std::clamp(offset, 0.0, static_cast<double>(frames - 1)));
  };

  // The note held from an earlier block may be due to end inside this one.
  if (sounding_note_ >= 0 && sounding_off_ < end_beat)
    stop_sounding(frame_for(std::max(sounding_off_, start_beat)));

  // Every step boundary that falls inside this block. The loop is bounded by
  // the block, not by the pattern, so a long block or a fast division cannot
  // spin: at most kMaxEvents get out anyway.
  const double first_index = std::ceil(start_beat / step_beats);
  for (double index = first_index; index * step_beats < end_beat; index += 1.0) {
    const double beat = index * step_beats;
    // A block boundary can land exactly on a step, and the next block starts
    // on the same beat. Without this the step plays twice.
    if (beat <= last_step_beat_) continue;
    last_step_beat_ = beat;

    const int step = static_cast<int>(std::fmod(index, static_cast<double>(length)));
    const uint32_t frame = frame_for(beat);
    playhead_.store(step, std::memory_order_relaxed);

    // The previous note ends where this step starts, even if its gate said
    // longer: one note at a time is the whole point.
    stop_sounding(frame);

    if (!active_[step].load(std::memory_order_relaxed)) continue;

    const int pitch = std::clamp(note_[step].load(std::memory_order_relaxed) +
                                     transpose_.load(std::memory_order_relaxed),
                                 0, 127);
    const int velocity =
        std::clamp(velocity_[step].load(std::memory_order_relaxed), 1, 127);

    emit(frame, static_cast<uint8_t>(kNoteOn | channel),
         static_cast<uint8_t>(pitch), static_cast<uint8_t>(velocity));
    sounding_note_ = pitch;
    sounding_off_ =
        beat + step_beats * std::clamp(gate_.load(std::memory_order_relaxed),
                                       0.05, 1.0);
  }
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

std::vector<ParameterInfo> StepSequencerInstance::parameters() const {
  std::vector<ParameterInfo> info{
      {kDivision, "Division (0 1/4, 1 1/8, 2 1/16, 3 1/32, 4 1/4T, 5 1/8T)", 0.0,
       kDivisionCount - 1.0, 2.0},
      {kLength, "Steps", 1.0, kSteps, kSteps},
      {kGate, "Gate", 0.05, 1.0, 0.5},
      {kTranspose, "Transpose", -24.0, 24.0, 0.0},
      {kChannel, "MIDI channel", 1.0, 16.0, 1.0},
  };
  info.reserve(info.size() + kSteps * 3);
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
  return info;
}

double StepSequencerInstance::parameter_value(uint32_t id) const {
  switch (id) {
    case kDivision: return division_.load(std::memory_order_relaxed);
    case kLength: return length_.load(std::memory_order_relaxed);
    case kGate: return gate_.load(std::memory_order_relaxed);
    case kTranspose: return transpose_.load(std::memory_order_relaxed);
    case kChannel: return channel_.load(std::memory_order_relaxed) + 1;
    default: break;
  }
  if (id >= kStepNote && id < kStepNote + kSteps)
    return note_[id - kStepNote].load(std::memory_order_relaxed);
  if (id >= kStepVelocity && id < kStepVelocity + kSteps)
    return velocity_[id - kStepVelocity].load(std::memory_order_relaxed);
  if (id >= kStepActive && id < kStepActive + kSteps)
    return active_[id - kStepActive].load(std::memory_order_relaxed) ? 1.0 : 0.0;
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
      // 1..16 to the user, 0..15 on the wire.
      channel_.store(clamp_int(value, 1, 16) - 1, std::memory_order_relaxed);
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
  }
}

std::vector<uint8_t> StepSequencerInstance::save_state() const {
  // One line per field, the same shape the other built-in plugins use, so a
  // session stays something a person can read and repair.
  std::string text;
  char line[64];
  std::snprintf(line, sizeof(line), "division %d\nlength %d\ngate %.4f\n",
                division_.load(std::memory_order_relaxed),
                length_.load(std::memory_order_relaxed),
                gate_.load(std::memory_order_relaxed));
  text += line;
  std::snprintf(line, sizeof(line), "transpose %d\nchannel %d\n",
                transpose_.load(std::memory_order_relaxed),
                channel_.load(std::memory_order_relaxed));
  text += line;
  for (int i = 0; i < kSteps; ++i) {
    std::snprintf(line, sizeof(line), "step %d %d %d\n",
                  note_[i].load(std::memory_order_relaxed),
                  velocity_[i].load(std::memory_order_relaxed),
                  active_[i].load(std::memory_order_relaxed) ? 1 : 0);
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

    // A hand-edited or truncated session gives back the default rather than
    // throwing, which is why parse_number exists at all.
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
      // Stored on the wire, and set_parameter takes it 1-based.
      set_parameter(kChannel, value + 1);
    } else if (key == "step" && step < kSteps) {
      double velocity = 0.0;
      double on = 0.0;
      if (number(&value) && number(&velocity) && number(&on)) {
        set_parameter(kStepNote + step, value);
        set_parameter(kStepVelocity + step, velocity);
        set_parameter(kStepActive + step, on);
      }
      ++step;
    }
  }
  return true;
}

}  // namespace nirbija
