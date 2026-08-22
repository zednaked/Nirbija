#include "core/arpeggiator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace nirbija {
namespace {

// Beats per step, in quarter notes; the index is the `division` parameter.
// Deliberately the same table and the same order as the step sequencer's, so
// the two read alike when they sit in one strip.
constexpr double kDivisions[] = {1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0};
constexpr int kDivisionCount = static_cast<int>(std::size(kDivisions));

enum Params : uint32_t {
  kDivision = 0,
  kMode = 1,
  kOctaves = 2,
  kGate = 3,
  kTranspose = 4,
  kLatch = 5,
  kThru = 6,
};

constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kNoteOff = 0x80;

int clamp_int(double value, int low, int high) {
  return std::clamp(static_cast<int>(std::lround(value)), low, high);
}

}  // namespace

PluginDescriptor ArpeggiatorInstance::make_descriptor() {
  PluginDescriptor descriptor;
  descriptor.format = PluginFormat::Internal;
  descriptor.uid = "nirbija.arp";
  descriptor.name = "Arpeggiator";
  descriptor.vendor = "Nirbija";
  descriptor.audio_inputs = 0;
  descriptor.audio_outputs = 0;
  descriptor.has_midi_input = true;
  descriptor.category = "Arpeggiator";
  descriptor.kind = PluginKind::MidiEffect;
  return descriptor;
}

ArpeggiatorInstance::ArpeggiatorInstance() : descriptor_(make_descriptor()) {}

bool ArpeggiatorInstance::activate(double sample_rate, uint32_t) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  deactivate();
  return true;
}

void ArpeggiatorInstance::deactivate() {
  held_count_ = 0;
  sounding_count_ = 0;
  position_ = 0;
  last_step_beat_ = -1.0;
  event_count_ = 0;
  keys_down_count_ = 0;
  key_held_.fill(false);
  restart_on_next_ = false;
  playhead_.store(-1, std::memory_order_relaxed);
}

double ArpeggiatorInstance::beats_per_step() const {
  return kDivisions[std::clamp(division_.load(std::memory_order_relaxed), 0,
                               kDivisionCount - 1)];
}

void ArpeggiatorInstance::emit(uint32_t frame, uint8_t status, uint8_t data1,
                               uint8_t data2) {
  if (event_count_ >= kMaxEvents) return;
  MidiEvent& event = events_[event_count_++];
  event.frame = frame;
  event.size = 3;
  event.data[0] = status;
  event.data[1] = data1;
  event.data[2] = data2;
}

void ArpeggiatorInstance::release_all(uint32_t frame) {
  for (size_t i = 0; i < sounding_count_; ++i)
    emit(frame, kNoteOff, sounding_[i], 0);
  sounding_count_ = 0;
}

void ArpeggiatorInstance::hold(uint8_t note, uint8_t velocity) {
  for (size_t i = 0; i < held_count_; ++i) {
    if (held_[i].note == note) {
      held_[i].velocity = velocity;  // struck again, harder or softer
      return;
    }
  }
  if (held_count_ >= kMaxHeld) return;
  held_[held_count_++] = {note, velocity};
}

void ArpeggiatorInstance::drop(uint8_t note) {
  for (size_t i = 0; i < held_count_; ++i) {
    if (held_[i].note != note) continue;
    // Order matters for AsPlayed, so this closes the gap rather than swapping
    // the last one into it.
    for (size_t j = i; j + 1 < held_count_; ++j) held_[j] = held_[j + 1];
    --held_count_;
    return;
  }
}

void ArpeggiatorInstance::queue_midi(const MidiEvent& event) {
  if (thru_.load(std::memory_order_relaxed)) {
    // An arpeggiator normally replaces what it is given - you hold a chord and
    // hear the figure, not the chord - so passing it on is opt-in.
    if (event_count_ < kMaxEvents) events_[event_count_++] = event;
  }
  if (event.size < 3) return;

  const uint8_t status = event.data[0] & 0xf0;
  const uint8_t note = event.data[1];
  const uint8_t velocity = event.data[2];

  if (status == kNoteOn && velocity > 0) {
    // With latch on, the first key after everything came up starts a new chord
    // instead of adding to the one still ringing.
    if (restart_on_next_) {
      held_count_ = 0;
      restart_on_next_ = false;
    }
    hold(note, velocity);
    if (note < key_held_.size() && !key_held_[note]) {
      key_held_[note] = true;
      ++keys_down_count_;
    }
    return;
  }
  if (status == kNoteOff || (status == kNoteOn && velocity == 0)) {
    if (note < key_held_.size() && key_held_[note]) {
      key_held_[note] = false;
      if (keys_down_count_ > 0) --keys_down_count_;
    }
    if (latch_.load(std::memory_order_relaxed)) {
      // The chord stays until every key is up. One note-off mid-hold is
      // not the start of a new chord.
      if (keys_down_count_ == 0) restart_on_next_ = true;
      return;
    }
    drop(note);
  }
}

size_t ArpeggiatorInstance::figure(std::array<Held, kMaxHeld>& out) const {
  const size_t count = held_count_;
  if (count == 0) return 0;
  std::copy_n(held_.begin(), count, out.begin());

  const int mode = mode_.load(std::memory_order_relaxed);
  if (mode == AsPlayed || mode == Chord || mode == Random) return count;

  // Insertion sort: sixteen elements at most, and it allocates nothing.
  for (size_t i = 1; i < count; ++i) {
    Held key = out[i];
    size_t j = i;
    while (j > 0 && out[j - 1].note > key.note) {
      out[j] = out[j - 1];
      --j;
    }
    out[j] = key;
  }
  if (mode == Down || mode == DownUp) std::reverse(out.begin(), out.begin() + count);
  return count;
}

void ArpeggiatorInstance::play_position(size_t position, uint32_t frame) {
  std::array<Held, kMaxHeld> notes{};
  const size_t count = figure(notes);
  if (count == 0) return;

  const int mode = mode_.load(std::memory_order_relaxed);
  const int octaves = std::clamp(octaves_.load(std::memory_order_relaxed), 1, 4);
  const int transpose = transpose_.load(std::memory_order_relaxed);

  const auto send = [&](const Held& held, int octave) {
    const int pitch = std::clamp(held.note + octave * 12 + transpose, 0, 127);
    emit(frame, kNoteOn, static_cast<uint8_t>(pitch), held.velocity);
    if (sounding_count_ < kMaxSounding)
      sounding_[sounding_count_++] = static_cast<uint8_t>(pitch);
  };

  if (mode == Chord) {
    // The whole handful, on the beat. The octave stack still applies.
    for (int octave = 0; octave < octaves; ++octave)
      for (size_t i = 0; i < count; ++i) send(notes[i], octave);
    playhead_.store(0, std::memory_order_relaxed);
    return;
  }

  const size_t span = count * static_cast<size_t>(octaves);
  size_t index = 0;

  if (mode == Random) {
    // xorshift: the audio thread cannot call into a random engine that might
    // lock or allocate, and this needs no more randomness than a coin.
    random_state_ ^= random_state_ << 13;
    random_state_ ^= random_state_ >> 17;
    random_state_ ^= random_state_ << 5;
    index = random_state_ % span;
  } else if (mode == UpDown || mode == DownUp) {
    // Turns without striking the ends twice, which is what makes it sound like
    // a figure rather than a stutter. A single note has no turn to make.
    const size_t cycle = span > 1 ? span * 2 - 2 : 1;
    const size_t step = position % cycle;
    index = step < span ? step : cycle - step;
  } else {
    index = position % span;
  }

  send(notes[index % count], static_cast<int>(index / count));
  playhead_.store(static_cast<int>(index), std::memory_order_relaxed);
}

void ArpeggiatorInstance::process(const float* const*, float* const*,
                                  uint32_t frames) {
  if (frames == 0) return;

  if (!transport_.playing || transport_.changed) {
    release_all(0);
    last_step_beat_ = -1.0;
    position_ = 0;
    if (!transport_.playing) {
      playhead_.store(-1, std::memory_order_relaxed);
      return;
    }
  }

  // Nothing held: let go of whatever is still ringing and wait at the start,
  // so the next chord begins at the top of the figure rather than mid-way.
  if (held_count_ == 0) {
    release_all(0);
    position_ = 0;
    playhead_.store(-1, std::memory_order_relaxed);
    return;
  }

  const double tempo = transport_.tempo_bpm > 0.0 ? transport_.tempo_bpm : 120.0;
  const double block_beats =
      static_cast<double>(frames) / sample_rate_ * tempo / 60.0;
  if (block_beats <= 0.0) return;

  const double start_beat = transport_.beats;
  const double end_beat = start_beat + block_beats;
  const double step_beats = beats_per_step();

  const auto frame_for = [&](double beat) {
    const double offset = (beat - start_beat) / block_beats * frames;
    return static_cast<uint32_t>(
        std::clamp(offset, 0.0, static_cast<double>(frames - 1)));
  };

  if (sounding_count_ > 0 && sounding_off_ < end_beat)
    release_all(frame_for(std::max(sounding_off_, start_beat)));

  const double first_index = std::ceil(start_beat / step_beats);
  for (double index = first_index; index * step_beats < end_beat; index += 1.0) {
    const double beat = index * step_beats;
    if (beat <= last_step_beat_) continue;  // the same beat, reported twice
    last_step_beat_ = beat;

    const uint32_t frame = frame_for(beat);
    release_all(frame);
    play_position(position_++, frame);
    sounding_off_ =
        beat + step_beats * std::clamp(gate_.load(std::memory_order_relaxed),
                                       0.05, 1.0);
  }
}

size_t ArpeggiatorInstance::take_midi_output(MidiEvent* out, size_t capacity) {
  std::sort(events_.begin(), events_.begin() + event_count_,
            [](const MidiEvent& a, const MidiEvent& b) {
              if (a.frame != b.frame) return a.frame < b.frame;
              // Off before on when a repeated pitch turns over on this frame.
              return (a.data[0] & 0xf0) < (b.data[0] & 0xf0);
            });
  const size_t count = std::min(event_count_, capacity);
  std::copy_n(events_.begin(), count, out);
  event_count_ = 0;
  return count;
}

std::vector<ParameterInfo> ArpeggiatorInstance::parameters() const {
  return {
      {kDivision, "Division (0 1/4, 1 1/8, 2 1/16, 3 1/32, 4 1/4T, 5 1/8T)", 0.0,
       kDivisionCount - 1.0, 2.0},
      {kMode, "Mode (0 up, 1 down, 2 up-down, 3 down-up, 4 played, 5 random, 6 chord)",
       0.0, static_cast<double>(ModeCount) - 1.0, 0.0},
      {kOctaves, "Octaves", 1.0, 4.0, 1.0},
      {kGate, "Gate", 0.05, 1.0, 0.5},
      {kTranspose, "Transpose", -24.0, 24.0, 0.0},
      {kLatch, "Latch", 0.0, 1.0, 0.0},
      {kThru, "Pass input through", 0.0, 1.0, 0.0},
  };
}

double ArpeggiatorInstance::parameter_value(uint32_t id) const {
  switch (id) {
    case kDivision: return division_.load(std::memory_order_relaxed);
    case kMode: return mode_.load(std::memory_order_relaxed);
    case kOctaves: return octaves_.load(std::memory_order_relaxed);
    case kGate: return gate_.load(std::memory_order_relaxed);
    case kTranspose: return transpose_.load(std::memory_order_relaxed);
    case kLatch: return latch_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    case kThru: return thru_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    default: return 0.0;
  }
}

void ArpeggiatorInstance::set_parameter(uint32_t id, double value) {
  switch (id) {
    case kDivision:
      division_.store(clamp_int(value, 0, kDivisionCount - 1),
                      std::memory_order_relaxed);
      break;
    case kMode:
      mode_.store(clamp_int(value, 0, ModeCount - 1), std::memory_order_relaxed);
      break;
    case kOctaves:
      octaves_.store(clamp_int(value, 1, 4), std::memory_order_relaxed);
      break;
    case kGate:
      gate_.store(std::clamp(value, 0.05, 1.0), std::memory_order_relaxed);
      break;
    case kTranspose:
      transpose_.store(clamp_int(value, -24, 24), std::memory_order_relaxed);
      break;
    case kLatch:
      latch_.store(value >= 0.5, std::memory_order_relaxed);
      break;
    case kThru:
      thru_.store(value >= 0.5, std::memory_order_relaxed);
      break;
    default:
      break;
  }
}

std::vector<uint8_t> ArpeggiatorInstance::save_state() const {
  // The held chord is performance, not configuration, so only the knobs are
  // written - the same line the looper draws about its recorded audio.
  char text[192];
  const int written = std::snprintf(
      text, sizeof(text),
      "division %d\nmode %d\noctaves %d\ngate %.4f\ntranspose %d\nlatch %d\n"
      "thru %d\n",
      division_.load(std::memory_order_relaxed),
      mode_.load(std::memory_order_relaxed),
      octaves_.load(std::memory_order_relaxed),
      gate_.load(std::memory_order_relaxed),
      transpose_.load(std::memory_order_relaxed),
      latch_.load(std::memory_order_relaxed) ? 1 : 0,
      thru_.load(std::memory_order_relaxed) ? 1 : 0);
  if (written <= 0) return {};
  return {text, text + written};
}

bool ArpeggiatorInstance::load_state(const std::vector<uint8_t>& blob) {
  const std::string text(blob.begin(), blob.end());
  size_t position = 0;
  while (position < text.size()) {
    const size_t end = text.find('\n', position);
    const std::string_view line(
        text.data() + position,
        (end == std::string::npos ? text.size() : end) - position);
    position = (end == std::string::npos) ? text.size() : end + 1;

    const size_t space = line.find(' ');
    if (space == std::string_view::npos) continue;
    const std::string_view key = line.substr(0, space);

    double value = 0.0;
    if (!parse_number(line.substr(space + 1), &value)) continue;

    if (key == "division") set_parameter(kDivision, value);
    else if (key == "mode") set_parameter(kMode, value);
    else if (key == "octaves") set_parameter(kOctaves, value);
    else if (key == "gate") set_parameter(kGate, value);
    else if (key == "transpose") set_parameter(kTranspose, value);
    else if (key == "latch") set_parameter(kLatch, value);
    else if (key == "thru") set_parameter(kThru, value);
  }
  return true;
}

}  // namespace nirbija
