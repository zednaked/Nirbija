#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nirbija {

enum class PluginFormat { Lv2, Clap, Vst3 };

// Enough to find and re-instantiate a plugin across sessions.
struct PluginDescriptor {
  PluginFormat format;
  std::string uid;   // LV2 URI, CLAP id, VST3 class UID
  std::string path;  // bundle or module on disk
  std::string name;
  std::string vendor;
  int audio_inputs = 0;
  int audio_outputs = 0;
  bool has_midi_input = false;
};

struct ParameterInfo {
  uint32_t id;
  std::string name;
  double min_value;
  double max_value;
  double default_value;
};

// One loaded plugin. Everything named process_* runs on the realtime thread and
// must not allocate, lock, or touch the filesystem; everything else runs on the
// UI thread while the instance is detached from the graph.
class PluginInstance {
 public:
  virtual ~PluginInstance() = default;

  virtual bool activate(double sample_rate, uint32_t max_block_frames) = 0;
  virtual void deactivate() = 0;

  // Buffers are non-interleaved, one pointer per channel, `frames` long.
  virtual void process(const float* const* inputs, float* const* outputs,
                       uint32_t frames) = 0;

  virtual std::vector<ParameterInfo> parameters() const = 0;
  virtual double parameter_value(uint32_t id) const = 0;
  virtual void set_parameter(uint32_t id, double value) = 0;

  // Opaque blob owned by the plugin, stored verbatim in the session file.
  virtual std::vector<uint8_t> save_state() const = 0;
  virtual bool load_state(const std::vector<uint8_t>& blob) = 0;

  virtual const PluginDescriptor& descriptor() const = 0;
};

// One per format. Scanning walks the disk, so it never runs on the audio thread.
class PluginBackend {
 public:
  virtual ~PluginBackend() = default;
  virtual PluginFormat format() const = 0;
  virtual std::vector<PluginDescriptor> scan() = 0;
  virtual std::unique_ptr<PluginInstance> instantiate(
      const PluginDescriptor& desc) = 0;
};

// Backends compiled into this build, in scan order.
std::vector<std::unique_ptr<PluginBackend>> make_all_backends();

}  // namespace nirbija
