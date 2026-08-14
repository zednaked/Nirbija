#include "core/audio_graph.h"

#include <algorithm>

namespace nirbija {

AudioGraph::AudioGraph() = default;
AudioGraph::~AudioGraph() = default;

void AudioGraph::prepare(double sample_rate, uint32_t max_block_frames) {
  sample_rate_ = sample_rate;
  max_block_frames_ = max_block_frames;

  size_t max_channels = 2;
  for (const auto& channel : channels_)
    max_channels = std::max(max_channels,
                            static_cast<size_t>(channel->channel_count()));

  scratch_.assign(max_channels, std::vector<float>(max_block_frames, 0.0f));
  scratch_ptrs_.assign(max_channels, nullptr);
  for (size_t i = 0; i < max_channels; ++i) scratch_ptrs_[i] = scratch_[i].data();

  for (auto& channel : channels_) channel->prepare(sample_rate, max_block_frames);
}

bool AudioGraph::any_soloed() const {
  return std::any_of(channels_.begin(), channels_.end(),
                     [](const auto& c) { return c->soloed(); });
}

void AudioGraph::render(float* const* master, uint32_t frames) {
  for (int ch = 0; ch < 2; ++ch) std::fill_n(master[ch], frames, 0.0f);

  const bool solo_active = any_soloed();

  for (auto& channel : channels_) {
    if (solo_active && !channel->soloed()) continue;

    // TODO(phase-1): fill scratch from the channel's JACK input ports. Until the
    // engine wires real sources, every strip renders silence and this is a
    // structural pass only.
    const int channel_channels = channel->channel_count();
    for (int ch = 0; ch < channel_channels; ++ch)
      std::fill_n(scratch_ptrs_[ch], frames, 0.0f);

    channel->process(scratch_ptrs_.data(), frames);

    for (int ch = 0; ch < 2; ++ch) {
      const float* source = scratch_ptrs_[std::min(ch, channel_channels - 1)];
      for (uint32_t i = 0; i < frames; ++i) master[ch][i] += source[i];
    }
  }
}

ChannelStrip& AudioGraph::add_channel(std::string name, int channel_count) {
  channels_.push_back(
      std::make_unique<ChannelStrip>(std::move(name), channel_count));
  ChannelStrip& added = *channels_.back();
  if (sample_rate_ > 0.0) {
    // Re-prepare so the scratch buffers cover the new channel's width.
    prepare(sample_rate_, max_block_frames_);
  }
  return added;
}

}  // namespace nirbija
