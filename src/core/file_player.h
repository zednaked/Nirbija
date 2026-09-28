// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/dsp.h"
#include "core/plugin.h"

namespace nirbija {

// A decoded audio file: interleaved stereo at the file's own rate, resampled
// to nothing. The ratio is applied on the way out, so the buffer holds the
// file exactly as decoded. The file player and the sampler share this and the
// decoder below, so a file that loads in one loads in the other and a decode
// bug is fixed once.
struct DecodedAudio {
  std::vector<float> samples;  // frame-interleaved, always 2 channels wide
  uint64_t frames = 0;
  double sample_rate = 48000.0;
};

// UI thread. Decodes the whole file into memory a chunk at a time, widened to
// stereo on the way in so no consumer has to care how many channels the file
// had. `max_frames` (0 for no limit) stops the decode at that many frames -
// the sampler's pads hold a few seconds, and reading a whole album track to
// then throw most of it away was the slow part of loading one. nullptr when
// the file cannot be read or holds no audio.
std::shared_ptr<DecodedAudio> decode_audio_file(const std::string& path,
                                                uint64_t max_frames = 0);

// One channel at a fractional frame position, 4-point Hermite, the neighbours
// clamped at the ends of the buffer. `position` must be below `frames`.
inline float read_interpolated(const DecodedAudio& audio, double position,
                               int channel) {
  const uint64_t i1 = static_cast<uint64_t>(position);
  if (audio.frames == 0 || i1 >= audio.frames) return 0.0f;
  const float frac = static_cast<float>(position - static_cast<double>(i1));
  const uint64_t last = audio.frames - 1;
  const uint64_t i0 = i1 > 0 ? i1 - 1 : 0;
  const uint64_t i2 = std::min(i1 + 1, last);
  const uint64_t i3 = std::min(i1 + 2, last);
  const float* s = audio.samples.data();
  const size_t ch = static_cast<size_t>(channel);
  return dsp::hermite4(s[i0 * 2 + ch], s[i1 * 2 + ch], s[i2 * 2 + ch],
                       s[i3 * 2 + ch], frac);
}

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
  void deactivate() override;

  void set_transport(const TransportInfo& transport) override;
  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override;

  std::vector<ParameterInfo> parameters() const override;
  double parameter_value(uint32_t id) const override;
  void set_parameter(uint32_t id, double value) override;

  std::vector<uint8_t> save_state() const override;
  bool load_state(const std::vector<uint8_t>& blob) override;

  // O host diz se existe thread de audio: sem ela o portao de geracao nunca
  // abre e nada aposentado seria liberado. Ver PluginInstance.
  void reclaim_retired(bool audio_running) override;

  const PluginDescriptor& descriptor() const override { return descriptor_; }

  static PluginDescriptor make_descriptor();

  // Every gain change, start, stop, rewind and file swap is a ramp of this
  // length rather than a step. Five milliseconds: below what reads as a fade,
  // above what reads as a click.
  static constexpr double kFadeSeconds = 0.005;

 private:
  using Buffer = DecodedAudio;

  // The playback itself. Split out so process() can count every call on its
  // way out, early returns included.
  void render(const float* const* inputs, float* const* outputs,
              uint32_t frames);

  PluginDescriptor descriptor_;
  std::string path_;

  std::shared_ptr<Buffer> owned_;
  std::atomic<Buffer*> live_{nullptr};
  // Replaced buffers wait here: the audio thread may still be reading one the
  // moment it is swapped out. See dsp::RetiredList for the order that makes
  // the wait safe.
  dsp::RetiredList<Buffer> retired_;
  std::atomic<uint64_t> process_generation_{0};
  // reclaim_retired(false) frees everything at once because there is no
  // audio thread; the next block must then not crossfade out of a buffer it
  // remembers from before, which may be one of those.
  std::atomic<bool> forget_tail_{false};

  double engine_rate_ = 48000.0;
  int channels_ = 2;

  // Playback position in file frames, fractional because of resampling.
  // Audio thread only. A load asks for a rewind via the flag below rather
  // than writing this from the UI thread while process() is using it.
  double position_ = 0.0;
  std::atomic<bool> rewind_{false};
  bool transport_playing_ = false;
  // The buffer the last block read from, so a swap is noticed on the block
  // after and the old one is crossfaded out inside that block - the one block
  // it is still guaranteed to be alive for.
  Buffer* playing_buffer_ = nullptr;
  // True from a start, rewind or swap until the first loop wrap: the head of
  // the file plays alone the first time round and blends with the tail only
  // when it actually follows it.
  bool first_pass_ = true;

  dsp::LinearRamp gain_ramp_;  // the Gain parameter, zipper-free
  dsp::LinearRamp gate_;       // 1 while the transport plays, 0 stopped
  uint32_t fade_frames_ = 240;

  std::atomic<float> gain_{1.0f};
  std::atomic<bool> loop_{true};
};

}  // namespace nirbija
