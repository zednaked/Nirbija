// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "core/channel_strip.h"

#include <algorithm>
#include <cmath>

#include "core/dsp.h"
#include "core/midi_out.h"

namespace nirbija {

namespace {
// ~15 ms one-pole smoothing: fast enough to feel immediate, slow enough that a
// full fader throw does not step.
constexpr double kSmoothingSeconds = 0.015;
// A mute, and a bypass, take this long from all to nothing: a switch to the
// ear, a slope to the speaker.
constexpr double kMuteSeconds = 0.010;
// The least a change of compensation delay takes to cross from the old read
// position to the new one. A block longer than this fades over the whole
// block; a shorter one carries the fade into the next.
constexpr double kPdcFadeSeconds = 0.005;
// The longest compensation the delay line holds, on top of one block.
constexpr size_t kPdcMaxSamples = 192000;
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

  // Grow, never shrink, and never reallocate what is already big enough: a
  // buffer-size callback is not a place to throw away a delay line that
  // holds the last seconds of the music. The delay line in particular is
  // 192000 floats per channel and used to be reallocated on every call.
  plugin_io_.assign(static_cast<size_t>(channel_count_), nullptr);
  if (output_cache_.size() != static_cast<size_t>(channel_count_))
    output_cache_.resize(static_cast<size_t>(channel_count_));
  for (auto& cache : output_cache_)
    if (cache.size() < max_block_frames_) cache.assign(max_block_frames_, 0.0f);
  for (auto& curve : gain_curve_)
    if (curve.size() < max_block_frames_) curve.assign(max_block_frames_, 1.0f);
  if (delay_line_.size() != static_cast<size_t>(channel_count_)) {
    delay_line_.assign(static_cast<size_t>(channel_count_), {});
    delay_write_.assign(static_cast<size_t>(channel_count_), 0);
  }
  const size_t line_size = max_block_frames_ + kPdcMaxSamples;
  for (size_t ch = 0; ch < delay_line_.size(); ++ch) {
    if (delay_line_[ch].size() < line_size) {
      delay_line_[ch].assign(line_size, 0.0f);
      delay_write_[ch] = 0;
    }
  }

  // Plugins are activated with the largest block they will ever be handed.
  // A period that grows past that has to activate them again: CLAP and VST3
  // plugins allocate from the number they were given and would run off the
  // end of their own buffers. Process is stopped whenever prepare() runs, so
  // the deactivate/activate pair cannot tear a block.
  const bool grew = max_block_frames_ > activated_block_frames_;
  if (first || rate_changed || force_reactivate || grew) {
    for (auto& insert : owned_inserts_) {
      if (activated_block_frames_ > 0) insert->deactivate();
      insert->set_channel_layout(channel_count_);
      insert->activate(sample_rate, max_block_frames_);
    }
    activated_block_frames_ = max_block_frames_;
  }
}

bool ChannelStrip::midi_allowed(const MidiEvent& event, uint16_t mask) {
  if (event.size == 0) return false;
  const uint8_t status = event.data[0];
  if (status < 0x80 || status >= 0xf0) return true;  // clock etc. always pass
  const int channel = status & 0x0f;
  return (mask & (1u << channel)) != 0;
}

void ChannelStrip::queue_chain_midi(PluginInstance* insert) {
  if (midi_chain_unsorted_) {
    sort_midi_by_frame(midi_chain_.data(), midi_chain_count_);
    midi_chain_unsorted_ = false;
  }
  for (size_t e = 0; e < midi_chain_count_; ++e) insert->queue_midi(midi_chain_[e]);
}

void ChannelStrip::append_insert_midi(PluginInstance* insert) {
  if (midi_chain_count_ >= midi_chain_.size()) return;
  const size_t added = insert->take_midi_output(
      midi_chain_.data() + midi_chain_count_,
      midi_chain_.size() - midi_chain_count_);
  midi_chain_count_ += added;
  if (added > 0) midi_chain_unsorted_ = true;
}

void ChannelStrip::run_insert(PluginInstance* insert, float* const* buffers,
                              uint32_t frames, const TransportInfo* transport) {
  if (transport != nullptr) insert->set_transport(*transport);
  queue_chain_midi(insert);
  insert->process(buffers, buffers, frames);
  append_insert_midi(insert);
}

void ChannelStrip::run_bypassed(PluginInstance* insert, float* const* buffers,
                                uint32_t frames,
                                const TransportInfo* transport) {
  if (transport != nullptr) insert->set_transport(*transport);
  queue_chain_midi(insert);

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
    else run_insert(insert, buffers, frames, transport);
    return;
  }

  if (transport != nullptr) insert->set_transport(*transport);
  queue_chain_midi(insert);

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
  } else {
    append_insert_midi(insert);
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
      out->tags[i] = insert_tags_[i].load(std::memory_order_acquire);
    }

    if (chain_seq_.load(std::memory_order_acquire) == before) return true;
  }
  out->count = 0;
  return false;
}

void ChannelStrip::reconcile_bypass_mix(const ChainSnapshot& chain) {
  // The common block: the same tags in the same slots as last time. Tags,
  // not pointers: see insert_tags_.
  bool same = true;
  for (size_t i = 0; i < kMaxInserts && same; ++i) {
    const uint32_t now = i < chain.count && chain.inserts[i] != nullptr ? chain.tags[i] : 0;
    same = now == bypass_mix_tag_[i];
  }
  if (same) return;

  // Something moved. Each slot's mix is looked up by the plugin that now
  // sits there: found in an old slot, it comes along (a swap); not found, the
  // plugin is new and starts settled where its flags say, wet unless it was
  // published bypassed.
  std::array<float, kMaxInserts> mix{};
  std::array<uint32_t, kMaxInserts> tags{};
  for (size_t i = 0; i < kMaxInserts; ++i) {
    const uint32_t now = i < chain.count && chain.inserts[i] != nullptr ? chain.tags[i] : 0;
    tags[i] = now;
    if (now == 0) continue;
    bool found = false;
    for (size_t j = 0; j < kMaxInserts; ++j) {
      if (bypass_mix_tag_[j] == now) {
        mix[i] = bypass_mix_[j];
        found = true;
        break;
      }
    }
    if (!found) mix[i] = (chain.flags[i] & kBypass) != 0 ? 1.0f : 0.0f;
  }
  bypass_mix_ = mix;
  bypass_mix_tag_ = tags;
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
  // The port's events came in order but the graph appends injected ones
  // after them; the first insert to ask sorts the lot.
  midi_chain_unsorted_ = midi_chain_count_ > 1;

  for (int ch = 0; ch < channel_count_; ++ch) plugin_io_[ch] = buffers[ch];

  // The whole chain, taken at one instant. Reading each slot as the walk
  // reaches it let a reorder land halfway through and run one plugin twice.
  // A torn read falls back on the last whole one for a single block; see
  // last_good_ for why not longer.
  ChainSnapshot chain;
  if (snapshot_chain(&chain)) {
    last_good_ = chain;
    stale_snapshot_blocks_ = 0;
  } else if (stale_snapshot_blocks_ == 0) {
    chain = last_good_;
    stale_snapshot_blocks_ = 1;
  } else {
    chain.count = 0;
  }
  reconcile_bypass_mix(chain);
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
    float mix = bypass_mix_[i];
    const float settled = bypass ? 1.0f : 0.0f;
    if (mix != settled) {
      run_crossfade(insert, plugin_io_.data(), frames, transport, &mix, bypass);
      bypass_mix_[i] = mix;
      return;
    }
    if (bypass) run_bypassed(insert, buffers, frames, transport);
    else run_insert(insert, plugin_io_.data(), frames, transport);
  };

  for (size_t i = 0; i < insert_count; ++i)
    if ((flags_of(i) & kPostFader) == 0) run_slot(i);

  {
    // The fader: the gain curve for the block is written first, one value
    // per sample per side, then multiplied into the audio. Splitting the two
    // keeps the smoother's serial recurrence out of the multiply loop, which
    // is then a plain product the compiler vectorises.
    const float target_gain = gain_.load(std::memory_order_relaxed);
    const float target_pan = pan_.load(std::memory_order_relaxed);
    const float mute_target = muted_.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
    const bool stereo = channel_count_ == 2;
    const bool have_curve = frames <= gain_curve_[0].size() &&
                            (!stereo || frames <= gain_curve_[1].size());
    float* curve_l = have_curve ? gain_curve_[0].data() : nullptr;
    float* curve_r = have_curve && stereo ? gain_curve_[1].data() : nullptr;
    for (uint32_t i = 0; i < frames; ++i) {
      smoothed_gain_ += (1.0f - smoothing_coeff_) * (target_gain - smoothed_gain_);
      smoothed_pan_ += (1.0f - smoothing_coeff_) * (target_pan - smoothed_pan_);
      if (mute_gain_ < mute_target)
        mute_gain_ = std::min(mute_target, mute_gain_ + mute_step_);
      else if (mute_gain_ > mute_target)
        mute_gain_ = std::max(mute_target, mute_gain_ - mute_step_);
      const float left = stereo ? std::min(1.0f, 1.0f - smoothed_pan_) : 1.0f;
      const float right = stereo ? std::min(1.0f, 1.0f + smoothed_pan_) : 1.0f;
      if (scene_delay_ > 0) {
        --scene_delay_;
      } else if (scene_pos_ < scene_len_) {
        ++scene_pos_;
        const float t = static_cast<float>(scene_pos_) / static_cast<float>(scene_len_);
        const float shape = 0.5f - 0.5f * std::cos(3.14159265358979f * t);
        scene_gain_ = scene_pos_ == scene_len_
                          ? scene_to_
                          : scene_from_ + (scene_to_ - scene_from_) * shape;
      }
      const float gain = smoothed_gain_ * mute_gain_ * scene_gain_;
      if (have_curve) {
        curve_l[i] = gain * left;
        if (stereo) curve_r[i] = gain * right;
      } else {
        // A block wider than the curve was sized for: the slow way, in place.
        for (int ch = 0; ch < channel_count_; ++ch)
          buffers[ch][i] *= gain * ((ch == 0) ? left : right);
      }
    }
    if (have_curve) {
      for (int ch = 0; ch < channel_count_; ++ch) {
        const float* curve = ch == 0 ? curve_l : curve_r;
        float* audio = buffers[ch];
        for (uint32_t i = 0; i < frames; ++i) audio[i] *= curve[i];
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
  // new plugin in that hole is not the old one and should start clean; the
  // audio thread sees a new pointer in the slot and starts its mix wet.
  insert_flags_[index].store(0, std::memory_order_release);
  if (next_insert_tag_ == 0) next_insert_tag_ = 1;  // never a hole's tag
  insert_tags_[index].store(next_insert_tag_++, std::memory_order_release);
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
  insert_tags_[index].store(0, std::memory_order_release);
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
  // plugin, or a reordered insert would keep the neighbour's bypass. The
  // bypass mix travels too, but the audio thread moves it itself when it
  // sees the pointers change slots (reconcile_bypass_mix).
  const uint8_t first_flags = insert_flags_[a].load(std::memory_order_relaxed);
  const uint8_t second_flags = insert_flags_[b].load(std::memory_order_relaxed);
  const uint32_t first_tag = insert_tags_[a].load(std::memory_order_relaxed);
  const uint32_t second_tag = insert_tags_[b].load(std::memory_order_relaxed);

  begin_chain_edit();
  insert_slots_[a].store(second, std::memory_order_release);
  insert_slots_[b].store(first, std::memory_order_release);
  insert_flags_[a].store(second_flags, std::memory_order_release);
  insert_flags_[b].store(first_flags, std::memory_order_release);
  insert_tags_[a].store(second_tag, std::memory_order_release);
  insert_tags_[b].store(first_tag, std::memory_order_release);
  end_chain_edit();
}

void ChannelStrip::start_scene_gate(bool on, uint32_t delay_frames,
                                    uint32_t length_samples) {
  scene_on_.store(on, std::memory_order_relaxed);
  scene_from_ = scene_gain_;
  scene_to_ = on ? 1.0f : 0.0f;
  scene_delay_ = delay_frames;
  scene_pos_ = 0;
  scene_len_ = std::max<uint32_t>(1, length_samples);
}

void ChannelStrip::set_scene_on(bool on) {
  const auto length = static_cast<uint32_t>(
      std::max(1.0, kMuteSeconds * (sample_rate_ > 0.0 ? sample_rate_ : 48000.0)));
  start_scene_gate(on, 0, length);
}

PluginInstance* ChannelStrip::insert_by_tag(uint32_t tag) const {
  if (tag == 0) return nullptr;
  // Under the chain's sequence counter, the same as a render: a swap caught
  // halfway would pair one plugin's tag with the other's slot.
  ChainSnapshot chain;
  if (!snapshot_chain(&chain)) return nullptr;
  for (size_t i = 0; i < chain.count; ++i)
    if (chain.tags[i] == tag) return chain.inserts[i];
  return nullptr;
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

// The compensation delay. The line is written every block whatever the delay,
// so when the delay changes the new read position holds real audio. A change
// crossfades linearly from the old read position to the new one over
// max(block, kPdcFadeSeconds): the two are the same signal a few milliseconds
// apart, so a linear cross keeps the level where an equal-power one would
// bump it. A change that arrives mid-fade waits for the fade to end rather
// than restarting from a mix that has no single read position to start from.
void ChannelStrip::apply_pdc(float* const* buffers, uint32_t frames) {
  if (delay_line_.empty() || frames == 0) return;
  const size_t size = delay_line_[0].size();
  if (size == 0) return;

  const uint32_t wanted =
      std::min<uint32_t>(pdc_delay_.load(std::memory_order_relaxed),
                         static_cast<uint32_t>(size - 1));
  if (pdc_fade_left_ == 0 && wanted != pdc_current_) {
    pdc_previous_ = pdc_current_;
    pdc_current_ = wanted;
    const uint32_t least = sample_rate_ > 0.0
        ? static_cast<uint32_t>(kPdcFadeSeconds * sample_rate_)
        : frames;
    pdc_fade_length_ = std::max<uint32_t>(1, std::max(frames, least));
    pdc_fade_left_ = pdc_fade_length_;
  }

  // Nothing to delay and nothing to fade: only keep the line current.
  const bool fading = pdc_fade_left_ > 0;
  if (!fading && pdc_current_ == 0) {
    for (int ch = 0; ch < channel_count_ && ch < static_cast<int>(delay_line_.size());
         ++ch) {
      float* line = delay_line_[ch].data();
      size_t write = delay_write_[ch];
      for (uint32_t i = 0; i < frames; ++i) {
        line[write] = buffers[ch][i];
        write = dsp::ring_next(write, size);
      }
      delay_write_[ch] = write;
    }
    return;
  }

  const size_t current = pdc_current_;
  const size_t previous = pdc_previous_;
  const float fade_step = 1.0f / static_cast<float>(pdc_fade_length_);
  uint32_t fade_left_after = pdc_fade_left_;
  for (int ch = 0; ch < channel_count_ && ch < static_cast<int>(delay_line_.size());
       ++ch) {
    float* line = delay_line_[ch].data();
    size_t write = delay_write_[ch];
    // Written first, then read: a delay of zero reads back the sample just
    // written, and any other reads what was written that many samples ago.
    size_t read_now = dsp::ring_back(write, current, size);
    size_t read_old = dsp::ring_back(write, previous, size);
    uint32_t left = pdc_fade_left_;
    // How far the fade had got at the start of this block, 0 old to 1 new.
    float amount = 1.0f - static_cast<float>(left) * fade_step;
    for (uint32_t i = 0; i < frames; ++i) {
      line[write] = buffers[ch][i];
      const float now = line[read_now];
      if (left > 0) {
        amount += fade_step;
        --left;
        const float old = line[read_old];
        buffers[ch][i] = old + (now - old) * amount;
      } else {
        buffers[ch][i] = now;
      }
      write = dsp::ring_next(write, size);
      read_now = dsp::ring_next(read_now, size);
      read_old = dsp::ring_next(read_old, size);
    }
    delay_write_[ch] = write;
    fade_left_after = left;
  }
  pdc_fade_left_ = fade_left_after;
}

}  // namespace nirbija
