// The computer keyboard has no transport and no clock: a key down is a
// note-on the moment process() next runs, a key up its note-off. What is
// worth checking at a desk is the queue crossing threads correctly, the
// dedupe that keeps OS key-repeat from flooding note-ons, and that a plugin
// ahead of this one in the chain still gets heard.

#include <cstdio>
#include <string>
#include <vector>

#include "core/keyboard_instrument.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

void expect(bool condition, const std::string& what) {
  if (!condition) fail(what);
}

struct Note {
  bool on;
  int pitch;
  int velocity;
};

std::vector<Note> drain(nirbija::KeyboardInstrumentInstance& kb) {
  nirbija::TransportInfo transport;
  kb.set_transport(transport);
  kb.process(nullptr, nullptr, 256);

  nirbija::MidiEvent buffer[64];
  const size_t count = kb.take_midi_output(buffer, 64);
  std::vector<Note> out;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t status = buffer[i].data[0] & 0xf0;
    if (status != 0x90 && status != 0x80) continue;
    out.push_back({status == 0x90 && buffer[i].data[2] > 0, buffer[i].data[1],
                   buffer[i].data[2]});
  }
  return out;
}

}  // namespace

int main() {
  // --- a key down is a note-on, velocity and all ------------------------------
  {
    nirbija::KeyboardInstrumentInstance kb;
    kb.activate(48000.0, 256);
    kb.key_down(60, 90);
    const std::vector<Note> notes = drain(kb);
    expect(notes.size() == 1, "one key down should be one MIDI message, got " +
                                   std::to_string(notes.size()));
    if (!notes.empty()) {
      expect(notes[0].on, "a key down produced a note-off");
      expect(notes[0].pitch == 60, "the wrong pitch came out");
      expect(notes[0].velocity == 90, "velocity was not carried over");
    }
  }

  // --- releasing it turns into a note-off --------------------------------------
  {
    nirbija::KeyboardInstrumentInstance kb;
    kb.activate(48000.0, 256);
    kb.key_down(60, 90);
    drain(kb);
    kb.key_up(60);
    const std::vector<Note> notes = drain(kb);
    expect(notes.size() == 1 && !notes[0].on,
           "releasing a held key did not produce a note-off");
  }

  // --- OS key-repeat does not retrigger the same note --------------------------
  {
    nirbija::KeyboardInstrumentInstance kb;
    kb.activate(48000.0, 256);
    kb.key_down(60, 90);
    kb.key_down(60, 90);
    kb.key_down(60, 90);
    const std::vector<Note> notes = drain(kb);
    int ons = 0;
    for (const Note& note : notes)
      if (note.on) ++ons;
    expect(ons == 1, "a key held down fired " + std::to_string(ons) +
                          " note-ons instead of one");
  }

  // --- a release with nothing held is silently ignored -------------------------
  {
    nirbija::KeyboardInstrumentInstance kb;
    kb.activate(48000.0, 256);
    kb.key_up(60);  // never pressed
    const std::vector<Note> notes = drain(kb);
    expect(notes.empty(), "a stray key-up produced a note-off from nowhere");
  }

  // --- a chord is polyphonic ----------------------------------------------------
  {
    nirbija::KeyboardInstrumentInstance kb;
    kb.activate(48000.0, 256);
    kb.key_down(60, 100);
    kb.key_down(64, 100);
    kb.key_down(67, 100);
    const std::vector<Note> notes = drain(kb);
    int ons = 0;
    for (const Note& note : notes)
      if (note.on) ++ons;
    expect(ons == 3, "a three-note chord produced " + std::to_string(ons) +
                          " note-ons, wanted 3");
  }

  // --- notes from earlier in the chain still pass through ----------------------
  {
    nirbija::KeyboardInstrumentInstance kb;
    kb.activate(48000.0, 256);
    nirbija::MidiEvent incoming{};
    incoming.frame = 12;
    incoming.size = 3;
    incoming.data[0] = 0x90;
    incoming.data[1] = 72;
    incoming.data[2] = 88;
    kb.queue_midi(incoming);

    nirbija::TransportInfo transport;
    kb.set_transport(transport);
    kb.process(nullptr, nullptr, 256);
    nirbija::MidiEvent buffer[64];
    const size_t count = kb.take_midi_output(buffer, 64);
    bool passed = false;
    for (size_t i = 0; i < count; ++i)
      if (buffer[i].data[1] == 72 && buffer[i].frame == 12) passed = true;
    expect(passed, "a note from earlier in the chain did not pass through");
  }

  // --- the channel parameter reaches the wire -----------------------------------
  {
    nirbija::KeyboardInstrumentInstance kb;
    kb.activate(48000.0, 256);
    kb.set_parameter(0, 10.0);  // channel 10
    kb.key_down(60, 100);
    const std::vector<Note> notes = drain(kb);
    // Re-fetch the raw status byte to check the channel nibble directly.
    nirbija::KeyboardInstrumentInstance kb2;
    kb2.activate(48000.0, 256);
    kb2.set_parameter(0, 10.0);
    kb2.key_down(60, 100);
    nirbija::TransportInfo transport;
    kb2.set_transport(transport);
    kb2.process(nullptr, nullptr, 256);
    nirbija::MidiEvent buffer[64];
    const size_t count = kb2.take_midi_output(buffer, 64);
    bool onRightChannel = false;
    for (size_t i = 0; i < count; ++i)
      if ((buffer[i].data[0] & 0x0f) == 9) onRightChannel = true;  // channel 10 = 0-based 9
    expect(onRightChannel, "the MIDI channel parameter did not reach the wire");
    if (kb.parameter_value(0) != 10.0) fail("channel did not read back as 10");
  }

  // --- state survives a round trip ------------------------------------------
  {
    nirbija::KeyboardInstrumentInstance kb;
    kb.activate(48000.0, 256);
    kb.set_parameter(0, 5.0);
    const std::vector<uint8_t> blob = kb.save_state();

    nirbija::KeyboardInstrumentInstance restored;
    restored.activate(48000.0, 256);
    if (!restored.load_state(blob)) fail("load_state refused its own blob");
    if (restored.parameter_value(0) != 5.0)
      fail("channel did not survive the state round trip");
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
