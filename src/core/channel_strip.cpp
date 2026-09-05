#include "core/channel_strip.h"

#include <algorithm>
#include <cmath>

namespace nirbija {

namespace {
// ~15 ms one-pole smoothing: fast enough to feel immediate, slow enough that a
// full fader throw does not step.
constexpr double kSmoothingSeconds = 0.015;
// A mute, and a bypass, take this long from all to nothing: a switch to the
// ear, a slope to the speaker.
constexpr double kMuteSeconds = 0.010;
}  // namespace

ChannelStrip::ChannelStrip(std::string name, int channel_count)
    : name_(std::move(name)),
      channel_count_(std::clamp(channel_count, 1, kMaxStripChannels)),
      peaks_(static_cast<size_t>(std::clamp(channel_count, 1, kMaxStripChannels))) {
  for (auto& peak : peaks_) peak.store(0.0f, std::memory_order_relaxed);
}

ChannelStrip::~ChannelStrip() = default;

void ChannelStrip::prepare(double sample_rate, uint32_t max_block_frames,
                           bool force_reactivate) {
  const bool first = sample_rate_ <= 0.0;
  const bool rate_changed = sample_rate_ > 0.0 && sample_rate_ != sample_rate;
  sample_rate_ = sample_rate;
  if (max_block_frames > max_block_frames_) max_block_frames_ = max_block_frames;
  smoothing_coeff_ =
      static_cast<float>(std::exp(-1.0 / (kSmoothingSeconds * sample_rate)));
  mute_step_ = static_cast<float>(1.0 / (kMuteSeconds * sample_rate));
  if (first) {
    smoothed_gain_ = gain_.load(std::memory_order_relaxed);
    smoothed_pan_ = pan_.load(std::memory_order_relaxed);
  }

  plugin_io_.assign(static_cast<size_t>(channel_count_), nullptr);
  output_cache_.assign(static_cast<size_t>(channel_count_),
                       std::vector<float>(max_block_frames_, 0.0f));
  delay_line_.assign(static_cast<size_t>(channel_count_),
                     std::vector<float>(max_block_frames_ + 192000, 0.0f));
  delay_write_.assign(static_cast<size_t>(channel_count_), 0);
  if (first || rate_changed || force_reactivate) {
    for (auto& insert : owned_inserts_) {
      insert->set_channel_layout(channel_count_);
      insert->activate(sample_rate, max_block_frames_);
    }
  }
}

bool ChannelStrip::midi_allowed(const MidiEvent& event, uint16_t mask) {
  if (event.size == 0) return false;
  const uint8_t status = event.data[0];
  if (status < 0x80 || status >= 0xf0) return true;  // clock etc. always pass
  const int channel = status & 0x0f;
  return (mask & (1u << channel)) != 0;
}

void ChannelStrip::run_insert(PluginInstance* insert, float* const* buffers,
                              uint32_t frames, const TransportInfo* transport,
                              bool filter_midi) {
  if (transport != nullptr) insert->set_transport(*transport);
  const uint16_t mask = midi_mask_.load(std::memory_order_relaxed);
  for (size_t e = 0; e < midi_chain_count_; ++e) {
    if (filter_midi && !midi_allowed(midi_chain_[e], mask)) continue;
    insert->queue_midi(midi_chain_[e]);
  }
  insert->process(buffers, buffers, frames);
  if (midi_chain_count_ < midi_chain_.size()) {
    midi_chain_count_ += insert->take_midi_output(
        midi_chain_.data() + midi_chain_count_,
        midi_chain_.size() - midi_chain_count_);
  }
}

void ChannelStrip::run_bypassed(PluginInstance* insert, float* const* buffers,
                                uint32_t frames,
                                const TransportInfo* transport) {
  if (transport != nullptr) insert->set_transport(*transport);
  for (size_t e = 0; e < midi_chain_count_; ++e)
    insert->queue_midi(midi_chain_[e]);

  // Running the insert is only safe if the dry can be put back over whatever
  // it writes. Without room to stash it - a block wider than the cache was
  // sized for - the insert does not get to run at all, because processing and
  // then failing to restore is exactly the audio leak a bypass is meant to
  // stop.
  const bool can_stash =
      !output_cache_.empty() &&
      static_cast<int>(output_cache_.size()) >= channel_count_ &&
      frames <= output_cache_[0].size();
  if (can_stash) {
    for (int ch = 0; ch < channel_count_; ++ch)
      std::copy_n(buffers[ch], frames, output_cache_[ch].data());
    insert->process(buffers, buffers, frames);
    for (int ch = 0; ch < channel_count_; ++ch)
      std::copy_n(output_cache_[ch].data(), frames, buffers[ch]);
  }

  // Either way the queued events have to come back out, or a bypassed plugin
  // sits on a note until it is switched back in.
  MidiEvent dump[32];
  while (insert->take_midi_output(dump, 32) == 32) {
  }
}

void ChannelStrip::run_crossfade(PluginInstance* insert, float* const* buffers,
                                 uint32_t frames, const TransportInfo* transport,
                                 float* mix, bool to_bypass) {
  const bool can_stash =
      !output_cache_.empty() &&
      static_cast<int>(output_cache_.size()) >= channel_count_ &&
      frames <= output_cache_[0].size();
  if (!can_stash) {
    // No room to keep the dry: land where the flag says and run it plainly.
    *mix = to_bypass ? 1.0f : 0.0f;
    if (to_bypass) run_bypassed(insert, buffers, frames, transport);
    else run_insert(insert, buffers, frames, transport, false);
    return;
  }

  if (transport != nullptr) insert->set_transport(*transport);
  for (size_t e = 0; e < midi_chain_count_; ++e)
    insert->queue_midi(midi_chain_[e]);

  for (int ch = 0; ch < channel_count_; ++ch)
    std::copy_n(buffers[ch], frames, output_cache_[ch].data());
  insert->process(buffers, buffers, frames);

  const float from = *mix;
  const float target = to_bypass ? 1.0f : 0.0f;
  const float most = mute_step_ * static_cast<float>(frames);
  const float to = std::clamp(target, from - most, from + most);
  *mix = to;
  const float step = (to - from) / static_cast<float>(frames);
  for (int ch = 0; ch < channel_count_; ++ch) {
    const float* dry = output_cache_[ch].data();
    float* wet = buffers[ch];
    float amount = from;
    for (uint32_t f = 0; f < frames; ++f) {
      amount += step;
      wet[f] += (dry[f] - wet[f]) * amount;
    }
  }

  // The notes it made follow where it is going: kept while it comes back
  // in, dropped while it goes out, the same as either settled state.
  if (to_bypass) {
    MidiEvent dump[32];
    while (insert->take_midi_output(dump, 32) == 32) {
    }
  } else if (midi_chain_count_ < midi_chain_.size()) {
    midi_chain_count_ += insert->take_midi_output(
        midi_chain_.data() + midi_chain_count_,
        midi_chain_.size() - midi_chain_count_);
  }
}

bool ChannelStrip::snapshot_chain(ChainSnapshot* out) const {
  // Four attempts is generous: the writer's window is a handful of stores, and
  // failing every time means the UI thread was descheduled inside one of them.
  for (int attempt = 0; attempt < 4; ++attempt) {
    const uint32_t before = chain_seq_.load(std::memory_order_acquire);
    if ((before & 1u) != 0) continue;  // an edit is in progress

    out->count = insert_count_.load(std::memory_order_acquire);
    if (out->count > kMaxInserts) out->count = kMaxInserts;
    for (size_t i = 0; i < out->count; ++i) {
      out->inserts[i] = insert_slots_[i].load(std::memory_order_acquire);
      out->flags[i] = insert_flags_[i].load(std::memory_order_acquire);
    }

    if (chain_seq_.load(std::memory_order_acquire) == before) return true;
  }
  out->count = 0;
  return false;
}

void ChannelStrip::process(float* const* buffers, uint32_t frames,
                           const MidiEvent* midi, size_t midi_count,
                           const TransportInfo* transport) {
  const uint16_t mask = midi_mask_.load(std::memory_order_relaxed);
  midi_chain_count_ = 0;
  for (size_t i = 0; i < midi_count && midi_chain_count_ < midi_chain_.size();
       ++i) {
    if (!midi_allowed(midi[i], mask)) continue;
    midi_chain_[midi_chain_count_++] = midi[i];
  }

  for (int ch = 0; ch < channel_count_; ++ch) plugin_io_[ch] = buffers[ch];

  // The whole chain, taken at one instant. Reading each slot as the walk
  // reaches it let a reorder land halfway through and run one plugin twice.
  ChainSnapshot chain;
  snapshot_chain(&chain);
  const size_t insert_count = chain.count;
  auto flags_of = [&chain](size_t i) { return chain.flags[i]; };

  // Pre-fader inserts first, then the fader, then post-fader. Bypass still
  // delivers MIDI so a muted-style hang cannot happen on a bypassed synth.
  // A slot whose bypass just flipped spends the next few blocks walking
  // between wet and dry rather than jumping.
  auto run_slot = [&](size_t i) {
    PluginInstance* insert = chain.inserts[i];
    if (insert == nullptr) return;
    const bool bypass = (flags_of(i) & kBypass) != 0;
    float mix = bypass_mix_[i].load(std::memory_order_relaxed);
    const float settled = bypass ? 1.0f : 0.0f;
    if (mix != settled) {
      run_crossfade(insert, plugin_io_.data(), frames, transport, &mix, bypass);
      bypass_mix_[i].store(mix, std::memory_order_relaxed);
      return;
    }
    if (bypass) run_bypassed(insert, buffers, frames, transport);
    else run_insert(insert, plugin_io_.data(), frames, transport, false);
  };

  for (size_t i = 0; i < insert_count; ++i)
    if ((flags_of(i) & kPostFader) == 0) run_slot(i);

  {
    const float target_gain = gain_.load(std::memory_order_relaxed);
    const float target_pan = pan_.load(std::memory_order_relaxed);
    const float mute_target = muted_.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
    const bool stereo = channel_count_ == 2;
    for (uint32_t i = 0; i < frames; ++i) {
      smoothed_gain_ += (1.0f - smoothing_coeff_) * (target_gain - smoothed_gain_);
      smoothed_pan_ += (1.0f - smoothing_coeff_) * (target_pan - smoothed_pan_);
      if (mute_gain_ < mute_target)
        mute_gain_ = std::min(mute_target, mute_gain_ + mute_step_);
      else if (mute_gain_ > mute_target)
        mute_gain_ = std::max(mute_target, mute_gain_ - mute_step_);
      const float left = stereo ? std::min(1.0f, 1.0f - smoothed_pan_) : 1.0f;
      const float right = stereo ? std::min(1.0f, 1.0f + smoothed_pan_) : 1.0f;
      const float gain = smoothed_gain_ * mute_gain_;
      for (int ch = 0; ch < channel_count_; ++ch) {
        const float pan_gain = (ch == 0) ? left : right;
        buffers[ch][i] *= gain * pan_gain;
      }
    }
  }

  for (size_t i = 0; i < insert_count; ++i)
    if ((flags_of(i) & kPostFader) != 0) run_slot(i);

  apply_pdc(buffers, frames);

  for (int ch = 0; ch < channel_count_; ++ch) {
    if (ch < static_cast<int>(output_cache_.size()) &&
        frames <= output_cache_[ch].size())
      std::copy_n(buffers[ch], frames, output_cache_[ch].data());
    float peak = 0.0f;
    for (uint32_t i = 0; i < frames; ++i)
      peak = std::max(peak, std::fabs(buffers[ch][i]));
    float previous = peaks_[ch].load(std::memory_order_relaxed);
    if (peak > previous) peaks_[ch].store(peak, std::memory_order_relaxed);
  }
  process_generation_.fetch_add(1, std::memory_order_release);
}

float ChannelStrip::read_peak(int channel) {
  if (channel < 0 || channel >= static_cast<int>(peaks_.size())) return 0.0f;
  return peaks_[channel].exchange(0.0f, std::memory_order_relaxed);
}

bool ChannelStrip::add_insert(std::unique_ptr<PluginInstance> plugin,
                              size_t* placed_at) {
  if (plugin == nullptr) return false;

  // A removal leaves a null slot so the others keep their index; the next add
  // fills the first such hole rather than growing past it, or a chain that had
  // something removed could only ever grow downwards.
  const size_t count = insert_count_.load(std::memory_order_relaxed);
  size_t index = count;
  for (size_t i = 0; i < count; ++i) {
    if (insert_slots_[i].load(std::memory_order_relaxed) == nullptr) {
      index = i;
      break;
    }
  }
  if (index >= kMaxInserts) return false;

  plugin->set_channel_layout(channel_count_);
  if (sample_rate_ > 0.0 && !plugin->activate(sample_rate_, max_block_frames_))
    return false;

  PluginInstance* raw = plugin.get();
  owned_inserts_.push_back(std::move(plugin));

  // Publish the slot before the count, so the audio thread can never see a
  // count that reaches a slot it cannot read yet.
  begin_chain_edit();
  // A removal leaves the previous occupant's bypass/post-fader bits. A
  // new plugin in that hole is not the old one and should start clean.
  insert_flags_[index].store(0, std::memory_order_release);
  bypass_mix_[index].store(0.0f, std::memory_order_relaxed);
  insert_slots_[index].store(raw, std::memory_order_release);
  if (index == count) insert_count_.store(count + 1, std::memory_order_release);
  end_chain_edit();
  if (placed_at != nullptr) *placed_at = index;
  return true;
}

void ChannelStrip::remove_insert(size_t index) {
  if (index >= insert_count_.load(std::memory_order_relaxed)) return;
  begin_chain_edit();
  PluginInstance* raw = insert_slots_[index].exchange(nullptr, std::memory_order_release);
  insert_flags_[index].store(0, std::memory_order_release);
  end_chain_edit();
  if (raw == nullptr) return;

  // The audio thread may be inside this plugin right now, so it is retired
  // rather than destroyed, and is left activated for the same reason.
  auto it = std::find_if(owned_inserts_.begin(), owned_inserts_.end(),
                         [raw](const auto& owned) { return owned.get() == raw; });
  if (it != owned_inserts_.end()) {
    retired_.push_back({std::move(*it),
                        process_generation_.load(std::memory_order_acquire)});
    owned_inserts_.erase(it);
  }
}

void ChannelStrip::reclaim(bool audio_running) {
  // With no audio thread at all there is nothing that could still be inside a
  // retired insert, so the generation gate has nothing to wait for - and it
  // would wait forever: the generation only advances at the end of a render,
  // which the JACK callback drives. The app stays fully usable with no audio
  // server (the status bar says "no audio server - start PipeWire or JACK and
  // restart"), so without this every insert removed there is held until exit.
  //
  // The test cannot be "generation == 0": one created while audio is already
  // running also sits at 0 until its first block, and the audio thread may be
  // inside it by then. Only the absence of the JACK client settles it, which is
  // what the caller passes in.
  if (!audio_running) {
    retired_.clear();
    return;
  }

  const uint64_t now = process_generation_.load(std::memory_order_acquire);
  std::erase_if(retired_, [now](const RetiredInsert& item) {
    return now >= item.generation + 2;
  });
}

void ChannelStrip::swap_inserts(size_t a, size_t b) {
  const size_t count = insert_count_.load(std::memory_order_relaxed);
  if (a >= count || b >= count || a == b) return;

  PluginInstance* first = insert_slots_[a].load(std::memory_order_relaxed);
  PluginInstance* second = insert_slots_[b].load(std::memory_order_relaxed);

  // The pair of stores is not atomic on its own; the sequence counter is what
  // makes the audio thread take both or neither. The flags travel with the
  // plugin, or a reordered insert would keep the neighbour's bypass.
  const uint8_t first_flags = insert_flags_[a].load(std::memory_order_relaxed);
  const uint8_t second_flags = insert_flags_[b].load(std::memory_order_relaxed);
  const float first_mix = bypass_mix_[a].load(std::memory_order_relaxed);
  const float second_mix = bypass_mix_[b].load(std::memory_order_relaxed);

  begin_chain_edit();
  insert_slots_[a].store(second, std::memory_order_release);
  insert_slots_[b].store(first, std::memory_order_release);
  insert_flags_[a].store(second_flags, std::memory_order_release);
  insert_flags_[b].store(first_flags, std::memory_order_release);
  bypass_mix_[a].store(second_mix, std::memory_order_relaxed);
  bypass_mix_[b].store(first_mix, std::memory_order_relaxed);
  end_chain_edit();
}

PluginInstance* ChannelStrip::insert_at(size_t index) const {
  if (index >= insert_count_.load(std::memory_order_acquire)) return nullptr;
  return insert_slots_[index].load(std::memory_order_acquire);
}

void ChannelStrip::set_insert_bypassed(size_t index, bool on) {
  if (index >= kMaxInserts) return;
  uint8_t flags = insert_flags_[index].load(std::memory_order_relaxed);
  if (on) flags |= kBypass;
  else flags = static_cast<uint8_t>(flags & ~kBypass);
  insert_flags_[index].store(flags, std::memory_order_release);
}

bool ChannelStrip::insert_bypassed(size_t index) const {
  if (index >= kMaxInserts) return false;
  return (insert_flags_[index].load(std::memory_order_acquire) & kBypass) != 0;
}

void ChannelStrip::set_insert_post_fader(size_t index, bool on) {
  if (index >= kMaxInserts) return;
  uint8_t flags = insert_flags_[index].load(std::memory_order_relaxed);
  if (on) flags |= kPostFader;
  else flags = static_cast<uint8_t>(flags & ~kPostFader);
  insert_flags_[index].store(flags, std::memory_order_release);
}

bool ChannelStrip::insert_post_fader(size_t index) const {
  if (index >= kMaxInserts) return false;
  return (insert_flags_[index].load(std::memory_order_acquire) & kPostFader) != 0;
}

uint32_t ChannelStrip::latency_samples() const {
  uint32_t total = 0;
  const size_t count = insert_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < count; ++i) {
    if (insert_bypassed(i)) continue;
    PluginInstance* insert = insert_slots_[i].load(std::memory_order_acquire);
    if (insert != nullptr) total += insert->latency_samples();
  }
  return total;
}

int ChannelStrip::extra_output_pairs() const {
  const size_t count = insert_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < count; ++i) {
    PluginInstance* insert = insert_slots_[i].load(std::memory_order_acquire);
    if (insert == nullptr || insert_bypassed(i)) continue;
    const int pairs = insert->extra_output_pairs();
    if (pairs > 0) return pairs;
  }
  return 0;
}

void ChannelStrip::copy_extra_output(int pair, float* left, float* right,
                                     uint32_t frames) const {
  const size_t count = insert_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < count; ++i) {
    PluginInstance* insert = insert_slots_[i].load(std::memory_order_acquire);
    if (insert == nullptr || insert_bypassed(i)) continue;
    if (insert->extra_output_pairs() <= 0) continue;
    insert->copy_extra_output(pair, left, right, frames);
    return;
  }
  std::fill_n(left, frames, 0.0f);
  std::fill_n(right, frames, 0.0f);
}

const float* ChannelStrip::output_cache(int channel) const {
  if (channel < 0 || channel >= static_cast<int>(output_cache_.size()))
    return nullptr;
  return output_cache_[channel].data();
}

void ChannelStrip::feed_sidechain(const float* const* buffers, int channels,
                                  uint32_t frames) {
  const size_t count = insert_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < count; ++i) {
    PluginInstance* insert = insert_slots_[i].load(std::memory_order_acquire);
    if (insert != nullptr) insert->set_sidechain(buffers, channels, frames);
  }
}

void ChannelStrip::apply_pdc(float* const* buffers, uint32_t frames) {
  const uint32_t delay = pdc_delay_.load(std::memory_order_relaxed);
  if (delay == 0 || delay_line_.empty()) return;
  for (int ch = 0; ch < channel_count_ && ch < static_cast<int>(delay_line_.size());
       ++ch) {
    auto& line = delay_line_[ch];
    if (line.size() <= delay) continue;
    size_t write = delay_write_[ch];
    for (uint32_t i = 0; i < frames; ++i) {
      const size_t read = (write + line.size() - delay) % line.size();
      const float incoming = buffers[ch][i];
      buffers[ch][i] = line[read];
      line[write] = incoming;
      write = (write + 1) % line.size();
    }
    delay_write_[ch] = write;
  }
}

}  // namespace nirbija
