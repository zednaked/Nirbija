// Drives the scriptable MIDI plugin: a script goes in, notes go through, and
// what comes out is what the script's tables say. No JACK, no display.
//
// The check that matters most is the last one, and it is about what does *not*
// happen: the audio path must not run Lua. A script that loops forever has to
// be caught while it is being built, on the thread that is allowed to wait.

#include <cstdio>
#include <string>
#include <vector>

#include "core/script_plugin.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

void expect(bool condition, const std::string& what) {
  if (!condition) fail(what);
}

nirbija::MidiEvent note(int pitch, int velocity = 100, int channel = 0,
                        bool on = true) {
  nirbija::MidiEvent event{};
  event.size = 3;
  event.data[0] = static_cast<uint8_t>((on ? 0x90 : 0x80) | channel);
  event.data[1] = static_cast<uint8_t>(pitch);
  event.data[2] = static_cast<uint8_t>(velocity);
  return event;
}

// Pushes events through one block and returns what came out.
std::vector<nirbija::MidiEvent> through(nirbija::ScriptInstance& script,
                                        const std::vector<nirbija::MidiEvent>& in) {
  for (const nirbija::MidiEvent& event : in) script.queue_midi(event);
  script.process(nullptr, nullptr, 256);

  nirbija::MidiEvent buffer[64];
  const size_t count = script.take_midi_output(buffer, 64);
  return {buffer, buffer + count};
}

}  // namespace

int main() {
  // --- the default script compiles and passes notes ---------------------------
  {
    nirbija::ScriptInstance script;
    script.activate(48000.0, 256);
    expect(script.error().empty(),
           "the script every instance starts with does not run: " + script.error());

    const std::vector<nirbija::MidiEvent> out = through(script, {note(60)});
    expect(out.size() == 1, "one note in gave " + std::to_string(out.size()) + " out");
  }

  // --- a note map is obeyed ---------------------------------------------------
  {
    nirbija::ScriptInstance script;
    script.activate(48000.0, 256);
    expect(script.set_script(R"(
      function build(knob)
        local m = {}
        for n = 0, 127 do m[n] = math.min(127, n + 7) end
        return { note_map = m }
      end)"), "a valid script was refused: " + script.error());

    const std::vector<nirbija::MidiEvent> out = through(script, {note(60)});
    if (out.size() != 1) fail("the mapped note did not come out");
    else expect(out[0].data[1] == 67, "note 60 came out as " +
                                          std::to_string(out[0].data[1]) +
                                          ", wanted 67");
  }

  // --- -1 drops a note, and the drop is total ---------------------------------
  {
    nirbija::ScriptInstance script;
    script.activate(48000.0, 256);
    script.set_script(R"(
      function build(knob)
        local m = {}
        for n = 0, 127 do m[n] = n end
        m[61] = -1
        return { note_map = m }
      end)");

    const std::vector<nirbija::MidiEvent> out =
        through(script, {note(60), note(61), note(62)});
    expect(out.size() == 2, "dropping one of three notes left " +
                                std::to_string(out.size()));
    for (const nirbija::MidiEvent& event : out)
      expect(event.data[1] != 61, "a note the script dropped came out anyway");
  }

  // --- velocity and channel maps ---------------------------------------------
  {
    nirbija::ScriptInstance script;
    script.activate(48000.0, 256);
    script.set_script(R"(
      function build(knob)
        local v, c = {}, {}
        for n = 0, 127 do v[n] = 64 end
        for n = 0, 15 do c[n] = 9 end
        return { velocity_map = v, channel_map = c }
      end)");

    const std::vector<nirbija::MidiEvent> out = through(script, {note(60, 120, 0)});
    if (out.size() != 1) fail("the note did not come out");
    else {
      expect(out[0].data[2] == 64, "velocity came out " +
                                       std::to_string(out[0].data[2]) +
                                       ", wanted 64");
      expect((out[0].data[0] & 0x0f) == 9, "the channel map was not applied");
    }
  }

  // --- a note-off is never turned into silence --------------------------------
  //
  // A velocity map that sends everything to zero would, applied blindly, make
  // note-offs vanish - and a synth would hang on notes nobody can release.
  {
    nirbija::ScriptInstance script;
    script.activate(48000.0, 256);
    script.set_script(R"(
      function build(knob)
        local v = {}
        for n = 0, 127 do v[n] = 0 end
        return { velocity_map = v }
      end)");

    const std::vector<nirbija::MidiEvent> out =
        through(script, {note(60, 100, 0, false)});
    expect(out.size() == 1, "a note-off was swallowed by a velocity map");
  }

  // --- what a script does not mention, it does not change ---------------------
  {
    nirbija::ScriptInstance script;
    script.activate(48000.0, 256);
    script.set_script("function build(knob) return {} end");

    const std::vector<nirbija::MidiEvent> out = through(script, {note(64, 77, 3)});
    if (out.size() != 1) fail("a script that maps nothing lost the note");
    else {
      expect(out[0].data[1] == 64 && out[0].data[2] == 77 &&
                 (out[0].data[0] & 0x0f) == 3,
             "a script that maps nothing still changed the note");
    }
  }

  // --- a broken script is reported, and the last good one keeps playing -------
  {
    nirbija::ScriptInstance script;
    script.activate(48000.0, 256);
    script.set_script(R"(
      function build(knob)
        local m = {}
        for n = 0, 127 do m[n] = 100 end
        return { note_map = m }
      end)");

    expect(!script.set_script("function build( this is not lua"),
           "a script that does not compile was accepted");
    expect(!script.error().empty(), "a broken script reported no error");

    const std::vector<nirbija::MidiEvent> out = through(script, {note(60)});
    if (out.size() != 1) fail("the last good script stopped working");
    else expect(out[0].data[1] == 100,
                "a broken script replaced the one that worked");
  }

  // --- a script with no build function is refused -----------------------------
  {
    nirbija::ScriptInstance script;
    script.activate(48000.0, 256);
    expect(!script.set_script("local x = 1"),
           "a script with no build function was accepted");
  }

  // --- the sandbox is shut -----------------------------------------------------
  {
    nirbija::ScriptInstance script;
    script.activate(48000.0, 256);
    for (const char* reach : {"io", "os", "package", "require", "load",
                              "dofile", "loadfile", "debug", "collectgarbage"}) {
      const std::string source =
          std::string("function build(knob) return { n = ") + reach + ".x } end";
      script.set_script(source);
      // Either it refuses outright or it errors on the call; what must not
      // happen is a script reaching the machine and succeeding.
      expect(!script.error().empty() || true, "");
      if (script.error().empty()) fail(std::string(reach) + " is reachable from a script");
    }
  }

  // --- a loop that never ends is caught, not survived --------------------------
  {
    nirbija::ScriptInstance script;
    script.activate(48000.0, 256);
    const bool ok = script.set_script(
        "function build(knob) while true do end end");
    expect(!ok, "a script that loops forever was accepted");
    expect(!script.error().empty(), "an endless loop reported no error");
    // The point: control came back at all. Reaching this line is the check.
    std::printf("  endless loop stopped: %s\n", script.error().c_str());
  }

  // --- rebuilding while notes flow does not lose the old tables ----------------
  //
  // Publishing a new table set retires the old one; freeing it while the audio
  // thread still holds a pointer to it would be a use-after-free that only
  // shows up under load.
  {
    nirbija::ScriptInstance script;
    script.activate(48000.0, 256);
    for (int round = 0; round < 50; ++round) {
      script.set_script("function build(knob)\n"
                        "  local m = {}\n"
                        "  for n = 0, 127 do m[n] = " +
                        std::to_string(round % 128) +
                        " end\n"
                        "  return { note_map = m }\n"
                        "end");
      const std::vector<nirbija::MidiEvent> out = through(script, {note(60)});
      if (out.size() != 1 || out[0].data[1] != round % 128)
        fail("a rebuild in round " + std::to_string(round) + " lost the mapping");
    }
  }

  // --- state carries the script and the knobs ---------------------------------
  {
    nirbija::ScriptInstance script;
    script.activate(48000.0, 256);
    const std::string source =
        "function build(knob)\n"
        "  local m = {}\n"
        "  for n = 0, 127 do m[n] = math.floor(knob[1] * 100) end\n"
        "  return { note_map = m }\n"
        "end";
    script.set_script(source);
    script.set_parameter(0, 0.5);

    nirbija::ScriptInstance restored;
    restored.activate(48000.0, 256);
    restored.load_state(script.save_state());

    expect(restored.script() == source, "the script did not survive the state");
    expect(restored.parameter_value(0) == 0.5, "a knob did not survive the state");

    const std::vector<nirbija::MidiEvent> out = through(restored, {note(60)});
    if (out.size() != 1) fail("the restored script produced nothing");
    else expect(out[0].data[1] == 50,
                "the restored script did not see the restored knob");
  }

  // A held note must turn off at the pitch that actually sounded, even if
  // a knob rebuild remaps the same incoming key to somewhere else.
  {
    nirbija::ScriptInstance script;
    script.activate(48000.0, 256);
    expect(script.set_script(R"(
      function build(knob)
        local m = {}
        local shift = math.floor(knob[1] * 12 + 0.5)
        for n = 0, 127 do m[n] = math.min(127, n + shift) end
        return { note_map = m }
      end)"), "transpose script refused: " + script.error());

    const auto on = through(script, {note(60)});
    expect(on.size() == 1 && on[0].data[1] == 60, "the first on was not 60");
    script.set_parameter(0, 1.0);  // +12
    const auto off = through(script, {note(60, 0, 0, false)});
    if (off.size() != 1) fail("the matching off was dropped after a rebuild");
    else expect(off[0].data[1] == 60,
                "the off went to " + std::to_string(off[0].data[1]) +
                    " after the rebuild, not the pitch that sounded");
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
