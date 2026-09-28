// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// MIDI from the strip into the event a CLAP note port asked for. A plugin that
// declares only the CLAP note dialect must get clap_event_note, never raw
// MIDI, or it stays silent; one that declares only MIDI must get MIDI. No
// plugin is loaded: the translation depends on nothing but the declaration.

#include <cmath>
#include <cstdio>
#include <string>

#include "hosting/clap_events.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

nirbija::MidiEvent midi(uint8_t status, uint8_t a, uint8_t b, uint32_t frame = 7) {
  nirbija::MidiEvent event;
  event.frame = frame;
  event.size = 3;
  event.data[0] = status;
  event.data[1] = a;
  event.data[2] = b;
  return event;
}

}  // namespace

int main() {
  using nirbija::hosting::ClapInEvent;
  using nirbija::hosting::ClapNoteDialect;
  using nirbija::hosting::clap_event_from_midi;

  // A port that only takes CLAP notes: what a fake plugin with
  // supported_dialects = CLAP_NOTE_DIALECT_CLAP would declare.
  {
    const ClapNoteDialect dialect =
        ClapNoteDialect::from_port(CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
    if (!dialect.clap_notes || dialect.midi)
      fail("CLAP-only port did not resolve to clap notes without MIDI");

    ClapInEvent event;
    if (!clap_event_from_midi(midi(0x92, 60, 100), dialect, &event)) {
      fail("note on into a CLAP-only port produced nothing");
    } else {
      if (event.header.type != CLAP_EVENT_NOTE_ON) fail("note on is not CLAP_EVENT_NOTE_ON");
      if (event.header.size != sizeof(clap_event_note_t)) fail("note event size is wrong");
      if (event.header.space_id != CLAP_CORE_EVENT_SPACE_ID) fail("note event space is wrong");
      if (event.header.time != 7) fail("note event lost its frame");
      if (event.note.key != 60) fail("note event key is wrong");
      if (event.note.channel != 2) fail("note event channel is wrong");
      if (event.note.note_id != -1) fail("note event id must be -1 (untracked)");
      if (event.note.port_index != 0) fail("note event port must be 0");
      if (std::abs(event.note.velocity - 100.0 / 127.0) > 1e-9)
        fail("note event velocity is not velocity/127");
    }

    if (!clap_event_from_midi(midi(0x80, 60, 0), dialect, &event) ||
        event.header.type != CLAP_EVENT_NOTE_OFF)
      fail("note off (0x80) is not CLAP_EVENT_NOTE_OFF");
    if (!clap_event_from_midi(midi(0x90, 60, 0), dialect, &event) ||
        event.header.type != CLAP_EVENT_NOTE_OFF)
      fail("note on at velocity 0 is not CLAP_EVENT_NOTE_OFF");

    // A CC has no CLAP note equivalent and the port takes no MIDI: dropped,
    // never sent as something the plugin did not ask for.
    if (clap_event_from_midi(midi(0xb0, 64, 127), dialect, &event))
      fail("a CC reached a CLAP-only port as an event");
  }

  // MIDI only: everything as raw bytes.
  {
    const ClapNoteDialect dialect =
        ClapNoteDialect::from_port(CLAP_NOTE_DIALECT_MIDI, CLAP_NOTE_DIALECT_MIDI);
    if (dialect.clap_notes || !dialect.midi)
      fail("MIDI-only port did not resolve to MIDI without clap notes");
    ClapInEvent event;
    if (!clap_event_from_midi(midi(0x91, 61, 99), dialect, &event) ||
        event.header.type != CLAP_EVENT_MIDI)
      fail("note into a MIDI-only port is not CLAP_EVENT_MIDI");
    else if (event.midi.data[0] != 0x91 || event.midi.data[1] != 61 ||
             event.midi.data[2] != 99)
      fail("MIDI event bytes were not copied verbatim");
    if (!clap_event_from_midi(midi(0xb0, 64, 127), dialect, &event) ||
        event.header.type != CLAP_EVENT_MIDI)
      fail("CC into a MIDI-only port is not CLAP_EVENT_MIDI");
  }

  // Both, preferring CLAP (the Drone's declaration): notes native, CC as MIDI.
  {
    const ClapNoteDialect dialect = ClapNoteDialect::from_port(
        CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI, CLAP_NOTE_DIALECT_CLAP);
    ClapInEvent event;
    if (!clap_event_from_midi(midi(0x90, 60, 100), dialect, &event) ||
        event.header.type != CLAP_EVENT_NOTE_ON)
      fail("note into a both/prefers-CLAP port is not CLAP_EVENT_NOTE_ON");
    if (!clap_event_from_midi(midi(0xe0, 0, 64), dialect, &event) ||
        event.header.type != CLAP_EVENT_MIDI)
      fail("pitch bend into a both/prefers-CLAP port is not CLAP_EVENT_MIDI");
  }

  // Both, preferring MIDI: the old behaviour, everything as MIDI.
  {
    const ClapNoteDialect dialect = ClapNoteDialect::from_port(
        CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI, CLAP_NOTE_DIALECT_MIDI);
    ClapInEvent event;
    if (!clap_event_from_midi(midi(0x90, 60, 100), dialect, &event) ||
        event.header.type != CLAP_EVENT_MIDI)
      fail("note into a both/prefers-MIDI port is not CLAP_EVENT_MIDI");
  }

  // Nothing declared: MIDI, as it always was.
  {
    const ClapNoteDialect dialect = ClapNoteDialect::from_port(0, 0);
    if (!dialect.midi || dialect.clap_notes)
      fail("an undeclared port did not fall back to MIDI");
    const ClapNoteDialect defaulted;
    if (!defaulted.midi || defaulted.clap_notes)
      fail("the default dialect (no note port) is not MIDI");
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok: CLAP note dialect translation\n");
  return 0;
}
