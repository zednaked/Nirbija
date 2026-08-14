#include "core/audio_graph.h"

#include <algorithm>
#include <cmath>

namespace nirbija {

AudioGraph::AudioGraph() = default;
AudioGraph::~AudioGraph() = default;

void AudioGraph::prepare(double sample_rate, uint32_t max_block_frames) {
  sample_rate_ = sample_rate;
  max_block_frames_ = max_block_frames;

  // A strip is mono or stereo, never wider, so two scratch buffers always fit.
  // Keeping the size fixed means add_channel never reallocates behind the audio
  // thread's back.
  scratch_.assign(2, std::vector<float>(max_block_frames, 0.0f));
  scratch_ptrs_.assign(2, nullptr);
  for (size_t i = 0; i < 2; ++i) scratch_ptrs_[i] = scratch_[i].data();

  const size_t count = active_.load(std::memory_order_acquire);
  for (size_t i = 0; i < count; ++i)
    if (channels_[i] != nullptr) channels_[i]->prepare(sample_rate, max_block_frames);
}

bool AudioGraph::any_soloed(size_t count) const {
  for (size_t i = 0; i < count; ++i) {
    const ChannelStrip* strip = live_[i].load(std::memory_order_acquire);
    if (strip != nullptr && strip->soloed()) return true;
  }
  return false;
}

bool AudioGraph::channel_alive(size_t index) const {
  if (index >= kMaxChannels) return false;
  return live_[index].load(std::memory_order_acquire) != nullptr;
}

void AudioGraph::remove_channel(size_t index) {
  if (index >= active_.load(std::memory_order_relaxed)) return;

  // Clearing the live slot hides the channel from the next render pass, but a
  // pass already inside this slot keeps going to the end of the block. So the
  // strip is retired rather than freed, and the sources are left exactly where
  // they are: moving them out would pull the ground from under that pass.
  live_[index].store(nullptr, std::memory_order_release);
  if (channels_[index] != nullptr) retired_.push_back(std::move(channels_[index]));
}

void AudioGraph::render(float* const* master, uint32_t frames) {
  for (int ch = 0; ch < 2; ++ch) std::fill_n(master[ch], frames, 0.0f);

  Recorder* recorder = recorder_.load(std::memory_order_acquire);

  const size_t count = active_.load(std::memory_order_acquire);
  const bool solo_active = any_soloed(count);

  for (size_t i = 0; i < count; ++i) {
    ChannelStrip* live = live_[i].load(std::memory_order_acquire);
    if (live == nullptr) continue;  // removed channel
    ChannelStrip& strip = *live;
    if (solo_active && !strip.soloed()) continue;

    const int width = strip.channel_count();
    sources_[i]->read(scratch_ptrs_.data(), width, frames);

    size_t midi_count = 0;
    if (midi_sources_[i] != nullptr) {
      midi_count = midi_sources_[i]->read(midi_scratch_.data(),
                                          midi_scratch_.size(), frames);
    }

    strip.process(scratch_ptrs_.data(), frames, midi_scratch_.data(), midi_count,
                  &transport_);

    // Recorded post-fader and post-insert: what the channel actually sends to
    // the mix is what someone expects to hear back.
    if (recorder != nullptr) {
      const int track = strip.record_track();
      if (track >= 0)
        recorder->write(static_cast<size_t>(track), scratch_ptrs_.data(), width,
                        frames);
    }

    // A mono strip is widened here, with constant-power pan so sweeping it
    // across the image keeps the same loudness. A stereo strip already had its
    // balance applied inside the strip.
    float spread[2] = {1.0f, 1.0f};
    if (width == 1) {
      const float angle = 0.25f * 3.14159265358979f * (strip.pan() + 1.0f);
      spread[0] = std::cos(angle);
      spread[1] = std::sin(angle);
    }
    for (int ch = 0; ch < 2; ++ch) {
      const float* source = scratch_ptrs_[std::min(ch, width - 1)];
      for (uint32_t f = 0; f < frames; ++f) master[ch][f] += source[f] * spread[ch];
    }
  }

  const float gain = master_gain_.load(std::memory_order_relaxed);
  for (int ch = 0; ch < 2; ++ch) {
    float peak = 0.0f;
    for (uint32_t f = 0; f < frames; ++f) {
      master[ch][f] *= gain;
      peak = std::max(peak, std::fabs(master[ch][f]));
    }
    if (peak > master_peaks_[ch].load(std::memory_order_relaxed))
      master_peaks_[ch].store(peak, std::memory_order_relaxed);
  }

  record_master(master, frames);
}

void AudioGraph::record_master(float* const* master, uint32_t frames) {
  Recorder* recorder = recorder_.load(std::memory_order_acquire);
  if (recorder == nullptr || master_track_ < 0) return;
  recorder->write(static_cast<size_t>(master_track_), master, 2, frames);
  recorder->advance(frames);
}

float AudioGraph::read_master_peak(int channel) {
  return master_peaks_[channel].exchange(0.0f, std::memory_order_relaxed);
}

size_t AudioGraph::add_channel(std::string name, int channel_count,
                               std::unique_ptr<AudioSource> source,
                               std::unique_ptr<MidiSource> midi) {
  const size_t index = active_.load(std::memory_order_relaxed);
  if (index >= kMaxChannels) return kMaxChannels;

  auto strip = std::make_unique<ChannelStrip>(std::move(name), channel_count);
  if (sample_rate_ > 0.0) strip->prepare(sample_rate_, max_block_frames_);

  ChannelStrip* raw = strip.get();
  channels_[index] = std::move(strip);
  sources_[index] = std::move(source);
  midi_sources_[index] = std::move(midi);

  // Release twice over: the slot has to be visible before the count that
  // reaches it, or the audio thread could walk into a slot it cannot read yet.
  live_[index].store(raw, std::memory_order_release);
  active_.store(index + 1, std::memory_order_release);
  return index;
}

}  // namespace nirbija
