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

  for (size_t i = 0; i < kMaxBuses; ++i) {
    bus_buffers_[i].assign(2, std::vector<float>(max_block_frames, 0.0f));
    bus_ptrs_[i].assign(2, nullptr);
    for (size_t ch = 0; ch < 2; ++ch)
      bus_ptrs_[i][ch] = bus_buffers_[i][ch].data();
  }

  for (size_t i = 0; i < kMaxChannels; ++i) {
    channel_buffers_[i].assign(2, std::vector<float>(max_block_frames, 0.0f));
    channel_ptrs_[i].assign(2, nullptr);
    for (size_t ch = 0; ch < 2; ++ch)
      channel_ptrs_[i][ch] = channel_buffers_[i][ch].data();
  }

  const size_t count = active_.load(std::memory_order_acquire);
  for (size_t i = 0; i < count; ++i)
    if (channels_[i] != nullptr) channels_[i]->prepare(sample_rate, max_block_frames);

  const size_t buses = bus_active_.load(std::memory_order_acquire);
  for (size_t i = 0; i < buses; ++i)
    if (buses_[i] != nullptr) buses_[i]->prepare(sample_rate, max_block_frames);
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

size_t AudioGraph::add_bus(std::string name) {
  const size_t index = bus_active_.load(std::memory_order_relaxed);
  if (index >= kMaxBuses) return kMaxBuses;

  auto strip = std::make_unique<ChannelStrip>(std::move(name), 2);
  if (sample_rate_ > 0.0) strip->prepare(sample_rate_, max_block_frames_);

  ChannelStrip* raw = strip.get();
  buses_[index] = std::move(strip);
  live_buses_[index].store(raw, std::memory_order_release);
  bus_active_.store(index + 1, std::memory_order_release);
  return index;
}

bool AudioGraph::bus_alive(size_t index) const {
  if (index >= kMaxBuses) return false;
  return live_buses_[index].load(std::memory_order_acquire) != nullptr;
}

void AudioGraph::remove_bus(size_t index) {
  if (index >= bus_active_.load(std::memory_order_relaxed)) return;

  live_buses_[index].store(nullptr, std::memory_order_release);
  if (buses_[index] != nullptr) retired_.push_back(std::move(buses_[index]));

  // Anything pointed at the bus that just went away falls back to the master,
  // which is better than going silent with no visible reason.
  const size_t count = active_.load(std::memory_order_acquire);
  for (size_t i = 0; i < count; ++i) {
    ChannelStrip* strip = live_[i].load(std::memory_order_acquire);
    if (strip != nullptr && strip->destination() == static_cast<int>(index))
      strip->set_destination(-1);
  }
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

// A mono strip is widened here, with constant-power pan so sweeping it across
// the image keeps the same loudness. A stereo strip already had its balance
// applied inside the strip.
void AudioGraph::mix_into(float* const* target, const ChannelStrip& strip,
                          int width, uint32_t frames, float gain) {
  float spread[2] = {1.0f, 1.0f};
  if (width == 1) {
    const float angle = 0.25f * 3.14159265358979f * (strip.pan() + 1.0f);
    spread[0] = std::cos(angle);
    spread[1] = std::sin(angle);
  }
  for (int ch = 0; ch < 2; ++ch) {
    const float* source = scratch_ptrs_[std::min(ch, width - 1)];
    const float amount = spread[ch] * gain;
    for (uint32_t f = 0; f < frames; ++f) target[ch][f] += source[f] * amount;
  }
}

void AudioGraph::apply_sends(const ChannelStrip& strip, int width,
                             uint32_t frames, long long rendered_buses) {
  for (size_t i = 0; i < kMaxSends; ++i) {
    const int bus = strip.send_bus(i);
    if (bus < 0) continue;

    const float level = strip.send_level(i);
    if (level <= 0.0f) continue;

    const long long index = bus;
    // The same rule as a destination: only a bus still ahead in this pass, or
    // the send would land in a buffer that has already been rendered.
    if (index >= static_cast<long long>(bus_count())) continue;
    if (index <= rendered_buses) continue;
    if (live_buses_[index].load(std::memory_order_acquire) == nullptr) continue;

    mix_into(bus_ptrs_[index].data(), strip, width, frames, level);
  }
}

float* const* AudioGraph::destination_for(int destination, float* const* master,
                                          long long rendered_channels,
                                          long long rendered_buses) {
  if (destination == kMasterDestination) return master;

  if (destination >= kChannelDestination) {
    const long long slot = destination - kChannelDestination;
    // Only a channel this pass has not reached yet. Anything else would land in
    // a buffer that has already been mixed away.
    if (slot <= rendered_channels) return master;
    if (slot >= static_cast<long long>(kMaxChannels)) return master;
    if (live_[slot].load(std::memory_order_acquire) == nullptr) return master;
    return channel_ptrs_[slot].data();
  }

  const long long index = destination;
  if (index >= static_cast<long long>(bus_count())) return master;
  if (index <= rendered_buses) return master;
  if (live_buses_[index].load(std::memory_order_acquire) == nullptr) return master;
  return bus_ptrs_[index].data();
}

void AudioGraph::render(float* const* master, uint32_t frames) {
  for (int ch = 0; ch < 2; ++ch) std::fill_n(master[ch], frames, 0.0f);

  const size_t buses = bus_active_.load(std::memory_order_acquire);
  for (size_t i = 0; i < buses; ++i)
    for (int ch = 0; ch < 2; ++ch) std::fill_n(bus_ptrs_[i][ch], frames, 0.0f);

  const size_t channel_slots = active_.load(std::memory_order_acquire);
  for (size_t i = 0; i < channel_slots; ++i)
    for (int ch = 0; ch < 2; ++ch) std::fill_n(channel_ptrs_[i][ch], frames, 0.0f);

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

    // Whatever an earlier channel sent here is part of this channel's input,
    // alongside its own port.
    for (int ch = 0; ch < width; ++ch)
      for (uint32_t f = 0; f < frames; ++f)
        scratch_ptrs_[ch][f] += channel_ptrs_[i][std::min(ch, 1)][f];

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

    // Channels run before every bus, so any live bus is still ahead of them.
    apply_sends(strip, width, frames, -1);
    mix_into(destination_for(strip.destination(), master,
                             static_cast<long long>(i), -1),
             strip, width, frames);
  }

  // Buses in index order, each summing into the master or into a bus still
  // ahead of it.
  for (size_t i = 0; i < buses; ++i) {
    ChannelStrip* live = live_buses_[i].load(std::memory_order_acquire);
    if (live == nullptr) continue;

    for (int ch = 0; ch < 2; ++ch)
      std::copy_n(bus_ptrs_[i][ch], frames, scratch_ptrs_[ch]);

    live->process(scratch_ptrs_.data(), frames, nullptr, 0, &transport_);

    if (recorder != nullptr) {
      const int track = live->record_track();
      if (track >= 0)
        recorder->write(static_cast<size_t>(track), scratch_ptrs_.data(), 2, frames);
    }

    apply_sends(*live, 2, frames, static_cast<long long>(i));
    // Every channel is behind a bus by now, so a bus can only feed a later bus
    // or the master.
    mix_into(destination_for(live->destination(), master,
                             static_cast<long long>(kMaxChannels),
                             static_cast<long long>(i)),
             *live, 2, frames);
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
