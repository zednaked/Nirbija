#include "core/audio_graph.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace nirbija {


AudioGraph::AudioGraph() = default;
AudioGraph::~AudioGraph() = default;

void AudioGraph::prepare(double sample_rate, uint32_t max_block_frames) {
  const bool grew = max_block_frames > max_block_frames_;
  const bool rate_changed = sample_rate != sample_rate_;
  sample_rate_ = sample_rate;
  if (grew) max_block_frames_ = max_block_frames;
  const uint32_t sized = max_block_frames_;

  // Grow scratch, never shrink: a JACK buffer-size callback is not a place
  // to free and realloc under a graph that is about to run again.
  auto grow = [sized, grew](std::vector<std::vector<float>>& buffers,
                            std::vector<float*>& ptrs) {
    if (buffers.size() < 2) buffers.assign(2, {});
    if (ptrs.size() < 2) ptrs.assign(2, nullptr);
    for (size_t i = 0; i < 2; ++i) {
      if (grew || buffers[i].size() < sized) buffers[i].assign(sized, 0.0f);
      ptrs[i] = buffers[i].data();
    }
  };

  grow(scratch_, scratch_ptrs_);
  for (size_t i = 0; i < kMaxBuses; ++i) grow(bus_buffers_[i], bus_ptrs_[i]);
  for (size_t i = 0; i < kMaxChannels; ++i) {
    grow(channel_buffers_[i], channel_ptrs_[i]);
    for (int p = 0; p < kMaxTapPairs; ++p) {
      if (grew || tap_l_[i][p].size() < sized) {
        tap_l_[i][p].assign(sized, 0.0f);
        tap_r_[i][p].assign(sized, 0.0f);
      }
    }
  }

  const size_t count = active_.load(std::memory_order_acquire);
  for (size_t i = 0; i < count; ++i)
    if (channels_[i] != nullptr)
      channels_[i]->prepare(sample_rate, sized, rate_changed);

  const size_t buses = bus_active_.load(std::memory_order_acquire);
  for (size_t i = 0; i < buses; ++i)
    if (buses_[i] != nullptr) buses_[i]->prepare(sample_rate, sized, rate_changed);
}

void AudioGraph::park() { parked_.store(true, std::memory_order_release); }

void AudioGraph::unpark() { parked_.store(false, std::memory_order_release); }

void AudioGraph::wait_renders(int blocks) {
  const uint64_t seen = render_generation_.load(std::memory_order_acquire);
  const uint64_t want = seen + static_cast<uint64_t>(std::max(blocks, 1));
  for (int spins = 0;
       spins < 100 &&
       render_generation_.load(std::memory_order_acquire) < want;
       ++spins) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void AudioGraph::reclaim() {
  const uint64_t now = render_generation_.load(std::memory_order_acquire);
  std::erase_if(retired_, [now](const RetiredStrip& item) {
    return now >= item.generation + 2;
  });
}

bool AudioGraph::any_channel_soloed(size_t count) const {
  for (size_t i = 0; i < count; ++i) {
    const ChannelStrip* strip = live_[i].load(std::memory_order_acquire);
    if (strip != nullptr && strip->soloed()) return true;
  }
  return false;
}

bool AudioGraph::any_bus_soloed(size_t count) const {
  for (size_t i = 0; i < count; ++i) {
    const ChannelStrip* strip = live_buses_[i].load(std::memory_order_acquire);
    if (strip != nullptr && strip->soloed()) return true;
  }
  return false;
}

size_t AudioGraph::next_channel_slot() const {
  const size_t used = active_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < used; ++i) {
    if (live_[i].load(std::memory_order_acquire) == nullptr &&
        channels_[i] == nullptr)
      return i;
  }
  if (used >= kMaxChannels) return kMaxChannels;
  return used;
}

size_t AudioGraph::take_channel_slot() {
  const size_t used = active_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < used; ++i) {
    if (live_[i].load(std::memory_order_acquire) == nullptr &&
        channels_[i] == nullptr)
      return i;
  }
  if (used >= kMaxChannels) return kMaxChannels;
  return used;
}

size_t AudioGraph::take_bus_slot() {
  const size_t used = bus_active_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < used; ++i) {
    if (live_buses_[i].load(std::memory_order_acquire) == nullptr &&
        buses_[i] == nullptr)
      return i;
  }
  if (used >= kMaxBuses) return kMaxBuses;
  return used;
}

bool AudioGraph::channel_alive(size_t index) const {
  if (index >= kMaxChannels) return false;
  return live_[index].load(std::memory_order_acquire) != nullptr;
}

size_t AudioGraph::add_bus(std::string name) {
  const size_t index = take_bus_slot();
  if (index >= kMaxBuses) return kMaxBuses;

  auto strip = std::make_unique<ChannelStrip>(std::move(name), 2);
  if (sample_rate_ > 0.0) strip->prepare(sample_rate_, max_block_frames_);

  ChannelStrip* raw = strip.get();
  buses_[index] = std::move(strip);
  live_buses_[index].store(raw, std::memory_order_release);
  const size_t used = bus_active_.load(std::memory_order_relaxed);
  if (index >= used) bus_active_.store(index + 1, std::memory_order_release);
  return index;
}

bool AudioGraph::bus_alive(size_t index) const {
  if (index >= kMaxBuses) return false;
  return live_buses_[index].load(std::memory_order_acquire) != nullptr;
}

void AudioGraph::remove_bus(size_t index) {
  if (index >= bus_active_.load(std::memory_order_relaxed)) return;

  live_buses_[index].store(nullptr, std::memory_order_release);
  if (buses_[index] != nullptr) {
    retired_.push_back({std::move(buses_[index]),
                        render_generation_.load(std::memory_order_acquire)});
  }

  // Anything pointed at the bus that just went away falls back to the master,
  // which is better than going silent with no visible reason. Sends too.
  const size_t count = active_.load(std::memory_order_acquire);
  for (size_t i = 0; i < count; ++i) {
    ChannelStrip* strip = live_[i].load(std::memory_order_acquire);
    if (strip == nullptr) continue;
    if (strip->destination() == static_cast<int>(index))
      strip->set_destination(-1);
    for (size_t s = 0; s < kMaxSends; ++s) {
      if (strip->send_bus(s) == static_cast<int>(index))
        strip->set_send(s, -1, 0.0f);
    }
  }
  const size_t buses = bus_active_.load(std::memory_order_acquire);
  for (size_t i = 0; i < buses; ++i) {
    ChannelStrip* strip = live_buses_[i].load(std::memory_order_acquire);
    if (strip == nullptr) continue;
    if (strip->destination() == static_cast<int>(index))
      strip->set_destination(-1);
    for (size_t s = 0; s < kMaxSends; ++s) {
      if (strip->send_bus(s) == static_cast<int>(index))
        strip->set_send(s, -1, 0.0f);
    }
  }
}

void AudioGraph::remove_channel(size_t index) {
  if (index >= active_.load(std::memory_order_relaxed)) return;

  // Clearing the live slot hides the channel from the next render pass, but a
  // pass already inside this slot keeps going to the end of the block. So the
  // strip is retired rather than freed, and the sources are left exactly where
  // they are: moving them out would pull the ground from under that pass.
  live_[index].store(nullptr, std::memory_order_release);
  if (channels_[index] != nullptr) {
    retired_.push_back({std::move(channels_[index]),
                        render_generation_.load(std::memory_order_acquire)});
  }
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
  if (max_block_frames_ > 0 && frames > max_block_frames_)
    frames = max_block_frames_;

  for (int ch = 0; ch < 2; ++ch) std::fill_n(master[ch], frames, 0.0f);

  if (parked_.load(std::memory_order_acquire)) {
    render_generation_.fetch_add(1, std::memory_order_release);
    return;
  }

  const size_t buses = bus_active_.load(std::memory_order_acquire);
  for (size_t i = 0; i < buses; ++i)
    for (int ch = 0; ch < 2; ++ch) std::fill_n(bus_ptrs_[i][ch], frames, 0.0f);

  const size_t channel_slots = active_.load(std::memory_order_acquire);
  for (size_t i = 0; i < channel_slots; ++i)
    for (int ch = 0; ch < 2; ++ch) std::fill_n(channel_ptrs_[i][ch], frames, 0.0f);

  Recorder* recorder = recorder_.load(std::memory_order_acquire);

  const size_t count = active_.load(std::memory_order_acquire);
  const bool channel_solo = any_channel_soloed(count);
  const bool bus_solo = any_bus_soloed(buses);

  uint32_t max_lat = 0;
  for (size_t s = 0; s < count; ++s) {
    ChannelStrip* other = live_[s].load(std::memory_order_acquire);
    if (other != nullptr) max_lat = std::max(max_lat, other->latency_samples());
  }
  for (size_t b = 0; b < buses; ++b) {
    ChannelStrip* other = live_buses_[b].load(std::memory_order_acquire);
    if (other != nullptr) max_lat = std::max(max_lat, other->latency_samples());
  }

  for (size_t i = 0; i < count; ++i) {
    ChannelStrip* live = live_[i].load(std::memory_order_acquire);
    if (live == nullptr) continue;  // removed channel
    ChannelStrip& strip = *live;

    const int width = strip.channel_count();
    if (sources_[i] != nullptr)
      sources_[i]->read(scratch_ptrs_.data(), width, frames);
    else
      for (int ch = 0; ch < width; ++ch)
        std::fill_n(scratch_ptrs_[ch], frames, 0.0f);

    // Whatever an earlier channel sent here is part of this channel's input,
    // alongside its own port. Fold stereo into mono so the right side is not
    // thrown away.
    if (width == 1) {
      for (uint32_t f = 0; f < frames; ++f)
        scratch_ptrs_[0][f] +=
            0.5f * (channel_ptrs_[i][0][f] + channel_ptrs_[i][1][f]);
    } else {
      for (int ch = 0; ch < width; ++ch)
        for (uint32_t f = 0; f < frames; ++f)
          scratch_ptrs_[ch][f] += channel_ptrs_[i][std::min(ch, 1)][f];
    }

    size_t midi_count = 0;
    if (midi_sources_[i] != nullptr) {
      midi_count = midi_sources_[i]->read(midi_scratch_.data(),
                                          midi_scratch_.size(), frames);
    }
    for (size_t e = 0; e < injected_midi_n_[i] && midi_count < midi_scratch_.size();
         ++e)
      midi_scratch_[midi_count++] = injected_midi_[i][e];
    injected_midi_n_[i] = 0;

    const int key = strip.sidechain_slot();
    if (key >= 0 && static_cast<size_t>(key) < i) {
      ChannelStrip* src = live_[static_cast<size_t>(key)].load(
          std::memory_order_acquire);
      if (src != nullptr) {
        const float* sc[2] = {src->output_cache(0),
                              src->output_cache(src->channel_count() > 1 ? 1 : 0)};
        if (sc[0] != nullptr) strip.feed_sidechain(sc, 2, frames);
      }
    }

    strip.set_pdc_delay(max_lat > strip.latency_samples()
                            ? max_lat - strip.latency_samples()
                            : 0);

    strip.process(scratch_ptrs_.data(), frames, midi_scratch_.data(), midi_count,
                  &transport_);

    const int pairs = std::min(strip.extra_output_pairs(), kMaxTapPairs);
    for (int p = 0; p < pairs; ++p)
      strip.copy_extra_output(p, tap_l_[i][p].data(), tap_r_[i][p].data(),
                              frames);

    const bool mix_channel =
        (!channel_solo && !bus_solo) ||
        (channel_solo && strip.soloed()) ||
        (!channel_solo && bus_solo &&
         (strip.destination() >= 0 && strip.destination() < kChannelDestination &&
          live_buses_[strip.destination()].load(std::memory_order_acquire) !=
              nullptr &&
          live_buses_[strip.destination()].load(std::memory_order_acquire)
              ->soloed()));

    // Recorded post-fader: a silent (non-soloed) armed track still writes so
    // the take files stay the same length.
    if (recorder != nullptr) {
      const int track = strip.record_track();
      if (track >= 0) {
        if (!mix_channel && channel_solo)
          for (int ch = 0; ch < width; ++ch)
            std::fill_n(scratch_ptrs_[ch], frames, 0.0f);
        recorder->write(static_cast<size_t>(track), scratch_ptrs_.data(), width,
                        frames);
      }
    }

    if (mix_channel ||
        (!channel_solo && bus_solo)) {
      // With only a bus soloed, still send to that bus (dest or send).
      if (mix_channel ||
          (bus_solo && strip.destination() >= 0 &&
           strip.destination() < kChannelDestination)) {
        apply_sends(strip, width, frames, -1);
        mix_into(destination_for(strip.destination(), master,
                                 static_cast<long long>(i), -1),
                 strip, width, frames);
      } else if (bus_solo) {
        apply_sends(strip, width, frames, -1);
      }
    }
  }

  // Buses in index order, each summing into the master or into a bus still
  // ahead of it. Always processed so their inserts keep state; mix depends
  // on solo.
  for (size_t i = 0; i < buses; ++i) {
    ChannelStrip* live = live_buses_[i].load(std::memory_order_acquire);
    if (live == nullptr) continue;

    for (int ch = 0; ch < 2; ++ch)
      std::copy_n(bus_ptrs_[i][ch], frames, scratch_ptrs_[ch]);

    live->set_pdc_delay(max_lat > live->latency_samples()
                            ? max_lat - live->latency_samples()
                            : 0);
    live->process(scratch_ptrs_.data(), frames, nullptr, 0, &transport_);

    const bool mix_bus = !bus_solo || live->soloed() || channel_solo;

    if (recorder != nullptr) {
      const int track = live->record_track();
      if (track >= 0)
        recorder->write(static_cast<size_t>(track), scratch_ptrs_.data(), 2, frames);
    }

    if (mix_bus) {
      apply_sends(*live, 2, frames, static_cast<long long>(i));
      mix_into(destination_for(live->destination(), master,
                               static_cast<long long>(kMaxChannels),
                               static_cast<long long>(i)),
               *live, 2, frames);
    }
  }

  const float gain = master_gain_.load(std::memory_order_relaxed) *
                     (master_dim_.load(std::memory_order_relaxed) ? 0.25f : 1.0f);
  const bool mute = master_mute_.load(std::memory_order_relaxed);
  const bool mono = master_mono_.load(std::memory_order_relaxed);
  if (mono) {
    for (uint32_t f = 0; f < frames; ++f) {
      const float m = 0.5f * (master[0][f] + master[1][f]);
      master[0][f] = master[1][f] = m;
    }
  }
  for (int ch = 0; ch < 2; ++ch) {
    float peak = 0.0f;
    for (uint32_t f = 0; f < frames; ++f) {
      if (mute) master[ch][f] = 0.0f;
      else master[ch][f] *= gain;
      peak = std::max(peak, std::fabs(master[ch][f]));
    }
    if (peak > master_peaks_[ch].load(std::memory_order_relaxed))
      master_peaks_[ch].store(peak, std::memory_order_relaxed);
    if (peak >= 1.0f) master_clip_.store(1.0f, std::memory_order_relaxed);
  }

  record_master(master, frames);
  render_generation_.fetch_add(1, std::memory_order_release);
}

void AudioGraph::record_master(float* const* master, uint32_t frames) {
  Recorder* recorder = recorder_.load(std::memory_order_acquire);
  const int track = master_track_.load(std::memory_order_acquire);
  if (recorder == nullptr || track < 0) return;
  recorder->write(static_cast<size_t>(track), master, 2, frames);
  recorder->advance(frames);
}

float AudioGraph::read_master_peak(int channel) {
  return master_peaks_[channel].exchange(0.0f, std::memory_order_relaxed);
}

void AudioGraph::push_injected_midi(size_t channel, const MidiEvent& event) {
  if (channel >= kMaxChannels) return;
  size_t& n = injected_midi_n_[channel];
  if (n >= injected_midi_[channel].size()) return;
  injected_midi_[channel][n++] = event;
}

void AudioGraph::copy_tap(size_t source, int pair, float* left, float* right,
                          uint32_t frames) const {
  if (source >= kMaxChannels || pair < 0 || pair >= kMaxTapPairs) {
    std::fill_n(left, frames, 0.0f);
    std::fill_n(right, frames, 0.0f);
    return;
  }
  const uint32_t n = std::min(frames, static_cast<uint32_t>(tap_l_[source][pair].size()));
  std::copy_n(tap_l_[source][pair].data(), n, left);
  std::copy_n(tap_r_[source][pair].data(), n, right);
  if (n < frames) {
    std::fill(left + n, left + frames, 0.0f);
    std::fill(right + n, right + frames, 0.0f);
  }
}

size_t AudioGraph::add_channel(std::string name, int channel_count,
                               std::unique_ptr<AudioSource> source,
                               std::unique_ptr<MidiSource> midi) {
  const size_t index = take_channel_slot();
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
  const size_t used = active_.load(std::memory_order_relaxed);
  if (index >= used) active_.store(index + 1, std::memory_order_release);
  return index;
}

}  // namespace nirbija
