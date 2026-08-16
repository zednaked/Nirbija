#include "core/script_plugin.h"

#include <lua.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nirbija {
namespace {

constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kNoteOff = 0x80;

// A runaway loop in a script must not take the interface with it. Lua counts
// instructions for us and calls back; this is generous for building a few
// tables and instant next to a person noticing.
constexpr int kInstructionBudget = 2'000'000;

void budget_hook(lua_State* state, lua_Debug*) {
  luaL_error(state, "script ran too long - is there a loop that never ends?");
}

// Everything a table-building script legitimately needs, and nothing that
// touches the machine. Named one by one rather than by removing from the
// standard set: a deny list is one Lua release away from being wrong.
void open_sandbox(lua_State* state) {
  luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
  luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
  luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
  luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(state, 4);

  // `load` and friends would let a script reach past the sandbox by compiling
  // a new chunk with a different environment; `collectgarbage` lets it stall
  // us on purpose; `dofile` and `loadfile` read the disk.
  for (const char* name : {"load", "loadstring", "dofile", "loadfile",
                           "collectgarbage", "require", "rawequal", "rawlen"}) {
    lua_pushnil(state);
    lua_setglobal(state, name);
  }
}

// Reads out[0..count) from a Lua table at the top of the stack, keyed by index,
// clamping into range. A missing key keeps whatever default was already there,
// so a script that fills only part of a map is not a script that broke.
template <size_t N>
void read_table(lua_State* state, const char* name, std::array<int, N>& out,
                int low, int high) {
  lua_getfield(state, -1, name);
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return;
  }
  for (size_t i = 0; i < N; ++i) {
    lua_pushinteger(state, static_cast<lua_Integer>(i));
    lua_gettable(state, -2);
    if (lua_isnumber(state, -1)) {
      const auto value = static_cast<int>(std::lround(lua_tonumber(state, -1)));
      out[i] = std::clamp(value, low, high);
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
}

}  // namespace

struct ScriptInstance::Lua {
  lua_State* state = nullptr;

  Lua() {
    state = luaL_newstate();
    if (state != nullptr) open_sandbox(state);
  }
  ~Lua() {
    if (state != nullptr) lua_close(state);
  }
  Lua(const Lua&) = delete;
  Lua& operator=(const Lua&) = delete;
};

const char* ScriptInstance::default_script() {
  return R"(-- Runs when the script or a knob changes, never while audio is
-- flowing. Return the tables the mixer should look notes up in.
--
--   knob[1..4]   the four sliders, each 0..1
--
-- Tables you can return, all optional:
--   note_map      [0..127] -> note, or -1 to drop it
--   velocity_map  [0..127] -> velocity, 0 drops the note
--   channel_map   [0..15]  -> channel

local major = { 0, 2, 4, 5, 7, 9, 11 }

function build(knob)
  local root = math.floor(knob[1] * 11 + 0.5)
  local note_map = {}
  for n = 0, 127 do
    -- Fold every note onto the nearest degree of the scale.
    local octave, degree = math.floor(n / 12), (n - root) % 12
    local best = major[1]
    for _, step in ipairs(major) do
      if math.abs(step - degree) < math.abs(best - degree) then best = step end
    end
    note_map[n] = math.min(127, octave * 12 + root + best)
  end
  return { note_map = note_map }
end
)";
}

PluginDescriptor ScriptInstance::make_descriptor() {
  PluginDescriptor descriptor;
  descriptor.format = PluginFormat::Internal;
  descriptor.uid = "nirbija.script";
  descriptor.name = "Script";
  descriptor.vendor = "Nirbija";
  descriptor.audio_inputs = 0;
  descriptor.audio_outputs = 0;
  descriptor.has_midi_input = true;
  descriptor.category = "Scriptable";
  descriptor.kind = PluginKind::MidiEffect;
  return descriptor;
}

ScriptInstance::ScriptInstance() : descriptor_(make_descriptor()) {
  for (auto& knob : knobs_) knob.store(0.0, std::memory_order_relaxed);
  lua_ = std::make_unique<Lua>();
  set_script(default_script());
}

ScriptInstance::~ScriptInstance() {
  delete live_.exchange(nullptr, std::memory_order_acq_rel);
}

bool ScriptInstance::activate(double, uint32_t) { return true; }

void ScriptInstance::publish(std::unique_ptr<Tables> tables) {
  Tables* old = live_.exchange(tables.release(), std::memory_order_acq_rel);
  if (old != nullptr) {
    retired_.emplace_back(blocks_.load(std::memory_order_acquire),
                          std::unique_ptr<Tables>(old));
  }
  collect();
}

void ScriptInstance::collect() {
  // A retired set is only safe to free once the audio thread has finished a
  // block that began after it was replaced. Nothing here ever waits: what is
  // not safe yet stays on the list until the next time round.
  const uint64_t seen = blocks_.load(std::memory_order_acquire);
  std::erase_if(retired_, [seen](const auto& entry) {
    return seen > entry.first + 1;
  });
}

bool ScriptInstance::set_script(const std::string& source) {
  {
    const std::lock_guard<std::mutex> guard(text_mutex_);
    source_ = source;
    error_.clear();
  }
  // A fresh interpreter every time, rather than a fresh chunk in the old one.
  // Globals survive a chunk, so a script that no longer defines `build` would
  // keep running the previous one, and what a script did would depend on what
  // had been typed before it. Building is a UI-thread act; a new state costs
  // nothing that matters here.
  lua_ = std::make_unique<Lua>();
  if (lua_->state == nullptr) return false;
  lua_State* state = lua_->state;

  const auto fail = [&](const std::string& message) {
    const std::lock_guard<std::mutex> guard(text_mutex_);
    error_ = message;
    return false;
  };

  if (luaL_loadbuffer(state, source.c_str(), source.size(), "script") != LUA_OK) {
    const std::string message = lua_tostring(state, -1);
    lua_pop(state, 1);
    return fail(message);
  }

  lua_sethook(state, budget_hook, LUA_MASKCOUNT, kInstructionBudget);
  if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
    const std::string message = lua_tostring(state, -1);
    lua_pop(state, 1);
    lua_sethook(state, nullptr, 0, 0);
    return fail(message);
  }

  lua_getglobal(state, "build");
  if (!lua_isfunction(state, -1)) {
    lua_pop(state, 1);
    lua_sethook(state, nullptr, 0, 0);
    return fail("the script defines no function called build");
  }

  // The knobs go in as a 1-based table, which is how Lua counts.
  lua_newtable(state);
  for (int i = 0; i < kKnobs; ++i) {
    lua_pushinteger(state, i + 1);
    lua_pushnumber(state, knobs_[i].load(std::memory_order_relaxed));
    lua_settable(state, -3);
  }

  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    const std::string message = lua_tostring(state, -1);
    lua_pop(state, 1);
    lua_sethook(state, nullptr, 0, 0);
    return fail(message);
  }
  lua_sethook(state, nullptr, 0, 0);

  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return fail("build must return a table of tables");
  }

  // Identity first, so a script that returns only one map leaves the rest
  // alone rather than silencing everything it did not mention.
  auto tables = std::make_unique<Tables>();
  for (int i = 0; i < 128; ++i) {
    tables->note[i] = i;
    tables->velocity[i] = i;
  }
  for (int i = 0; i < 16; ++i) tables->channel[i] = i;

  read_table(state, "note_map", tables->note, -1, 127);
  read_table(state, "velocity_map", tables->velocity, 0, 127);
  read_table(state, "channel_map", tables->channel, 0, 15);
  lua_pop(state, 1);

  publish(std::move(tables));
  return true;
}

std::string ScriptInstance::script() const {
  const std::lock_guard<std::mutex> guard(text_mutex_);
  return source_;
}

std::string ScriptInstance::error() const {
  const std::lock_guard<std::mutex> guard(text_mutex_);
  return error_;
}

void ScriptInstance::queue_midi(const MidiEvent& event) {
  if (event_count_ >= kMaxEvents) return;

  const Tables* tables = live_.load(std::memory_order_acquire);
  const uint8_t status = event.data[0] & 0xf0;
  const bool is_note = event.size >= 3 && (status == kNoteOn || status == kNoteOff);

  // Anything that is not a note, and anything at all before a script has been
  // published, goes through untouched.
  if (tables == nullptr || !is_note) {
    events_[event_count_++] = event;
    return;
  }

  const int note = tables->note[event.data[1] & 0x7f];
  if (note < 0) return;  // the script dropped it

  const uint8_t channel = event.data[0] & 0x0f;
  int velocity = event.data[2] & 0x7f;
  // A note-off carries a velocity too, and remapping it to zero would be
  // silently turning it into another note-off. Only note-ons can be dropped.
  if (status == kNoteOn) {
    velocity = tables->velocity[velocity];
    if (velocity == 0 && event.data[2] > 0) return;
  }

  MidiEvent& out = events_[event_count_++];
  out = event;
  out.data[0] = static_cast<uint8_t>(status | (tables->channel[channel] & 0x0f));
  out.data[1] = static_cast<uint8_t>(note);
  out.data[2] = static_cast<uint8_t>(velocity);
}

void ScriptInstance::process(const float* const*, float* const*, uint32_t) {
  // Counting blocks is the whole of the realtime side's bookkeeping: it is how
  // the UI thread knows when a replaced table set is safe to free.
  blocks_.fetch_add(1, std::memory_order_acq_rel);
}

size_t ScriptInstance::take_midi_output(MidiEvent* out, size_t capacity) {
  const size_t count = std::min(event_count_, capacity);
  std::copy_n(events_.begin(), count, out);
  event_count_ = 0;
  return count;
}

std::vector<ParameterInfo> ScriptInstance::parameters() const {
  std::vector<ParameterInfo> info;
  info.reserve(kKnobs);
  for (int i = 0; i < kKnobs; ++i) {
    char name[16];
    std::snprintf(name, sizeof(name), "Knob %d", i + 1);
    info.push_back({static_cast<uint32_t>(i), name, 0.0, 1.0, 0.0});
  }
  return info;
}

double ScriptInstance::parameter_value(uint32_t id) const {
  if (id >= kKnobs) return 0.0;
  return knobs_[id].load(std::memory_order_relaxed);
}

void ScriptInstance::set_parameter(uint32_t id, double value) {
  if (id >= kKnobs) return;
  knobs_[id].store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
  // A knob is an input to the script, so moving one means building again.
  // This is the UI thread, which is where that is allowed to happen.
  set_script(script());
}

std::vector<uint8_t> ScriptInstance::save_state() const {
  std::string text;
  char line[64];
  for (int i = 0; i < kKnobs; ++i) {
    std::snprintf(line, sizeof(line), "knob %d %.6f\n", i,
                  knobs_[i].load(std::memory_order_relaxed));
    text += line;
  }
  // The script goes last and takes the rest of the blob, so it may contain
  // anything at all, newlines included, without needing to be escaped.
  text += "script\n";
  text += script();
  return {text.begin(), text.end()};
}

bool ScriptInstance::load_state(const std::vector<uint8_t>& blob) {
  const std::string text(blob.begin(), blob.end());
  size_t position = 0;

  while (position < text.size()) {
    const size_t end = text.find('\n', position);
    const std::string_view line(
        text.data() + position,
        (end == std::string::npos ? text.size() : end) - position);
    const size_t next = (end == std::string::npos) ? text.size() : end + 1;

    if (line == "script") {
      set_script(text.substr(next));
      return true;
    }
    position = next;

    if (line.rfind("knob ", 0) != 0) continue;
    const std::string_view rest = line.substr(5);
    const size_t space = rest.find(' ');
    if (space == std::string_view::npos) continue;

    double index = 0.0;
    double value = 0.0;
    if (!parse_number(rest.substr(0, space), &index)) continue;
    if (!parse_number(rest.substr(space + 1), &value)) continue;
    const int slot = static_cast<int>(index);
    if (slot >= 0 && slot < kKnobs)
      knobs_[slot].store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
  }
  return true;
}

}  // namespace nirbija
