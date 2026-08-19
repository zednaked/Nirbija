#include "core/keyboard_instrument.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

namespace nirbija {
namespace {

enum Params : uint32_t {
  kChannel = 0,
};

constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kNoteOff = 0x80;

int clamp_int(double value, int low, int high) {
  const int rounded = static_cast<int>(value + (value >= 0.0 ? 0.5 : -0.5));
  return std::clamp(rounded, low, high);
}

}  // namespace

PluginDescriptor KeyboardInstrumentInstance::make_descriptor() {
  PluginDescriptor descriptor;
  descriptor.format = PluginFormat::Internal;
  descriptor.uid = "nirbija.keyboard";
  descriptor.name = "Computer Keyboard";
  descriptor.vendor = "Nirbija";
  // Same shape as the step sequencer: nothing but notes, so the picker sorts
  // it as MIDI without being told to.
  descriptor.audio_inputs = 0;
  descriptor.audio_outputs = 0;
  descriptor.has_midi_input = true;
  descriptor.category = "Keyboard";
  descriptor.kind = PluginKind::MidiEffect;
  return descriptor;
}

KeyboardInstrumentInstance::KeyboardInstrumentInstance()
    : descriptor_(make_descriptor()) {}

bool KeyboardInstrumentInstance::activate(double sample_rate, uint32_t) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  held_.fill(false);
  event_count_ = 0;
  // Drop anything a previous activation left queued rather than let it fire
  // the moment this one starts processing.
  KeyEvent discard;
  while (incoming_.pop(discard)) {
  }
  return true;
}

void KeyboardInstrumentInstance::deactivate() {
  held_.fill(false);
  event_count_ = 0;
}

void KeyboardInstrumentInstance::key_down(int note, int velocity) {
  KeyEvent event;
  event.note = static_cast<uint8_t>(std::clamp(note, 0, 127));
  event.velocity = static_cast<uint8_t>(std::clamp(velocity, 1, 127));
  event.down = true;
  incoming_.push(event);
}

void KeyboardInstrumentInstance::key_up(int note) {
  KeyEvent event;
  event.note = static_cast<uint8_t>(std::clamp(note, 0, 127));
  event.down = false;
  incoming_.push(event);
}

void KeyboardInstrumentInstance::emit_event(uint32_t frame, uint8_t status,
                                            uint8_t data1, uint8_t data2) {
  if (event_count_ >= kMaxEvents) return;
  MidiEvent& event = events_[event_count_++];
  event.frame = frame;
  event.size = 3;
  event.data[0] = status;
  event.data[1] = data1;
  event.data[2] = data2;
}

void KeyboardInstrumentInstance::queue_midi(const MidiEvent& event) {
  // Passed along untouched, so a step sequencer or another instrument can sit
  // ahead of this in the same strip and still be heard.
  if (event_count_ >= kMaxEvents) return;
  events_[event_count_++] = event;
}

void KeyboardInstrumentInstance::process(const float* const*, float* const*,
                                         uint32_t) {
  const uint8_t channel =
      static_cast<uint8_t>(std::clamp(channel_.load(std::memory_order_relaxed), 0, 15));

  // Every frame in the block is close enough to the same instant a physical
  // key press already was by the time it got off the UI thread - there is no
  // finer timestamp worth chasing here.
  KeyEvent key;
  while (incoming_.pop(key)) {
    if (key.down) {
      if (held_[key.note]) continue;  // OS key-repeat, or a duplicate press
      held_[key.note] = true;
      emit_event(0, static_cast<uint8_t>(kNoteOn | channel), key.note,
                key.velocity);
    } else {
      if (!held_[key.note]) continue;
      held_[key.note] = false;
      emit_event(0, static_cast<uint8_t>(kNoteOff | channel), key.note, 0);
    }
  }
}

size_t KeyboardInstrumentInstance::take_midi_output(MidiEvent* out,
                                                    size_t capacity) {
  std::sort(events_.begin(), events_.begin() + event_count_,
            [](const MidiEvent& a, const MidiEvent& b) {
              return a.frame < b.frame;
            });
  const size_t count = std::min(event_count_, capacity);
  std::copy_n(events_.begin(), count, out);
  event_count_ = 0;
  return count;
}

std::vector<ParameterInfo> KeyboardInstrumentInstance::parameters() const {
  return {{kChannel, "MIDI channel", 1.0, 16.0, 1.0}};
}

double KeyboardInstrumentInstance::parameter_value(uint32_t id) const {
  if (id == kChannel) return channel_.load(std::memory_order_relaxed) + 1;
  return 0.0;
}

void KeyboardInstrumentInstance::set_parameter(uint32_t id, double value) {
  if (id == kChannel)
    channel_.store(clamp_int(value, 1, 16) - 1, std::memory_order_relaxed);
}

std::vector<uint8_t> KeyboardInstrumentInstance::save_state() const {
  char line[32];
  std::snprintf(line, sizeof(line), "channel %d\n",
               channel_.load(std::memory_order_relaxed));
  const std::string text(line);
  return {text.begin(), text.end()};
}

bool KeyboardInstrumentInstance::load_state(const std::vector<uint8_t>& blob) {
  const std::string text(blob.begin(), blob.end());
  size_t position = 0;
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
    const std::string_view rest = line.substr(space + 1);

    double value = 0.0;
    // The stored value is the 0-based channel_ field itself; set_parameter
    // expects the 1-based number the user sees.
    if (key == "channel" && parse_number(rest, &value))
      set_parameter(kChannel, value + 1);
  }
  return true;
}

}  // namespace nirbija
