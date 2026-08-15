#include "core/channel_strip.h"

#include <algorithm>
#include <cmath>

namespace nirbija {

namespace {
// ~15 ms one-pole smoothing: fast enough to feel immediate, slow enough that a
// full fader throw does not step.
constexpr double kSmoothingSeconds = 0.015;
}  // namespace

ChannelStrip::ChannelStrip(std::string name, int channel_count)
    : name_(std::move(name)), channel_count_(channel_count), peaks_(channel_count) {
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
  if (first) {
    smoothed_gain_ = gain_.load(std::memory_order_relaxed);
    smoothed_pan_ = pan_.load(std::memory_order_relaxed);
  }

  plugin_io_.assign(static_cast<size_t>(channel_count_), nullptr);
  if (first || rate_changed || force_reactivate) {
    for (auto& insert : owned_inserts_) {
      insert->set_channel_layout(channel_count_);
      insert->activate(sample_rate, max_block_frames_);
    }
  }
}

void ChannelStrip::process(float* const* buffers, uint32_t frames,
                           const MidiEvent* midi, size_t midi_count,
                           const TransportInfo* transport) {
  // The block's MIDI starts as whatever came in on the channel port and grows
  // as inserts produce their own.
  midi_chain_count_ = std::min(midi_count, midi_chain_.size());
  for (size_t i = 0; i < midi_chain_count_; ++i) midi_chain_[i] = midi[i];

  // Inserts always run, even while muted: a synth that missed a note-off
  // because someone hit mute would hang that note forever, and a reverb
  // would lose its tail. Mute is applied to the audio after the chain.
  // TODO: per-slot pre/post, so an effect can be placed after the fader.
  for (int ch = 0; ch < channel_count_; ++ch) plugin_io_[ch] = buffers[ch];
  const size_t insert_count = insert_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < insert_count; ++i) {
    PluginInstance* insert = insert_slots_[i].load(std::memory_order_acquire);
    if (insert == nullptr) continue;

    if (transport != nullptr) insert->set_transport(*transport);

    // Everything the chain has produced so far reaches this insert, so a step
    // sequencer sitting above a synth actually plays it.
    for (size_t e = 0; e < midi_chain_count_; ++e)
      insert->queue_midi(midi_chain_[e]);

    insert->process(plugin_io_.data(), plugin_io_.data(), frames);

    // Whatever it emitted joins the stream for the inserts below it.
    if (midi_chain_count_ < midi_chain_.size()) {
      midi_chain_count_ += insert->take_midi_output(
          midi_chain_.data() + midi_chain_count_,
          midi_chain_.size() - midi_chain_count_);
    }
  }

  if (muted_.load(std::memory_order_relaxed)) {
    for (int ch = 0; ch < channel_count_; ++ch)
      std::fill_n(buffers[ch], frames, 0.0f);
    process_generation_.fetch_add(1, std::memory_order_release);
    return;
  }

  const float target_gain = gain_.load(std::memory_order_relaxed);
  const float target_pan = pan_.load(std::memory_order_relaxed);
  const bool stereo = channel_count_ == 2;

  for (uint32_t i = 0; i < frames; ++i) {
    smoothed_gain_ += (1.0f - smoothing_coeff_) * (target_gain - smoothed_gain_);
    smoothed_pan_ += (1.0f - smoothing_coeff_) * (target_pan - smoothed_pan_);

    // On a stereo strip pan is a balance: unity at centre, attenuating the side
    // you turn away from. A mono strip is panned by the graph as it widens,
    // where constant-power is the right law.
    const float left = stereo ? std::min(1.0f, 1.0f - smoothed_pan_) : 1.0f;
    const float right = stereo ? std::min(1.0f, 1.0f + smoothed_pan_) : 1.0f;

    for (int ch = 0; ch < channel_count_; ++ch) {
      const float pan_gain = (ch == 0) ? left : right;
      buffers[ch][i] *= smoothed_gain_ * pan_gain;
    }
  }

  for (int ch = 0; ch < channel_count_; ++ch) {
    float peak = 0.0f;
    for (uint32_t i = 0; i < frames; ++i)
      peak = std::max(peak, std::fabs(buffers[ch][i]));
    float previous = peaks_[ch].load(std::memory_order_relaxed);
    if (peak > previous) peaks_[ch].store(peak, std::memory_order_relaxed);
  }
  process_generation_.fetch_add(1, std::memory_order_release);
}

float ChannelStrip::read_peak(int channel) {
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
  insert_slots_[index].store(raw, std::memory_order_release);
  if (index == count) insert_count_.store(count + 1, std::memory_order_release);
  if (placed_at != nullptr) *placed_at = index;
  return true;
}

void ChannelStrip::remove_insert(size_t index) {
  if (index >= insert_count_.load(std::memory_order_relaxed)) return;
  PluginInstance* raw = insert_slots_[index].exchange(nullptr, std::memory_order_release);
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

void ChannelStrip::reclaim() {
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
  insert_slots_[a].store(second, std::memory_order_release);
  insert_slots_[b].store(first, std::memory_order_release);
}

PluginInstance* ChannelStrip::insert_at(size_t index) const {
  if (index >= insert_count_.load(std::memory_order_acquire)) return nullptr;
  return insert_slots_[index].load(std::memory_order_acquire);
}

}  // namespace nirbija
