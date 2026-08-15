#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nirbija {

enum class PluginFormat { Lv2, Clap, Vst3, Internal };

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

// Where the song is and whether it is moving. Plugins that generate anything
// rhythmic — sequencers, arpeggiators, tempo-synced delays — do nothing useful
// without it.
struct TransportInfo {
  bool playing = false;
  double tempo_bpm = 120.0;
  int numerator = 4;
  int denominator = 4;

  // Song position, at the start of the block.
  double beats = 0.0;    // in quarter notes
  double seconds = 0.0;
  uint64_t frame = 0;

  // Set on the first block after a jump or a start, so a plugin knows to
  // resynchronise rather than assume it can carry on counting.
  bool changed = false;
};

// A short MIDI message on its way to a plugin. Three bytes covers everything
// except SysEx, which no mixer strip needs to pass along.
struct MidiEvent {
  uint32_t frame = 0;  // offset into the block this event lands on
  uint8_t size = 0;
  uint8_t data[3] = {0, 0, 0};
};

struct ParameterInfo {
  uint32_t id;
  std::string name;
  double min_value;
  double max_value;
  double default_value;
};

// A plugin's own editor window, embedded into one of ours. Every format that
// ships a Linux editor draws it with X11, so the parent handle is an X11
// Window id and the host has to be running on X11 or XWayland for any of this
// to work.
class PluginGui {
 public:
  virtual ~PluginGui() = default;

  // Creates the editor as a child of `parent_window`. False means the plugin
  // has no editor this host can embed, which is common and not an error.
  virtual bool attach(uintptr_t parent_window) = 0;
  virtual void detach() = 0;

  // Editors expect to be called back regularly on the main thread; some only
  // repaint from here.
  virtual void idle() = 0;

  // The editor's preferred size. False leaves the caller to pick one.
  virtual bool preferred_size(int* width, int* height) const = 0;
};

// One loaded plugin. Everything named process_* runs on the realtime thread and
// must not allocate, lock, or touch the filesystem; everything else runs on the
// UI thread while the instance is detached from the graph.
class PluginInstance {
 public:
  virtual ~PluginInstance() = default;

  // How many channels the host will hand to process(). Backends adapt their own
  // port count to this; call it before activate().
  virtual void set_channel_layout(int channels) = 0;

  virtual bool activate(double sample_rate, uint32_t max_block_frames) = 0;
  virtual void deactivate() = 0;

  // Buffers are non-interleaved, one pointer per channel, `frames` long.
  virtual void process(const float* const* inputs, float* const* outputs,
                       uint32_t frames) = 0;

  // Realtime thread, called before process(). Plugins that ignore transport
  // simply do not override it.
  virtual void set_transport(const TransportInfo& transport) { (void)transport; }

  // Realtime thread, called before process(). The event is delivered on the
  // plugin's next process call, at the frame it carries. Plugins with no MIDI
  // input ignore it.
  virtual void queue_midi(const MidiEvent& event) { (void)event; }

  // Realtime thread, called after process(). Collects MIDI the plugin produced
  // during that block — a step sequencer or arpeggiator is a plugin whose whole
  // output is MIDI, and it is worth nothing if the host never reads it.
  // Returns how many events were written.
  virtual size_t take_midi_output(MidiEvent* out, size_t capacity) {
    (void)out;
    (void)capacity;
    return 0;
  }

  virtual std::vector<ParameterInfo> parameters() const = 0;
  virtual double parameter_value(uint32_t id) const = 0;
  virtual void set_parameter(uint32_t id, double value) = 0;

  // Opaque blob owned by the plugin, stored verbatim in the session file.
  virtual std::vector<uint8_t> save_state() const = 0;
  virtual bool load_state(const std::vector<uint8_t>& blob) = 0;

  virtual const PluginDescriptor& descriptor() const = 0;

  // Null when the plugin ships no editor this host can embed. Backends that
  // have not implemented editors yet inherit this.
  virtual std::unique_ptr<PluginGui> create_gui() { return nullptr; }
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
