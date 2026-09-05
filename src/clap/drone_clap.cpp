// The Drone as a CLAP plugin. The instrument is DroneInstance, unchanged; this
// file is the shell CLAP wants around it - a descriptor, a factory, and the
// four extensions a host needs to play it: parameters, state, one stereo
// output, one note input. No editor: the host draws the thirty-seven
// parameters itself, the way Nirbija does for plugins that ship none.
//
// ECOSYSTEM.md's route, taken: prove the idea as an internal plugin, extract
// it to CLAP once it is good. The DSP is the same file; only the casing
// differs.

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/drone.h"

namespace {

using nirbija::DroneInstance;
using nirbija::MidiEvent;

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                 CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                 CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "com.nirbija.drone",
    "Nirbija Drone",
    "Nirbija",
    "https://zednaked.github.io/nirbija-site/",
    "",
    "",
    "0.4.0",
    "Six strings that never stop sounding: a drone instrument with a swell, "
    "just intonation, slow drift and a long room.",
    kFeatures,
};

struct Plugin {
  clap_plugin_t plugin{};
  const clap_host_t* host = nullptr;
  DroneInstance drone;
  double sample_rate = 48000.0;
  uint32_t max_frames = 256;
};

Plugin* self(const clap_plugin_t* plugin) {
  return static_cast<Plugin*>(plugin->plugin_data);
}

// --- events -----------------------------------------------------------------------

void handle_event(Plugin& p, const clap_event_header_t* header) {
  if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
  switch (header->type) {
    case CLAP_EVENT_PARAM_VALUE: {
      const auto* event = reinterpret_cast<const clap_event_param_value_t*>(header);
      p.drone.set_parameter(event->param_id, event->value);
      break;
    }
    case CLAP_EVENT_NOTE_ON: {
      const auto* event = reinterpret_cast<const clap_event_note_t*>(header);
      if (event->key < 0 || event->key > 127) break;
      MidiEvent midi;
      midi.frame = header->time;
      midi.size = 3;
      midi.data[0] = static_cast<uint8_t>(
          0x90u | static_cast<uint8_t>(std::clamp<int16_t>(event->channel, 0, 15)));
      midi.data[1] = static_cast<uint8_t>(event->key);
      midi.data[2] = static_cast<uint8_t>(
          std::clamp(static_cast<int>(std::lround(event->velocity * 127.0)), 1, 127));
      p.drone.queue_midi(midi);
      break;
    }
    case CLAP_EVENT_MIDI: {
      const auto* event = reinterpret_cast<const clap_event_midi_t*>(header);
      MidiEvent midi;
      midi.frame = header->time;
      midi.size = 3;
      std::memcpy(midi.data, event->data, 3);
      p.drone.queue_midi(midi);
      break;
    }
    default:
      break;
  }
}

void handle_events(Plugin& p, const clap_input_events_t* events) {
  if (events == nullptr) return;
  const uint32_t count = events->size(events);
  for (uint32_t i = 0; i < count; ++i) {
    const clap_event_header_t* header = events->get(events, i);
    if (header != nullptr) handle_event(p, header);
  }
}

// --- params -----------------------------------------------------------------------

bool param_is_stepped(clap_id id) {
  return (id < DroneInstance::Swell &&
          id % DroneInstance::kVoiceStride == DroneInstance::Interval) ||
         id == DroneInstance::Root || id == DroneInstance::Just;
}

bool param_is_unit(clap_id id) {
  return DroneInstance::param_min(id) == 0.0 && DroneInstance::param_max(id) == 1.0 &&
         id != DroneInstance::Just;
}

uint32_t params_count(const clap_plugin_t*) { return DroneInstance::kParamCount; }

bool params_get_info(const clap_plugin_t*, uint32_t index,
                     clap_param_info_t* info) {
  if (index >= DroneInstance::kParamCount) return false;
  const clap_id id = index;
  info->id = id;
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  if (param_is_stepped(id)) info->flags |= CLAP_PARAM_IS_STEPPED;
  info->cookie = nullptr;
  std::snprintf(info->name, sizeof(info->name), "%s", DroneInstance::param_name(id));
  if (id < DroneInstance::Swell)
    std::snprintf(info->module, sizeof(info->module), "Strings/%u",
                  id / DroneInstance::kVoiceStride + 1);
  else
    std::snprintf(info->module, sizeof(info->module), "Drone");
  info->min_value = DroneInstance::param_min(id);
  info->max_value = DroneInstance::param_max(id);
  info->default_value = DroneInstance::param_default(id);
  return true;
}

bool params_get_value(const clap_plugin_t* plugin, clap_id id, double* out) {
  if (id >= DroneInstance::kParamCount) return false;
  *out = self(plugin)->drone.parameter_value(id);
  return true;
}

const char* note_name(int note, char* buffer, size_t size) {
  static const char* const kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                         "F#", "G",  "G#", "A",  "A#", "B"};
  std::snprintf(buffer, size, "%s%d", kNames[((note % 12) + 12) % 12],
                note / 12 - 1);
  return buffer;
}

bool params_value_to_text(const clap_plugin_t*, clap_id id, double value,
                          char* out, uint32_t size) {
  if (id >= DroneInstance::kParamCount) return false;
  char note[16];
  if (id < DroneInstance::Swell) {
    switch (id % DroneInstance::kVoiceStride) {
      case DroneInstance::Interval:
        std::snprintf(out, size, "%+d st", static_cast<int>(std::lround(value)));
        return true;
      case DroneInstance::Detune:
        std::snprintf(out, size, "%+.0f cents", value);
        return true;
      case DroneInstance::Shape:
        std::snprintf(out, size, "%s",
                      value < 0.25 ? "sine" : value < 0.75 ? "triangle" : "saw");
        return true;
      default:
        break;
    }
  }
  switch (id) {
    case DroneInstance::Root:
      std::snprintf(out, size, "%s", note_name(static_cast<int>(std::lround(value)), note, sizeof(note)));
      return true;
    case DroneInstance::Just:
      std::snprintf(out, size, "%s", value >= 0.5 ? "just" : "12-TET");
      return true;
    case DroneInstance::Rise:
      std::snprintf(out, size, "%.2f s", value);
      return true;
    default:
      std::snprintf(out, size, "%.0f %%", value * 100.0);
      return true;
  }
}

bool params_text_to_value(const clap_plugin_t*, clap_id id, const char* text,
                          double* out) {
  if (id >= DroneInstance::kParamCount || text == nullptr) return false;
  if (id == DroneInstance::Just) {
    *out = std::strstr(text, "just") != nullptr ? 1.0 : 0.0;
    return true;
  }
  char* end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text) return false;
  // A percentage typed as "40 %" for a 0..1 parameter.
  *out = (param_is_unit(id) && value > 1.0) ? value / 100.0 : value;
  return true;
}

void params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in,
                  const clap_output_events_t*) {
  handle_events(*self(plugin), in);
}

const clap_plugin_params_t kParams = {
    params_count, params_get_info, params_get_value, params_value_to_text,
    params_text_to_value, params_flush,
};

// --- state ------------------------------------------------------------------------

bool state_save(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
  const std::vector<uint8_t> blob = self(plugin)->drone.save_state();
  size_t written = 0;
  while (written < blob.size()) {
    const int64_t n = stream->write(stream, blob.data() + written,
                                    blob.size() - written);
    if (n <= 0) return false;
    written += static_cast<size_t>(n);
  }
  return true;
}

bool state_load(const clap_plugin_t* plugin, const clap_istream_t* stream) {
  std::vector<uint8_t> blob;
  uint8_t chunk[4096];
  for (;;) {
    const int64_t n = stream->read(stream, chunk, sizeof(chunk));
    if (n < 0) return false;
    if (n == 0) break;
    blob.insert(blob.end(), chunk, chunk + n);
  }
  return self(plugin)->drone.load_state(blob);
}

const clap_plugin_state_t kState = {state_save, state_load};

// --- ports ------------------------------------------------------------------------

uint32_t audio_ports_count(const clap_plugin_t*, bool is_input) {
  return is_input ? 0 : 1;
}

bool audio_ports_get(const clap_plugin_t*, uint32_t index, bool is_input,
                     clap_audio_port_info_t* info) {
  if (is_input || index != 0) return false;
  info->id = 0;
  std::snprintf(info->name, sizeof(info->name), "Out");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}

const clap_plugin_audio_ports_t kAudioPorts = {audio_ports_count, audio_ports_get};

uint32_t note_ports_count(const clap_plugin_t*, bool is_input) {
  return is_input ? 1 : 0;
}

bool note_ports_get(const clap_plugin_t*, uint32_t index, bool is_input,
                    clap_note_port_info_t* info) {
  if (!is_input || index != 0) return false;
  info->id = 0;
  info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
  info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
  std::snprintf(info->name, sizeof(info->name), "Notes");
  return true;
}

const clap_plugin_note_ports_t kNotePorts = {note_ports_count, note_ports_get};

// --- the plugin -------------------------------------------------------------------

bool plugin_init(const clap_plugin_t*) { return true; }

void plugin_destroy(const clap_plugin_t* plugin) { delete self(plugin); }

bool plugin_activate(const clap_plugin_t* plugin, double sample_rate,
                     uint32_t, uint32_t max_frames) {
  Plugin& p = *self(plugin);
  p.sample_rate = sample_rate;
  p.max_frames = max_frames;
  p.drone.set_channel_layout(2);
  return p.drone.activate(sample_rate, max_frames);
}

void plugin_deactivate(const clap_plugin_t* plugin) { self(plugin)->drone.deactivate(); }

bool plugin_start_processing(const clap_plugin_t*) { return true; }
void plugin_stop_processing(const clap_plugin_t*) {}

void plugin_reset(const clap_plugin_t* plugin) {
  Plugin& p = *self(plugin);
  p.drone.activate(p.sample_rate, p.max_frames);
}

clap_process_status plugin_process(const clap_plugin_t* plugin,
                                   const clap_process_t* process) {
  Plugin& p = *self(plugin);
  handle_events(p, process->in_events);
  if (process->audio_outputs_count < 1) return CLAP_PROCESS_CONTINUE;
  const clap_audio_buffer_t& out = process->audio_outputs[0];
  if (out.data32 == nullptr || out.channel_count < 1) return CLAP_PROCESS_CONTINUE;
  const int channels = std::min<int>(2, static_cast<int>(out.channel_count));
  p.drone.set_channel_layout(channels);
  float* outputs[2] = {out.data32[0], channels > 1 ? out.data32[1] : nullptr};
  p.drone.process(nullptr, outputs, process->frames_count);
  // A drone is never done: the host must keep calling.
  return CLAP_PROCESS_CONTINUE;
}

const void* plugin_get_extension(const clap_plugin_t*, const char* id) {
  if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kParams;
  if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &kState;
  if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPorts;
  if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &kNotePorts;
  return nullptr;
}

void plugin_on_main_thread(const clap_plugin_t*) {}

// --- factory and entry -------------------------------------------------------------

uint32_t factory_get_plugin_count(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* factory_get_plugin_descriptor(
    const clap_plugin_factory_t*, uint32_t index) {
  return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t* factory_create_plugin(const clap_plugin_factory_t*,
                                           const clap_host_t* host,
                                           const char* plugin_id) {
  if (plugin_id == nullptr || std::strcmp(plugin_id, kDescriptor.id) != 0)
    return nullptr;
  if (!clap_version_is_compatible(host->clap_version)) return nullptr;
  auto* p = new Plugin;
  p->host = host;
  p->plugin.desc = &kDescriptor;
  p->plugin.plugin_data = p;
  p->plugin.init = plugin_init;
  p->plugin.destroy = plugin_destroy;
  p->plugin.activate = plugin_activate;
  p->plugin.deactivate = plugin_deactivate;
  p->plugin.start_processing = plugin_start_processing;
  p->plugin.stop_processing = plugin_stop_processing;
  p->plugin.reset = plugin_reset;
  p->plugin.process = plugin_process;
  p->plugin.get_extension = plugin_get_extension;
  p->plugin.on_main_thread = plugin_on_main_thread;
  return &p->plugin;
}

const clap_plugin_factory_t kFactory = {
    factory_get_plugin_count,
    factory_get_plugin_descriptor,
    factory_create_plugin,
};

bool entry_init(const char*) { return true; }
void entry_deinit() {}

const void* entry_get_factory(const char* factory_id) {
  if (std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) return &kFactory;
  return nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entry_init,
    entry_deinit,
    entry_get_factory,
};
