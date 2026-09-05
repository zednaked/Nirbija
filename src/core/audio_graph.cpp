#include "core/audio_graph.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace nirbija {

namespace {
// How long the master takes to fade out for a park, and back in after.
constexpr double kParkFadeSeconds = 0.005;
// The least time any mix amount - a solo, a send, the master fader, a mono
// blend - takes to swing all the way: short enough to feel like a switch,
// long enough that nothing steps.
constexpr double kRampSeconds = 0.010;
constexpr double kLimiterLookaheadSeconds = 0.0015;
constexpr double kLimiterReleaseSeconds = 0.080;
// -0.3 dBFS: under full scale by enough that a converter's own filter does
// not overshoot into clipping on the way out.
constexpr float kLimiterCeiling = 0.966f;
}  // namespace

AudioGraph::AudioGraph() {
  channel_last_dest_.fill(kMasterDestination);
  bus_last_dest_.fill(kMasterDestination);
}
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
  for (size_t i = 0; i < count; ++i) {
    if (sources_[i] != nullptr) sources_[i]->prepare(sized);
    if (channels_[i] != nullptr)
      channels_[i]->prepare(sample_rate, sized, rate_changed);
  }

  const size_t buses = bus_active_.load(std::memory_order_acquire);
  for (size_t i = 0; i < buses; ++i)
    if (buses_[i] != nullptr) buses_[i]->prepare(sample_rate, sized, rate_changed);

  if (grew || park_ramp_.size() < sized) park_ramp_.assign(sized, 1.0f);

  // The limiter's lookahead is a length of time, so it is resized on a rate
  // change too. Process is stopped whenever prepare() runs, so the reset of
  // its state here cannot tear a block.
  const size_t lookahead = static_cast<size_t>(
      std::max(1.0, std::round(kLimiterLookaheadSeconds * sample_rate)));
  if (rate_changed || limiter_.lookahead != lookahead) {
    limiter_.lookahead = lookahead;
    for (auto& line : limiter_.delay) line.assign(lookahead, 0.0f);
    limiter_.gains.assign(lookahead, 1.0f);
    limiter_.write = 0;
    limiter_.window_sum = static_cast<double>(lookahead);
    limiter_.envelope = 1.0f;
    limiter_.hold_left = 0;
    limiter_.release_coeff = static_cast<float>(
        std::exp(-1.0 / (kLimiterReleaseSeconds * sample_rate)));
  }
}

uint32_t AudioGraph::master_latency_samples() const {
  return master_limiter() ? static_cast<uint32_t>(limiter_.lookahead) : 0;
}

float AudioGraph::slew(float from, float to, uint32_t frames) const {
  if (sample_rate_ <= 0.0) return to;
  const float most =
      static_cast<float>(frames / (kRampSeconds * sample_rate_));
  return std::clamp(to, from - most, from + most);
}

void AudioGraph::park() { parked_.store(true, std::memory_order_release); }

void AudioGraph::unpark() { parked_.store(false, std::memory_order_release); }

bool AudioGraph::wait_renders(int blocks) {
  const uint64_t seen = render_generation_.load(std::memory_order_acquire);
  const uint64_t want = seen + static_cast<uint64_t>(std::max(blocks, 1));
  for (int spins = 0; spins < 100; ++spins) {
    if (render_generation_.load(std::memory_order_acquire) >= want) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return render_generation_.load(std::memory_order_acquire) >= want;
}

bool AudioGraph::wait_quiescent() {
  const uint64_t seen = quiet_generation_.load(std::memory_order_acquire);
  const uint64_t want = seen + 1;
  for (int spins = 0; spins < 100; ++spins) {
    if (quiet_generation_.load(std::memory_order_acquire) >= want) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return quiet_generation_.load(std::memory_order_acquire) >= want;
}

void AudioGraph::apply_park_ramp(float* buffer, uint32_t frames) const {
  if (buffer == nullptr) return;
  if (last_render_quiet_) {
    std::fill_n(buffer, frames, 0.0f);
    return;
  }
  if (!park_ramp_active_) return;
  const uint32_t n = std::min<uint32_t>(
      frames, static_cast<uint32_t>(park_ramp_.size()));
  for (uint32_t f = 0; f < n; ++f) buffer[f] *= park_ramp_[f];
}

void AudioGraph::reclaim(bool audio_running) {
  // With no audio thread at all there is nothing that could still be inside a
  // retired strip, so the generation gate has nothing to wait for - and it
  // would wait forever: the generation only advances at the end of a render,
  // which the JACK callback drives. The app stays fully usable with no audio
  // server (the status bar says "no audio server - start PipeWire or JACK and
  // restart"), so without this every strip removed there is held until exit.
  //
  // The test cannot be "generation == 0": one created while audio is already
  // running also sits at 0 until its first block, and the audio thread may be
  // inside it by then. Only the absence of the JACK client settles it, which is
  // what the caller passes in.
  if (!audio_running) {
    retired_.clear();
    retired_sources_.clear();
    return;
  }

  const uint64_t now = render_generation_.load(std::memory_order_acquire);
  std::erase_if(retired_, [now](const RetiredStrip& item) {
    return now >= item.generation + 2;
  });
  std::erase_if(retired_sources_, [now](const RetiredSource& item) {
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
                          int width, uint32_t frames, float gain, float* state) {
  float spread[2] = {1.0f, 1.0f};
  if (width == 1) {
    // The strip's own smoothed pan, not the knob: the knob is where the
    // fader is going, the smoothed value is where it is.
    const float angle =
        0.25f * 3.14159265358979f * (strip.smoothed_pan() + 1.0f);
    spread[0] = std::cos(angle);
    spread[1] = std::sin(angle);
  }
  for (int ch = 0; ch < 2; ++ch) {
    const float* source = scratch_ptrs_[std::min(ch, width - 1)];
    const float from = state[ch];
    const float to = slew(from, spread[ch] * gain, frames);
    state[ch] = to;
    if (from == 0.0f && to == 0.0f) continue;
    if (from == to) {
      for (uint32_t f = 0; f < frames; ++f) target[ch][f] += source[f] * to;
      continue;
    }
    const float step = (to - from) / static_cast<float>(frames);
    float amount = from;
    for (uint32_t f = 0; f < frames; ++f) {
      amount += step;
      target[ch][f] += source[f] * amount;
    }
  }
}

void AudioGraph::apply_sends(const ChannelStrip& strip, int width,
                             uint32_t frames, long long rendered_buses,
                             size_t slot, bool audible) {
  for (size_t i = 0; i < kMaxSends; ++i) {
    float* state = send_mix_[slot][i].data();
    const int bus = strip.send_bus(i);
    const float level = audible ? strip.send_level(i) : 0.0f;

    const long long index = bus;
    // The same rule as a destination: only a bus still ahead in this pass, or
    // the send would land in a buffer that has already been rendered.
    const bool reachable =
        bus >= 0 && index < static_cast<long long>(bus_count()) &&
        index > rendered_buses &&
        live_buses_[index].load(std::memory_order_acquire) != nullptr;
    if (!reachable) {
      // Nowhere to fade out into: the next time this send has a bus it
      // starts from nothing rather than from wherever it was.
      state[0] = state[1] = 0.0f;
      continue;
    }
    if (level <= 0.0f && state[0] == 0.0f && state[1] == 0.0f) continue;

    mix_into(bus_ptrs_[index].data(), strip, width, frames, level, state);
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

  // The park fade. A parked graph keeps rendering until the fade has reached
  // zero, and only then stops running plugins: what a state load needs is
  // that nothing is inside process(), what the listener needs is that the
  // way there was not a step. Unparking is the same curve back up.
  const bool parked = parked_.load(std::memory_order_acquire);
  if (parked && park_gain_ <= 0.0f) {
    last_render_quiet_ = true;
    park_ramp_active_ = false;
    quiet_generation_.fetch_add(1, std::memory_order_release);
    render_generation_.fetch_add(1, std::memory_order_release);
    return;
  }
  last_render_quiet_ = false;
  {
    const float target = parked ? 0.0f : 1.0f;
    const float step = sample_rate_ > 0.0
        ? static_cast<float>(1.0 / (kParkFadeSeconds * sample_rate_))
        : 1.0f;
    park_ramp_active_ = park_gain_ != target || park_gain_ < 1.0f;
    if (park_ramp_active_) {
      const uint32_t n = std::min<uint32_t>(
          frames, static_cast<uint32_t>(park_ramp_.size()));
      for (uint32_t f = 0; f < n; ++f) {
        if (park_gain_ < target) park_gain_ = std::min(target, park_gain_ + step);
        else if (park_gain_ > target)
          park_gain_ = std::max(target, park_gain_ - step);
        park_ramp_[f] = park_gain_;
      }
    }
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
    if (live == nullptr) {
      if (tap_written_[i] > 0) {
        for (int p = 0; p < tap_written_[i]; ++p) {
          std::fill(tap_l_[i][p].begin(), tap_l_[i][p].end(), 0.0f);
          std::fill(tap_r_[i][p].begin(), tap_r_[i][p].end(), 0.0f);
        }
        tap_written_[i] = 0;
      }
      continue;  // removed channel
    }
    ChannelStrip& strip = *live;

    // Two channels wide is all the scratch there is, and a strip wider than
    // that would walk off the end of it.
    const int width = std::clamp(strip.channel_count(), 1, 2);
    AudioSource* source = live_sources_[i].load(std::memory_order_acquire);
    if (source != nullptr)
      source->read(scratch_ptrs_.data(), width, frames);
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
    if (MidiSource* midi_source =
            live_midi_sources_[i].load(std::memory_order_acquire)) {
      midi_count = midi_source->read(midi_scratch_.data(),
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
    for (int p = pairs; p < tap_written_[i]; ++p) {
      std::fill(tap_l_[i][p].begin(), tap_l_[i][p].end(), 0.0f);
      std::fill(tap_r_[i][p].begin(), tap_r_[i][p].end(), 0.0f);
    }
    tap_written_[i] = pairs;

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

    // Solo is a level, not a branch: a strip that solo leaves out fades to
    // nothing at its destination and comes back the same way. With only a
    // bus soloed, a strip still feeds that bus (as its destination or over
    // a send).
    const int destination = strip.destination();
    const bool feeds_bus =
        destination >= 0 && destination < kChannelDestination;
    const bool sends_audible = mix_channel || (!channel_solo && bus_solo);
    const bool dest_audible =
        mix_channel || (!channel_solo && bus_solo && feeds_bus);
    apply_sends(strip, width, frames, -1, i, sends_audible);
    float* state = channel_mix_[i].data();
    if (channel_last_dest_[i] != destination) {
      channel_last_dest_[i] = destination;
      state[0] = state[1] = 0.0f;
    }
    if (dest_audible || state[0] != 0.0f || state[1] != 0.0f) {
      mix_into(destination_for(destination, master,
                               static_cast<long long>(i), -1),
               strip, width, frames, dest_audible ? 1.0f : 0.0f, state);
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

    const int destination = live->destination();
    apply_sends(*live, 2, frames, static_cast<long long>(i), kMaxChannels + i,
                mix_bus);
    float* state = bus_mix_[i].data();
    if (bus_last_dest_[i] != destination) {
      bus_last_dest_[i] = destination;
      state[0] = state[1] = 0.0f;
    }
    if (mix_bus || state[0] != 0.0f || state[1] != 0.0f) {
      mix_into(destination_for(destination, master,
                               static_cast<long long>(kMaxChannels),
                               static_cast<long long>(i)),
               *live, 2, frames, mix_bus ? 1.0f : 0.0f, state);
    }
  }

  // Master: fader, dim and mute are one smoothed amount, mono is a smoothed
  // blend between the pair and their mid. Both walk across the block, never
  // step.
  {
    const bool mute = master_mute_.load(std::memory_order_relaxed);
    const float want =
        mute ? 0.0f
             : master_gain_.load(std::memory_order_relaxed) *
                   (master_dim_.load(std::memory_order_relaxed) ? 0.25f : 1.0f);
    const float gain_from = master_mix_;
    const float gain_to = slew(gain_from, want, frames);
    master_mix_ = gain_to;
    const float mono_from = master_mono_mix_;
    const float mono_to = slew(
        mono_from, master_mono_.load(std::memory_order_relaxed) ? 1.0f : 0.0f,
        frames);
    master_mono_mix_ = mono_to;

    const float gain_step = (gain_to - gain_from) / static_cast<float>(frames);
    const float mono_step = (mono_to - mono_from) / static_cast<float>(frames);
    float gain = gain_from;
    float mono = mono_from;
    for (uint32_t f = 0; f < frames; ++f) {
      gain += gain_step;
      mono += mono_step;
      const float left = master[0][f];
      const float right = master[1][f];
      const float mid = 0.5f * (left + right);
      master[0][f] = (left + (mid - left) * mono) * gain;
      master[1][f] = (right + (mid - right) * mono) * gain;
    }
  }

  run_limiter(master, frames);

  for (int ch = 0; ch < 2; ++ch) {
    float peak = 0.0f;
    for (uint32_t f = 0; f < frames; ++f)
      peak = std::max(peak, std::fabs(master[ch][f]));
    if (peak > master_peaks_[ch].load(std::memory_order_relaxed))
      master_peaks_[ch].store(peak, std::memory_order_relaxed);
    if (peak >= 1.0f) master_clip_.store(1.0f, std::memory_order_relaxed);
  }

  if (park_ramp_active_) {
    const uint32_t n = std::min<uint32_t>(
        frames, static_cast<uint32_t>(park_ramp_.size()));
    for (int ch = 0; ch < 2; ++ch)
      for (uint32_t f = 0; f < n; ++f) master[ch][f] *= park_ramp_[f];
  }

  record_master(master, frames);
  render_generation_.fetch_add(1, std::memory_order_release);
}

// A lookahead peak limiter. The gain the signal needs is computed a
// lookahead ahead of where the signal is heard, held at its deepest for the
// length of the window, then released; the gain actually applied is the
// window's running average of that, so a reduction arrives as a ramp that
// finishes exactly when the peak does. Nothing above the ceiling gets out,
// and nothing below it is touched.
void AudioGraph::run_limiter(float* const* master, uint32_t frames) {
  Limiter& lim = limiter_;
  const size_t n = lim.lookahead;
  if (n == 0) return;
  const bool on = master_limiter_.load(std::memory_order_relaxed);
  const float blend_from = lim.blend;
  const float blend_to = slew(blend_from, on ? 1.0f : 0.0f, frames);
  lim.blend = blend_to;
  // The delay line runs whether the limiter is on or not, so switching it
  // on blends towards a line that already holds the last millisecond and a
  // half of the music rather than silence or something stale. What is
  // skipped while off is the gain computer, so a reduction cannot be waiting
  // in the envelope when it comes on.
  const bool engaged = on || blend_from > 0.0f;
  const bool mixing = blend_from > 0.0f || blend_to > 0.0f;

  const float blend_step =
      (blend_to - blend_from) / static_cast<float>(frames);
  float blend = blend_from;
  float floor = 1.0f;
  const uint32_t hold = static_cast<uint32_t>(2 * n);
  for (uint32_t f = 0; f < frames; ++f) {
    blend += blend_step;
    const float in_l = master[0][f];
    const float in_r = master[1][f];
    if (engaged) {
      const float peak = std::max(std::fabs(in_l), std::fabs(in_r));
      const float need =
          peak > kLimiterCeiling ? kLimiterCeiling / peak : 1.0f;
      if (need < lim.envelope) {
        lim.envelope = need;
        lim.hold_left = hold;
      } else if (lim.hold_left > 0) {
        --lim.hold_left;
      } else {
        lim.envelope += (1.0f - lim.envelope) * (1.0f - lim.release_coeff);
      }
    } else {
      lim.envelope = 1.0f;
      lim.hold_left = 0;
    }

    // The window average: the oldest gain leaves as the newest enters.
    const size_t slot = lim.write;
    lim.window_sum += static_cast<double>(lim.envelope) -
                      static_cast<double>(lim.gains[slot]);
    lim.gains[slot] = lim.envelope;
    const float applied = static_cast<float>(
        std::min(1.0, lim.window_sum / static_cast<double>(n)));
    floor = std::min(floor, applied);

    const float out_l = lim.delay[0][slot] * applied;
    const float out_r = lim.delay[1][slot] * applied;
    lim.delay[0][slot] = in_l;
    lim.delay[1][slot] = in_r;
    lim.write = slot + 1 == n ? 0 : slot + 1;

    if (mixing) {
      master[0][f] = in_l + (out_l - in_l) * blend;
      master[1][f] = in_r + (out_r - in_r) * blend;
    }
  }
  if (engaged && floor < limiter_floor_.load(std::memory_order_relaxed))
    limiter_floor_.store(floor, std::memory_order_relaxed);
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

  // Whatever the previous occupant of this slot left behind is retired rather
  // than dropped here: a render pass that entered the slot before it was
  // removed can still be inside the old source's read().
  if (sources_[index] != nullptr || midi_sources_[index] != nullptr) {
    retired_sources_.push_back({std::move(sources_[index]),
                                std::move(midi_sources_[index]),
                                render_generation_.load(std::memory_order_acquire)});
  }
  sources_[index] = std::move(source);
  midi_sources_[index] = std::move(midi);

  // Published before the strip below, so a pass that can see the channel can
  // always see the source that feeds it.
  live_sources_[index].store(sources_[index].get(), std::memory_order_release);
  live_midi_sources_[index].store(midi_sources_[index].get(),
                                  std::memory_order_release);

  // Release twice over: the slot has to be visible before the count that
  // reaches it, or the audio thread could walk into a slot it cannot read yet.
  live_[index].store(raw, std::memory_order_release);
  const size_t used = active_.load(std::memory_order_relaxed);
  if (index >= used) active_.store(index + 1, std::memory_order_release);
  return index;
}

}  // namespace nirbija
