// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

// What the three hosting backends share. Each format has its own ABI, but the
// host-side plumbing around it - shuffling strip channels into plugin ports,
// keeping a dlopen'd module alive, waiting for the audio thread to let go,
// remembering what a scan found - is the same job three times over. It lives
// here once so a fix in one backend is a fix in all of them.

#include "core/plugin.h"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace nirbija::hosting {

// --- audio buffer shuffling ---------------------------------------------------

// Feeds every plugin input from the strip. The strip is narrower than the
// plugin as often as not; extra plugin inputs get a copy of the last strip
// channel rather than silence, which is what a mono source into a stereo
// effect should sound like.
inline void copy_strip_inputs(const float* const* inputs, int strip_channels,
                              float* const* plugin_inputs, size_t plugin_count,
                              uint32_t frames) {
  for (size_t i = 0; i < plugin_count; ++i) {
    const int source = std::min(static_cast<int>(i), strip_channels - 1);
    std::copy_n(inputs[source], frames, plugin_inputs[i]);
  }
}

// The other direction: a mono plugin on a stereo strip fills both sides.
inline void copy_strip_outputs(const float* const* plugin_outputs,
                               size_t plugin_count, float* const* outputs,
                               int strip_channels, uint32_t frames) {
  if (plugin_count == 0) return;
  for (int ch = 0; ch < strip_channels; ++ch) {
    const size_t source = std::min(static_cast<size_t>(ch), plugin_count - 1);
    std::copy_n(plugin_outputs[source], frames, outputs[ch]);
  }
}

// Stereo pairs beyond the strip's own width, see PluginInstance.
inline int extra_output_pairs(size_t plugin_outputs, int strip_channels) {
  const int extra = static_cast<int>(plugin_outputs) - strip_channels;
  return extra > 0 ? (extra + 1) / 2 : 0;
}

// One of those pairs. An odd last channel is doubled into both sides; a pair
// that does not exist is silence.
inline void copy_extra_output(const float* const* plugin_outputs,
                              size_t plugin_count, int strip_channels, int pair,
                              float* left, float* right, uint32_t frames) {
  const size_t base =
      static_cast<size_t>(strip_channels) + static_cast<size_t>(pair) * 2;
  if (base < plugin_count)
    std::copy_n(plugin_outputs[base], frames, left);
  else
    std::fill_n(left, frames, 0.0f);
  if (base + 1 < plugin_count)
    std::copy_n(plugin_outputs[base + 1], frames, right);
  else
    std::copy_n(left, frames, right);
}

// --- parking the audio thread -------------------------------------------------

// Waits for the audio thread to acknowledge something the UI thread asked of
// it. `generation` is a counter process() bumps every block; `done(seen)` says
// whether the acknowledgement has arrived, given the count at the start.
//
// The counter not moving at all means process() is not being called - the
// graph is parked, or there is no audio thread - which is the common case on
// session load. Returning at once there is what keeps a session with thirty
// plugins from paying 200 ms per plugin for nothing. Returns whether the
// condition was met; false means the audio thread is still moving and did not
// comply, and the caller must not touch anything it shares with it.
template <typename Done>
bool wait_for_audio_thread(const std::atomic<uint64_t>& generation, Done done) {
  const uint64_t seen = generation.load(std::memory_order_acquire);

  bool moving = false;
  for (int spins = 0; spins < 5 && !moving; ++spins) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    moving = generation.load(std::memory_order_acquire) > seen;
  }
  if (!moving) return true;

  for (int spins = 0; spins < 100 && !done(seen); ++spins)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  return done(seen);
}

// --- loaded modules -----------------------------------------------------------

// How many times any backend has dlopen'd a plugin module since the process
// started. A test hook: the scan cache is only worth having if a second scan
// leaves this alone.
inline std::atomic<uint64_t>& module_open_count() {
  static std::atomic<uint64_t> count{0};
  return count;
}

// Modules are cached so loading two plugins from one bundle does not dlopen it
// twice, and so a module stays resident while any of its plugins is alive.
//
// Once a plugin has actually been instantiated from a module, the module is
// pinned for the life of the backend: many GTK and JUCE plugins register
// process-wide state (type systems, timers, X11 error handlers) from their
// constructors and do not survive a dlclose followed by a second dlopen. A
// scan-only open is not pinned - a scan touches every module on the machine.
template <typename Module>
class ModuleCache {
 public:
  template <typename Opener>
  std::shared_ptr<Module> get(const std::string& key, Opener open, bool pin) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::shared_ptr<Module> module;
    auto it = weak_.find(key);
    if (it != weak_.end()) module = it->second.lock();
    if (module == nullptr) {
      module = open();
      if (module == nullptr) return nullptr;
      weak_[key] = module;
    }
    if (pin) pinned_[key] = module;
    return module;
  }

 private:
  std::mutex mutex_;
  std::map<std::string, std::weak_ptr<Module>> weak_;
  std::map<std::string, std::shared_ptr<Module>> pinned_;
};

// --- note names ---------------------------------------------------------------

// The list of named keys a plugin publishes, refreshed lazily. The plugin's
// "names changed" callback can fire from any thread while a refresh runs; a
// plain "valid" flag set at the end would clobber an invalidation that landed
// mid-read, so this is a generation counter: a refresh only counts if the
// generation it started with is still current when it finishes.
class NoteNameCache {
 public:
  // Any thread.
  void invalidate() { dirty_gen_.fetch_add(1, std::memory_order_acq_rel); }

  bool stale() const {
    return cached_gen_ != dirty_gen_.load(std::memory_order_acquire);
  }

  // Main thread. `read(out)` fills the vector from the plugin. Bounded: a
  // plugin that bumps the generation from its own thread on every block could
  // otherwise never let this converge, spinning the caller forever. It settles
  // for a possibly-stale result after a few tries rather than freeze; the
  // cached generation is then left stale on purpose so the next call retries.
  template <typename Read>
  void refresh(Read read) const {
    static constexpr int kMaxAttempts = 8;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
      const uint64_t gen = dirty_gen_.load(std::memory_order_acquire);
      names_.clear();
      read(names_);
      if (dirty_gen_.load(std::memory_order_acquire) == gen) {
        cached_gen_ = gen;
        return;
      }
    }
  }

  const std::vector<NoteName>& names() const { return names_; }

  void clear() {
    names_.clear();
    invalidate();
  }

 private:
  mutable std::atomic<uint64_t> dirty_gen_{1};
  mutable uint64_t cached_gen_ = 0;
  mutable std::vector<NoteName> names_;
};

// --- MIDI <-> native events -------------------------------------------------

// A three-byte MIDI message taken apart, for the formats that want notes and
// expression as typed events rather than bytes.
struct MidiMessage {
  enum class Kind { Other, NoteOn, NoteOff, Controller, PitchBend, ChannelPressure };
  Kind kind = Kind::Other;
  uint8_t channel = 0;
  uint8_t key = 0;         // NoteOn/NoteOff
  uint8_t controller = 0;  // Controller
  double value = 0.0;      // velocity, controller value, bend or pressure, 0..1
};

inline MidiMessage decode_midi(const MidiEvent& event) {
  MidiMessage out;
  if (event.size < 2) return out;
  const uint8_t status = event.data[0] & 0xf0u;
  out.channel = event.data[0] & 0x0fu;
  switch (status) {
    case 0x90:
      out.key = event.data[1];
      // A note-on at velocity zero is a note-off by long convention.
      if (event.size >= 3 && event.data[2] > 0) {
        out.kind = MidiMessage::Kind::NoteOn;
        out.value = event.data[2] / 127.0;
      } else {
        out.kind = MidiMessage::Kind::NoteOff;
      }
      break;
    case 0x80:
      out.kind = MidiMessage::Kind::NoteOff;
      out.key = event.data[1];
      out.value = event.size >= 3 ? event.data[2] / 127.0 : 0.0;
      break;
    case 0xb0:
      out.kind = MidiMessage::Kind::Controller;
      out.controller = event.data[1];
      out.value = event.size >= 3 ? event.data[2] / 127.0 : 0.0;
      break;
    case 0xe0:
      out.kind = MidiMessage::Kind::PitchBend;
      out.value = event.size >= 3
                      ? ((event.data[2] << 7) | event.data[1]) / 16383.0
                      : 0.5;
      break;
    case 0xd0:
      out.kind = MidiMessage::Kind::ChannelPressure;
      out.value = event.data[1] / 127.0;
      break;
    default:
      break;
  }
  return out;
}

// The way back: a typed note from a plugin becomes the one representation the
// rest of the host deals in. Velocity is 0..1 and lands on 0..127.
inline MidiEvent make_note_event(bool on, int channel, int key, double velocity,
                                 uint32_t frame) {
  MidiEvent event;
  event.frame = frame;
  event.size = 3;
  event.data[0] = static_cast<uint8_t>((on ? 0x90 : 0x80) | (channel & 0x0f));
  event.data[1] = static_cast<uint8_t>(key & 0x7f);
  event.data[2] = static_cast<uint8_t>(std::clamp(velocity * 127.0, 0.0, 127.0));
  return event;
}

// --- a zero-copy SPSC ring ----------------------------------------------------

// Like RtQueue, but the producer writes into the slot in place and the
// consumer reads it in place. For payloads of several kilobytes - an atom
// frame on its way to an editor - push(T) would copy the whole thing twice
// (into the argument, then into the slot); this copies it exactly once, from
// wherever it came from.
template <typename T, size_t Capacity>
class SlotQueue {
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two so the wrap is a mask");

 public:
  // Producer: the slot to fill, or null when full. commit() publishes it.
  T* begin_push() {
    const size_t write = write_.load(std::memory_order_relaxed);
    const size_t next = (write + 1) & kMask;
    if (next == read_.load(std::memory_order_acquire)) return nullptr;
    return &slots_[write];
  }
  void commit_push() {
    const size_t write = write_.load(std::memory_order_relaxed);
    write_.store((write + 1) & kMask, std::memory_order_release);
  }

  // Consumer: the oldest slot, or null when empty. pop() releases it.
  const T* peek() const {
    const size_t read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire)) return nullptr;
    return &slots_[read];
  }
  void pop() {
    const size_t read = read_.load(std::memory_order_relaxed);
    read_.store((read + 1) & kMask, std::memory_order_release);
  }

 private:
  static constexpr size_t kMask = Capacity - 1;
  std::array<T, Capacity> slots_{};
  std::atomic<size_t> write_{0};
  std::atomic<size_t> read_{0};
};

// --- the scan cache -----------------------------------------------------------

// What a previous scan learned about each module, so the next scan does not
// dlopen it again. Opening two hundred plugins costs seconds and runs two
// hundred static initialisers, several of which have crashed the host on
// their own; a module unchanged since last time (same path, mtime and size)
// has nothing new to say.
//
// The file is a small hand-written JSON document under
// $XDG_CACHE_HOME/nirbija/plugins-<format>.json. It is a cache: anything
// wrong with it - truncated, edited, from another version - is ignored and
// rebuilt, never trusted, and never reported.
class ScanCache {
 public:
  // `format` is stamped on every descriptor read back: the file does not
  // carry it, and a descriptor left at its default says Internal - which
  // made every CLAP and VST3 plugin from a warm cache unfindable by the
  // format a session names it with.
  ScanCache(std::string_view format_name, PluginFormat format) : format_(format) {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    std::filesystem::path base;
    if (xdg != nullptr && *xdg != '\0') {
      base = xdg;
    } else if (const char* home = std::getenv("HOME")) {
      base = std::filesystem::path(home) / ".cache";
    } else {
      return;  // nowhere to keep it; scanning still works, only slower
    }
    dir_ = base / "nirbija";
    file_ = dir_ / ("plugins-" + std::string(format_name) + ".json");
    load();
  }

  // The descriptors recorded for `module` when it last looked like `binary`
  // does now. False means it must be opened and read again.
  bool lookup(const std::string& module, const std::filesystem::path& binary,
              std::vector<PluginDescriptor>* out) {
    Stamp stamp;
    if (!stat_of(binary, &stamp)) return false;
    auto it = entries_.find(module);
    if (it == entries_.end()) return false;
    if (it->second.stamp.mtime != stamp.mtime || it->second.stamp.size != stamp.size)
      return false;
    it->second.seen = true;
    *out = it->second.plugins;
    for (PluginDescriptor& descriptor : *out) descriptor.format = format_;
    return true;
  }

  // What a fresh read of `module` found. An empty list is worth remembering
  // too: a module that is not a plugin at all should not be opened every time.
  void store(const std::string& module, const std::filesystem::path& binary,
             const std::vector<PluginDescriptor>& plugins) {
    Entry entry;
    if (!stat_of(binary, &entry.stamp)) return;
    entry.plugins = plugins;
    entry.seen = true;
    entries_[module] = std::move(entry);
    dirty_ = true;
  }

  // Writes the file if anything changed. Entries not seen by this scan are
  // dropped: their module is gone from disk.
  void save() {
    if (file_.empty()) return;
    bool pruned = false;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (!it->second.seen) {
        it = entries_.erase(it);
        pruned = true;
      } else {
        ++it;
      }
    }
    if (!dirty_ && !pruned) return;

    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    // Written beside and renamed over: a crash mid-write leaves the old cache,
    // not half of a new one.
    const std::filesystem::path temp = file_.string() + ".tmp";
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      if (!out) return;
      out << serialize();
    }
    std::filesystem::rename(temp, file_, ec);
    if (ec) std::filesystem::remove(temp, ec);
    dirty_ = false;
  }

 private:
  PluginFormat format_;

  struct Stamp {
    int64_t mtime = 0;
    int64_t size = 0;
  };
  struct Entry {
    Stamp stamp;
    std::vector<PluginDescriptor> plugins;
    bool seen = false;
  };

  static bool stat_of(const std::filesystem::path& path, Stamp* out) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    out->mtime = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL +
                 st.st_mtim.tv_nsec;
    out->size = static_cast<int64_t>(st.st_size);
    return true;
  }

  // -- writing --

  static void write_string(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
          } else {
            out += c;
          }
      }
    }
    out += '"';
  }

  std::string serialize() const {
    std::string out = "{\"version\":1,\"modules\":[";
    bool first_module = true;
    for (const auto& [module, entry] : entries_) {
      if (!first_module) out += ',';
      first_module = false;
      out += "{\"path\":";
      write_string(out, module);
      // Timestamps as strings: nanoseconds since the epoch do not fit a JSON
      // number without losing the low digits, which are the ones that change.
      out += ",\"mtime\":\"" + std::to_string(entry.stamp.mtime) + "\"";
      out += ",\"size\":\"" + std::to_string(entry.stamp.size) + "\"";
      out += ",\"plugins\":[";
      bool first_plugin = true;
      for (const PluginDescriptor& d : entry.plugins) {
        if (!first_plugin) out += ',';
        first_plugin = false;
        out += "{\"uid\":";
        write_string(out, d.uid);
        out += ",\"path\":";
        write_string(out, d.path);
        out += ",\"name\":";
        write_string(out, d.name);
        out += ",\"vendor\":";
        write_string(out, d.vendor);
        out += ",\"category\":";
        write_string(out, d.category);
        out += ",\"kind\":" + std::to_string(static_cast<int>(d.kind));
        out += ",\"in\":" + std::to_string(d.audio_inputs);
        out += ",\"out\":" + std::to_string(d.audio_outputs);
        out += std::string(",\"midi\":") + (d.has_midi_input ? "true" : "false");
        out += '}';
      }
      out += "]}";
    }
    out += "]}\n";
    return out;
  }

  // -- reading --

  // Just enough JSON to read back what serialize() wrote. Numbers are kept as
  // text and converted where they are used.
  struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    std::string text;  // Number or String
    std::vector<Json> items;
    std::map<std::string, Json> fields;

    const Json* get(const char* key) const {
      auto it = fields.find(key);
      return it == fields.end() ? nullptr : &it->second;
    }
    std::string str() const { return type == Type::String ? text : std::string(); }
    int64_t integer() const {
      if (type != Type::Number && type != Type::String) return 0;
      return std::strtoll(text.c_str(), nullptr, 10);
    }
  };

  struct Parser {
    std::string_view in;
    size_t pos = 0;
    int depth = 0;

    void skip() {
      while (pos < in.size() && std::isspace(static_cast<unsigned char>(in[pos])))
        ++pos;
    }
    bool take(char c) {
      skip();
      if (pos < in.size() && in[pos] == c) {
        ++pos;
        return true;
      }
      return false;
    }

    bool value(Json& out) {
      skip();
      if (pos >= in.size()) return false;
      if (++depth > 32) return false;  // a hand-edited file is not a stack test
      bool ok = false;
      const char c = in[pos];
      if (c == '{') {
        ok = object(out);
      } else if (c == '[') {
        ok = array(out);
      } else if (c == '"') {
        out.type = Json::Type::String;
        ok = string(out.text);
      } else if (in.substr(pos, 4) == "true") {
        out.type = Json::Type::Bool;
        out.boolean = true;
        pos += 4;
        ok = true;
      } else if (in.substr(pos, 5) == "false") {
        out.type = Json::Type::Bool;
        pos += 5;
        ok = true;
      } else if (in.substr(pos, 4) == "null") {
        pos += 4;
        ok = true;
      } else {
        ok = number(out);
      }
      --depth;
      return ok;
    }

    bool number(Json& out) {
      const size_t start = pos;
      while (pos < in.size() &&
             (std::isdigit(static_cast<unsigned char>(in[pos])) || in[pos] == '-' ||
              in[pos] == '+' || in[pos] == '.' || in[pos] == 'e' || in[pos] == 'E'))
        ++pos;
      if (pos == start) return false;
      out.type = Json::Type::Number;
      out.text = std::string(in.substr(start, pos - start));
      return true;
    }

    bool string(std::string& out) {
      if (!take('"')) return false;
      while (pos < in.size()) {
        const char c = in[pos++];
        if (c == '"') return true;
        if (c != '\\') {
          out += c;
          continue;
        }
        if (pos >= in.size()) return false;
        const char e = in[pos++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            if (pos + 4 > in.size()) return false;
            const unsigned code = static_cast<unsigned>(
                std::strtoul(std::string(in.substr(pos, 4)).c_str(), nullptr, 16));
            pos += 4;
            // Only control characters are ever escaped by the writer; anything
            // wider is written raw as UTF-8. Encode what arrives anyway.
            if (code < 0x80) {
              out += static_cast<char>(code);
            } else if (code < 0x800) {
              out += static_cast<char>(0xc0 | (code >> 6));
              out += static_cast<char>(0x80 | (code & 0x3f));
            } else {
              out += static_cast<char>(0xe0 | (code >> 12));
              out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
              out += static_cast<char>(0x80 | (code & 0x3f));
            }
            break;
          }
          default: return false;
        }
      }
      return false;
    }

    bool array(Json& out) {
      out.type = Json::Type::Array;
      if (!take('[')) return false;
      if (take(']')) return true;
      while (true) {
        Json item;
        if (!value(item)) return false;
        out.items.push_back(std::move(item));
        if (take(']')) return true;
        if (!take(',')) return false;
      }
    }

    bool object(Json& out) {
      out.type = Json::Type::Object;
      if (!take('{')) return false;
      if (take('}')) return true;
      while (true) {
        skip();
        std::string key;
        if (!string(key)) return false;
        if (!take(':')) return false;
        Json item;
        if (!value(item)) return false;
        out.fields[std::move(key)] = std::move(item);
        if (take('}')) return true;
        if (!take(',')) return false;
      }
    }
  };

  void load() {
    std::ifstream in(file_, std::ios::binary);
    if (!in) return;
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());

    Json root;
    Parser parser{text};
    if (!parser.value(root) || root.type != Json::Type::Object) return;
    const Json* version = root.get("version");
    if (version == nullptr || version->integer() != 1) return;
    const Json* modules = root.get("modules");
    if (modules == nullptr || modules->type != Json::Type::Array) return;

    for (const Json& module : modules->items) {
      if (module.type != Json::Type::Object) continue;
      const Json* path = module.get("path");
      const Json* mtime = module.get("mtime");
      const Json* size = module.get("size");
      const Json* plugins = module.get("plugins");
      if (path == nullptr || mtime == nullptr || size == nullptr ||
          plugins == nullptr || plugins->type != Json::Type::Array)
        continue;
      Entry entry;
      entry.stamp.mtime = mtime->integer();
      entry.stamp.size = size->integer();
      bool sane = true;
      for (const Json& plugin : plugins->items) {
        if (plugin.type != Json::Type::Object) {
          sane = false;
          break;
        }
        PluginDescriptor desc;
        auto field = [&](const char* key) {
          const Json* value = plugin.get(key);
          return value != nullptr ? value->str() : std::string();
        };
        auto number = [&](const char* key) {
          const Json* value = plugin.get(key);
          return value != nullptr ? value->integer() : 0;
        };
        desc.uid = field("uid");
        desc.path = field("path");
        desc.name = field("name");
        desc.vendor = field("vendor");
        desc.category = field("category");
        const int64_t kind = number("kind");
        if (kind < 0 || kind > static_cast<int64_t>(PluginKind::Unknown)) {
          sane = false;
          break;
        }
        desc.kind = static_cast<PluginKind>(kind);
        desc.audio_inputs = static_cast<int>(number("in"));
        desc.audio_outputs = static_cast<int>(number("out"));
        const Json* midi = plugin.get("midi");
        desc.has_midi_input = midi != nullptr && midi->boolean;
        entry.plugins.push_back(std::move(desc));
      }
      if (sane) entries_[path->str()] = std::move(entry);
    }
  }

  std::filesystem::path dir_;
  std::filesystem::path file_;
  std::map<std::string, Entry> entries_;
  bool dirty_ = false;
};

// Parameter values the scene conductor sets from the audio thread, for a
// backend whose own parameter queue has the UI thread as its producer. The
// conductor runs before the graph renders, on the same thread as process(),
// so this is a plain array and not a queue; a parameter set twice before the
// plugin's next block is one entry, holding the later value.
template <size_t Capacity>
struct RtParamBuffer {
  struct Entry {
    uint32_t id = 0;
    double value = 0.0;
  };
  std::array<Entry, Capacity> entries{};
  size_t count = 0;
  uint32_t drops = 0;  // read by the audio thread only; for a test or a log

  void set(uint32_t id, double value) {
    for (size_t i = 0; i < count; ++i) {
      if (entries[i].id == id) {
        entries[i].value = value;
        return;
      }
    }
    if (count < Capacity) entries[count++] = {id, value};
    else ++drops;
  }
  template <typename Fn>
  void drain(Fn&& fn) {
    for (size_t i = 0; i < count; ++i) fn(entries[i]);
    count = 0;
  }
};

// The last value the audio thread gave each parameter, for the UI thread to
// pass on to something only it may touch (a VST3 edit controller, which is
// what the plugin's editor draws from). Latest-value slots, not a queue: a
// fade writes every block, the UI looks a few dozen times a second, and
// only where the knob ended up matters - a queue would fill, and drop the
// value the fade stopped on. A slot belongs to one parameter for good;
// past Capacity distinct ones, the rest go unmirrored.
template <size_t Capacity>
class RtParamMirror {
 public:
  // Audio thread.
  void publish(uint32_t id, double value) {
    size_t i = 0;
    while (i < used_ && ids_[i].load(std::memory_order_relaxed) != id) ++i;
    if (i == used_) {
      if (used_ == Capacity) return;
      ids_[i].store(id, std::memory_order_relaxed);
      ++used_;
    }
    values_[i].store(value, std::memory_order_relaxed);
    seq_[i].fetch_add(1, std::memory_order_release);
  }
  // UI thread: `fn(id, value)` for every parameter moved since last time.
  template <typename Fn>
  void collect(Fn&& fn) {
    for (size_t i = 0; i < Capacity; ++i) {
      const uint32_t seq = seq_[i].load(std::memory_order_acquire);
      if (seq == seen_[i]) continue;
      seen_[i] = seq;
      fn(ids_[i].load(std::memory_order_relaxed), values_[i].load(std::memory_order_relaxed));
    }
  }

 private:
  std::array<std::atomic<uint32_t>, Capacity> ids_{};
  std::array<std::atomic<double>, Capacity> values_{};
  std::array<std::atomic<uint32_t>, Capacity> seq_{};
  size_t used_ = 0;                          // audio thread
  std::array<uint32_t, Capacity> seen_{};    // UI thread
};

// What a plugin's own editor moved, for take_touched(). UI thread only: the
// LV2 write_port and the VST3 performEdit both arrive there. A knob dragged
// across a poll is one entry holding where it got to.
class TouchedList {
 public:
  void note(uint32_t id, double value) {
    for (TouchedParam& item : items_) {
      if (item.id == id) {
        item.value = value;
        return;
      }
    }
    if (items_.size() < kMax) items_.push_back({id, value});
  }
  size_t take(TouchedParam* out, size_t capacity) {
    const size_t n = std::min(capacity, items_.size());
    std::copy_n(items_.begin(), n, out);
    items_.erase(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(n));
    return n;
  }

 private:
  static constexpr size_t kMax = 256;
  std::vector<TouchedParam> items_;
};

}  // namespace nirbija::hosting
