#include "core/chord.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace nirbija {
namespace {

struct ScaleDef {
  const int* intervals;  // semitones above the root, ascending, in [0,11]
  int count;
};

constexpr int kMajor[] = {0, 2, 4, 5, 7, 9, 11};
constexpr int kDorian[] = {0, 2, 3, 5, 7, 9, 10};
constexpr int kPhrygian[] = {0, 1, 3, 5, 7, 8, 10};
constexpr int kLydian[] = {0, 2, 4, 6, 7, 9, 11};
constexpr int kMixolydian[] = {0, 2, 4, 5, 7, 9, 10};
constexpr int kNaturalMinor[] = {0, 2, 3, 5, 7, 8, 10};
constexpr int kLocrian[] = {0, 1, 3, 5, 6, 8, 10};
constexpr int kHarmonicMinor[] = {0, 2, 3, 5, 7, 8, 11};
constexpr int kMelodicMinor[] = {0, 2, 3, 5, 7, 9, 11};
constexpr int kPentatonicMajor[] = {0, 2, 4, 7, 9};
constexpr int kPentatonicMinor[] = {0, 3, 5, 7, 10};
constexpr int kBlues[] = {0, 3, 5, 6, 7, 10};

// Same order as ChordInstance::Scale, so the enum indexes straight in.
constexpr ScaleDef kScales[] = {
    {kMajor, 7},         {kDorian, 7},         {kPhrygian, 7},
    {kLydian, 7},        {kMixolydian, 7},     {kNaturalMinor, 7},
    {kLocrian, 7},       {kHarmonicMinor, 7},  {kMelodicMinor, 7},
    {kPentatonicMajor, 5}, {kPentatonicMinor, 5}, {kBlues, 6},
};

enum Params : uint32_t {
  kRoot = 0,
  kScale = 1,
  kSplit = 2,
  kOctave = 3,
  kInversion = 4,
  kVoices = 5,
  kSpread = 6,
  kPassthrough = 7,
  kChannel = 8,
};

constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kNoteOff = 0x80;

int clamp_int(double value, int low, int high) {
  return std::clamp(static_cast<int>(std::lround(value)), low, high);
}

const ScaleDef& scale_def(int scale) {
  return kScales[std::clamp(scale, 0, static_cast<int>(ChordInstance::ScaleCount) - 1)];
}

// The scale degree (0-based index into `def.intervals`) whose pitch class
// matches `note`, or -1 when the note is not a tone of this scale at all -
// a black key the current scale does not have stays silent, on purpose.
int degree_for_note(int root, const ScaleDef& def, int note) {
  const int rel = ((note - root) % 12 + 12) % 12;
  for (int i = 0; i < def.count; ++i)
    if (def.intervals[i] == rel) return i;
  return -1;
}

// The nearest note (by absolute distance, ties toward higher) that is a tone
// of this scale. Used for the passthrough zone, not the trigger one - a
// melody note gets pulled to the scale, it does not vanish for missing it.
int quantize_to_scale(int note, int root, const ScaleDef& def) {
  for (int delta = 0; delta <= 6; ++delta) {
    const int up = note + delta;
    if (up <= 127) {
      const int rel = ((up - root) % 12 + 12) % 12;
      for (int i = 0; i < def.count; ++i)
        if (def.intervals[i] == rel) return up;
    }
    if (delta == 0) continue;
    const int down = note - delta;
    if (down >= 0) {
      const int rel = ((down - root) % 12 + 12) % 12;
      for (int i = 0; i < def.count; ++i)
        if (def.intervals[i] == rel) return down;
    }
  }
  return note;  // every scale on offer has at least one tone within a fifth
}

// Voice `k` (0 root, 1 third, 2 fifth, 3 seventh) of the chord built on
// `degree` by stacking thirds inside the scale - the same tertian-harmony
// arithmetic a theory class does by hand, just indexed instead of named.
// `anchor` is the MIDI note that stands for the root's own pitch class at
// octave zero, so the whole chord lands in the performance register
// regardless of what octave the trigger key was actually struck in.
int voice_pitch(const ScaleDef& def, int root, int anchor, int degree, int k) {
  const int idx = degree + 2 * k;
  const int wrap = idx / def.count;
  const int i = idx % def.count;
  return std::clamp(anchor + root + def.intervals[i] + 12 * wrap, 0, 127);
}

// Stacks the chord, then applies inversion (push the bottom `inversion`
// voices, in stacking order, up an octave) and spread (open lifts the
// second-lowest voice an octave, the classic un-clustering move).
void build_chord(const ScaleDef& def, int root, int anchor, int degree,
                  int voice_count, int inversion, int spread,
                  std::array<int, ChordInstance::kMaxVoices>& out) {
  for (int k = 0; k < voice_count; ++k) out[k] = voice_pitch(def, root, anchor, degree, k);

  const int inv = std::clamp(inversion, 0, voice_count - 1);
  for (int k = 0; k < inv; ++k) out[k] += 12;

  std::sort(out.begin(), out.begin() + voice_count);
  if (spread != 0 && voice_count >= 3) out[1] += 12;
  std::sort(out.begin(), out.begin() + voice_count);

  for (int k = 0; k < voice_count; ++k) out[k] = std::clamp(out[k], 0, 127);
}

}  // namespace

PluginDescriptor ChordInstance::make_descriptor() {
  PluginDescriptor descriptor;
  descriptor.format = PluginFormat::Internal;
  descriptor.uid = "nirbija.chord";
  descriptor.name = "Chord";
  descriptor.vendor = "Nirbija";
  descriptor.audio_inputs = 0;
  descriptor.audio_outputs = 0;
  descriptor.has_midi_input = true;
  descriptor.category = "Chord";
  descriptor.kind = PluginKind::MidiEffect;
  return descriptor;
}

ChordInstance::ChordInstance() : descriptor_(make_descriptor()) {
  passthrough_pitch_.fill(-1);
}

bool ChordInstance::activate(double, uint32_t) {
  deactivate();
  return true;
}

void ChordInstance::deactivate() {
  origin_.fill(Origin::None);
  trigger_voice_count_.fill(0);
  passthrough_pitch_.fill(-1);
  event_count_ = 0;
}

void ChordInstance::emit(uint32_t frame, uint8_t status, uint8_t data1,
                         uint8_t data2) {
  if (event_count_ >= kMaxEvents) return;
  MidiEvent& event = events_[event_count_++];
  event.frame = frame;
  event.size = 3;
  event.data[0] = status;
  event.data[1] = data1;
  event.data[2] = data2;
}

void ChordInstance::handle_trigger_on(uint32_t frame, uint8_t note,
                                      uint8_t velocity) {
  // A retrigger without a note-off in between - some controllers do this on
  // legato - must let go of the old voices first, or they never do.
  if (trigger_voice_count_[note] != 0) handle_trigger_off(frame, note);

  const int root = clamp_int(root_.load(std::memory_order_relaxed), 0, 11);
  const ScaleDef& def = scale_def(scale_.load(std::memory_order_relaxed));
  const int degree = degree_for_note(root, def, note);
  if (degree < 0) return;  // not a scale tone: silence, not the nearest guess

  const int anchor = 60 + 12 * clamp_int(octave_.load(std::memory_order_relaxed), -3, 3);
  const int voice_count =
      clamp_int(voices_.load(std::memory_order_relaxed), 3, kMaxVoices);
  const int inversion = clamp_int(inversion_.load(std::memory_order_relaxed), 0, 3);
  const int spread = spread_.load(std::memory_order_relaxed);
  const int channel = clamp_int(channel_.load(std::memory_order_relaxed), 0, 15);

  std::array<int, kMaxVoices> chord{};
  build_chord(def, root, anchor, degree, voice_count, inversion, spread, chord);

  for (int k = 0; k < voice_count; ++k) {
    trigger_voices_[note][k] = static_cast<uint8_t>(chord[k]);
    emit(frame, static_cast<uint8_t>(kNoteOn | channel),
         static_cast<uint8_t>(chord[k]), velocity);
  }
  trigger_voice_count_[note] = static_cast<uint8_t>(voice_count);
}

void ChordInstance::handle_trigger_off(uint32_t frame, uint8_t note) {
  const uint8_t count = trigger_voice_count_[note];
  if (count == 0) return;
  const int channel = clamp_int(channel_.load(std::memory_order_relaxed), 0, 15);
  for (uint8_t k = 0; k < count; ++k)
    emit(frame, static_cast<uint8_t>(kNoteOff | channel), trigger_voices_[note][k], 0);
  trigger_voice_count_[note] = 0;
}

void ChordInstance::handle_passthrough_on(uint32_t frame, uint8_t note,
                                          uint8_t velocity) {
  if (passthrough_pitch_[note] >= 0) handle_passthrough_off(frame, note);

  int out_note = note;
  if (passthrough_.load(std::memory_order_relaxed) != 0) {
    const int root = clamp_int(root_.load(std::memory_order_relaxed), 0, 11);
    const ScaleDef& def = scale_def(scale_.load(std::memory_order_relaxed));
    out_note = quantize_to_scale(note, root, def);
  }
  const int channel = clamp_int(channel_.load(std::memory_order_relaxed), 0, 15);
  passthrough_pitch_[note] = static_cast<int16_t>(out_note);
  emit(frame, static_cast<uint8_t>(kNoteOn | channel), static_cast<uint8_t>(out_note),
       velocity);
}

void ChordInstance::handle_passthrough_off(uint32_t frame, uint8_t note) {
  const int16_t out_note = passthrough_pitch_[note];
  if (out_note < 0) return;
  const int channel = clamp_int(channel_.load(std::memory_order_relaxed), 0, 15);
  emit(frame, static_cast<uint8_t>(kNoteOff | channel), static_cast<uint8_t>(out_note), 0);
  passthrough_pitch_[note] = -1;
}

void ChordInstance::queue_midi(const MidiEvent& event) {
  if (event.size < 2) return;
  const uint8_t status = event.data[0] & 0xf0;
  const uint8_t note = event.data[1];
  if (note >= 128) return;
  const uint8_t velocity = event.size >= 3 ? event.data[2] : 0;

  const bool is_on = status == kNoteOn && velocity > 0;
  const bool is_off = status == kNoteOff || (status == kNoteOn && velocity == 0);
  if (!is_on && !is_off) return;

  if (is_on) {
    const int split = clamp_int(split_.load(std::memory_order_relaxed), 0, 127);
    if (note < split) {
      origin_[note] = Origin::Trigger;
      handle_trigger_on(event.frame, note, velocity);
    } else {
      origin_[note] = Origin::Passthrough;
      handle_passthrough_on(event.frame, note, velocity);
    }
    return;
  }

  // Note-off is routed by where this key's note-on landed, not by the split
  // as it stands now - moving the split with a key still down must not
  // strand a chord or a passthrough note ringing forever.
  switch (origin_[note]) {
    case Origin::Trigger: handle_trigger_off(event.frame, note); break;
    case Origin::Passthrough: handle_passthrough_off(event.frame, note); break;
    case Origin::None: break;
  }
  origin_[note] = Origin::None;
}

void ChordInstance::process(const float* const*, float* const*, uint32_t) {
  // Purely reactive: everything this plugin emits was already decided in
  // queue_midi, which the host calls before process() for the whole block
  // (channel_strip.cpp). No clock, no scheduling, nothing to do here.
}

size_t ChordInstance::take_midi_output(MidiEvent* out, size_t capacity) {
  std::sort(events_.begin(), events_.begin() + event_count_,
            [](const MidiEvent& a, const MidiEvent& b) {
              if (a.frame != b.frame) return a.frame < b.frame;
              return (a.data[0] & 0xf0) < (b.data[0] & 0xf0);  // off before on
            });
  const size_t count = std::min(event_count_, capacity);
  std::copy_n(events_.begin(), count, out);
  event_count_ = 0;
  return count;
}

std::vector<ParameterInfo> ChordInstance::parameters() const {
  return {
      {kRoot, "Root (0 C .. 11 B)", 0.0, 11.0, 0.0},
      {kScale,
       "Scale (0 major, 1 dorian, 2 phrygian, 3 lydian, 4 mixolydian, "
       "5 minor, 6 locrian, 7 harm.minor, 8 mel.minor, 9 penta maj, "
       "10 penta min, 11 blues)",
       0.0, static_cast<double>(ScaleCount) - 1.0, 0.0},
      {kSplit, "Split point (MIDI note)", 0.0, 127.0, 60.0},
      {kOctave, "Performance octave", -3.0, 3.0, 0.0},
      {kInversion, "Inversion (0 root .. 3)", 0.0, 3.0, 0.0},
      {kVoices, "Voices (3 triad, 4 seventh)", 3.0, 4.0, 3.0},
      {kSpread, "Spread (0 closed, 1 open)", 0.0, 1.0, 0.0},
      {kPassthrough, "Passthrough quantize", 0.0, 1.0, 0.0},
      {kChannel, "MIDI channel", 0.0, 15.0, 0.0},
  };
}

double ChordInstance::parameter_value(uint32_t id) const {
  switch (id) {
    case kRoot: return root_.load(std::memory_order_relaxed);
    case kScale: return scale_.load(std::memory_order_relaxed);
    case kSplit: return split_.load(std::memory_order_relaxed);
    case kOctave: return octave_.load(std::memory_order_relaxed);
    case kInversion: return inversion_.load(std::memory_order_relaxed);
    case kVoices: return voices_.load(std::memory_order_relaxed);
    case kSpread: return spread_.load(std::memory_order_relaxed);
    case kPassthrough: return passthrough_.load(std::memory_order_relaxed);
    case kChannel: return channel_.load(std::memory_order_relaxed);
    default: return 0.0;
  }
}

void ChordInstance::set_parameter(uint32_t id, double value) {
  switch (id) {
    case kRoot:
      root_.store(clamp_int(value, 0, 11), std::memory_order_relaxed);
      break;
    case kScale:
      scale_.store(clamp_int(value, 0, ScaleCount - 1), std::memory_order_relaxed);
      break;
    case kSplit:
      split_.store(clamp_int(value, 0, 127), std::memory_order_relaxed);
      break;
    case kOctave:
      octave_.store(clamp_int(value, -3, 3), std::memory_order_relaxed);
      break;
    case kInversion:
      inversion_.store(clamp_int(value, 0, 3), std::memory_order_relaxed);
      break;
    case kVoices:
      voices_.store(clamp_int(value, 3, kMaxVoices), std::memory_order_relaxed);
      break;
    case kSpread:
      spread_.store(value >= 0.5 ? 1 : 0, std::memory_order_relaxed);
      break;
    case kPassthrough:
      passthrough_.store(value >= 0.5 ? 1 : 0, std::memory_order_relaxed);
      break;
    case kChannel:
      channel_.store(clamp_int(value, 0, 15), std::memory_order_relaxed);
      break;
    default:
      break;
  }
}

std::vector<uint8_t> ChordInstance::save_state() const {
  char text[256];
  const int written = std::snprintf(
      text, sizeof(text),
      "root %d\nscale %d\nsplit %d\noctave %d\ninversion %d\nvoices %d\n"
      "spread %d\npassthrough %d\nchannel %d\n",
      root_.load(std::memory_order_relaxed), scale_.load(std::memory_order_relaxed),
      split_.load(std::memory_order_relaxed), octave_.load(std::memory_order_relaxed),
      inversion_.load(std::memory_order_relaxed), voices_.load(std::memory_order_relaxed),
      spread_.load(std::memory_order_relaxed),
      passthrough_.load(std::memory_order_relaxed),
      channel_.load(std::memory_order_relaxed));
  if (written <= 0) return {};
  return {text, text + written};
}

bool ChordInstance::load_state(const std::vector<uint8_t>& blob) {
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

    if (key == "root") set_parameter(kRoot, value);
    else if (key == "scale") set_parameter(kScale, value);
    else if (key == "split") set_parameter(kSplit, value);
    else if (key == "octave") set_parameter(kOctave, value);
    else if (key == "inversion") set_parameter(kInversion, value);
    else if (key == "voices") set_parameter(kVoices, value);
    else if (key == "spread") set_parameter(kSpread, value);
    else if (key == "passthrough") set_parameter(kPassthrough, value);
    else if (key == "channel") set_parameter(kChannel, value);
  }
  return true;
}

}  // namespace nirbija
