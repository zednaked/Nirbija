#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// A built-in plugin that plays an audio file into its channel. It sits in an
// insert slot like any other plugin, so it gets the strip's fader, sends and
// destination for free — which is exactly how AUM treats its file player.
class FilePlayerInstance : public PluginInstance {
 public:
  FilePlayerInstance();
  ~FilePlayerInstance() override;

  // UI thread. Decodes the whole file into memory and swaps it in; the audio
  // thread only ever sees a finished buffer. Returns false when the file cannot
  // be read, leaving whatever was loaded before still playing.
  bool load(const std::string& path);
  std::string path() const { return path_; }

  // PluginInstance ------------------------------------------------------------
  void set_channel_layout(int channels) override { channels_ = channels; }
  bool activate(double sample_rate, uint32_t max_block_frames) override;
  void deactivate() override {}

  void set_transport(const TransportInfo& transport) override;
  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override;

  std::vector<ParameterInfo> parameters() const override;
  double parameter_value(uint32_t id) const override;
  void set_parameter(uint32_t id, double value) override;

  std::vector<uint8_t> save_state() const override;
  bool load_state(const std::vector<uint8_t>& blob) override;

  const PluginDescriptor& descriptor() const override { return descriptor_; }

  static PluginDescriptor make_descriptor();

 private:
  // The playback itself. Split out so process() can count every call on its
  // way out, early returns included.
  void render(const float* const* inputs, float* const* outputs,
              uint32_t frames);

  // Interleaved stereo, resampled to nothing: the ratio is applied on the way
  // out, so the buffer holds the file exactly as decoded.
  struct Buffer {
    std::vector<float> samples;  // frame-interleaved, always 2 channels wide
    uint64_t frames = 0;
    double sample_rate = 48000.0;
  };

  PluginDescriptor descriptor_;
  std::string path_;

  std::shared_ptr<Buffer> owned_;
  std::atomic<Buffer*> live_{nullptr};
  // Replaced buffers wait here: the audio thread may still be reading one the
  // moment it is swapped out. Tagged with the process generation at retire so
  // the next load can drop the ones the audio thread has since left — without
  // that they accumulated, a whole decoded file each.
  struct RetiredBuffer {
    std::shared_ptr<Buffer> buffer;
    uint64_t generation = 0;
  };
  std::vector<RetiredBuffer> retired_;
  std::atomic<uint64_t> process_generation_{0};

  double engine_rate_ = 48000.0;
  int channels_ = 2;

  // Playback position in file frames, fractional because of resampling.
  double position_ = 0.0;
  bool transport_playing_ = false;

  std::atomic<float> gain_{1.0f};
  std::atomic<bool> loop_{true};
};

}  // namespace nirbija
