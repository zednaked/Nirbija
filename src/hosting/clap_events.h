// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

// MIDI from the strip into the event a CLAP plugin asked for. Kept apart from
// the backend so it can be checked without a plugin: the translation depends
// only on what the note port declared, and a plugin that speaks CLAP notes
// only never sees a MIDI event.

#include "core/plugin.h"
#include "hosting/common.h"

#include <clap/clap.h>

#include <cstring>

namespace nirbija::hosting {

// What the plugin's first note input said it takes, boiled down to two
// decisions: do notes travel as clap_event_note, and may anything travel as
// raw MIDI at all.
struct ClapNoteDialect {
  bool clap_notes = false;  // note on/off as CLAP_EVENT_NOTE_ON/OFF
  bool midi = true;         // everything else as CLAP_EVENT_MIDI

  // From a clap_note_port_info: the CLAP dialect is used for notes when it is
  // preferred, or when MIDI is not on offer at all; MIDI carries the rest only
  // when supported. With no note port there is nothing to read and the old
  // behaviour - raw MIDI - stands, since it is what a plugin with no declared
  // preference has always been fed.
  static ClapNoteDialect from_port(uint32_t supported, uint32_t preferred) {
    ClapNoteDialect out;
    out.midi = (supported & CLAP_NOTE_DIALECT_MIDI) != 0;
    out.clap_notes = (supported & CLAP_NOTE_DIALECT_CLAP) != 0 &&
                     (!out.midi || preferred == CLAP_NOTE_DIALECT_CLAP);
    // A port that supports neither (MPE only, or a bogus declaration) still
    // gets MIDI: silence is the one thing certain to be wrong.
    if (!out.midi && !out.clap_notes) out.midi = true;
    return out;
  }
};

// Parameter changes, MIDI and notes travel in one list, so the storage has to
// hold any of them. All start with a clap_event_header_t.
union ClapInEvent {
  ClapInEvent() : header{} {}
  explicit ClapInEvent(const clap_event_param_value_t& value) : param(value) {}
  explicit ClapInEvent(const clap_event_midi_t& value) : midi(value) {}
  explicit ClapInEvent(const clap_event_note_t& value) : note(value) {}

  clap_event_header_t header;
  clap_event_param_value_t param;
  clap_event_midi_t midi;
  clap_event_note_t note;
};

// Fills `out` with the event for `source`, or returns false when the plugin
// takes nothing that could carry it (a CC into a CLAP-notes-only synth).
inline bool clap_event_from_midi(const MidiEvent& source, ClapNoteDialect dialect,
                                 ClapInEvent* out) {
  const MidiMessage message = decode_midi(source);
  const bool is_note = message.kind == MidiMessage::Kind::NoteOn ||
                       message.kind == MidiMessage::Kind::NoteOff;

  if (is_note && dialect.clap_notes) {
    clap_event_note_t event{};
    event.header.size = sizeof(event);
    event.header.time = source.frame;
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = message.kind == MidiMessage::Kind::NoteOn
                            ? CLAP_EVENT_NOTE_ON
                            : CLAP_EVENT_NOTE_OFF;
    event.header.flags = 0;
    // -1: the host does not track note ids, so the plugin matches on key and
    // channel, exactly as it would for MIDI.
    event.note_id = -1;
    event.port_index = 0;
    event.channel = message.channel;
    event.key = message.key;
    event.velocity = message.value;
    *out = ClapInEvent{event};
    return true;
  }

  if (!dialect.midi) return false;

  clap_event_midi_t event{};
  event.header.size = sizeof(event);
  event.header.time = source.frame;
  event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  event.header.type = CLAP_EVENT_MIDI;
  event.header.flags = 0;
  event.port_index = 0;
  std::memcpy(event.data, source.data, sizeof(event.data));
  *out = ClapInEvent{event};
  return true;
}

}  // namespace nirbija::hosting
