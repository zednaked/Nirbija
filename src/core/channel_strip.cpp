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

void ChannelStrip::prepare(double sample_rate, uint32_t max_block_frames) {
  sample_rate_ = sample_rate;
  max_block_frames_ = max_block_frames;
  smoothing_coeff_ =
      static_cast<float>(std::exp(-1.0 / (kSmoothingSeconds * sample_rate)));
  smoothed_gain_ = gain_.load(std::memory_order_relaxed);
  smoothed_pan_ = pan_.load(std::memory_order_relaxed);

  plugin_io_.assign(static_cast<size_t>(channel_count_), nullptr);
  for (auto& insert : owned_inserts_) {
    insert->set_channel_layout(channel_count_);
    insert->activate(sample_rate, max_block_frames);
  }
}

void ChannelStrip::process(float* const* buffers, uint32_t frames) {
  if (muted_.load(std::memory_order_relaxed)) {
    for (int ch = 0; ch < channel_count_; ++ch)
      std::fill_n(buffers[ch], frames, 0.0f);
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

  // Inserts run after the fader, which is AUM's default and what the strip
  // layout shows: the slots sit below the fader, on the way to the output.
  // TODO(phase-6): per-slot pre/post, toggled by long-pressing the slot.
  for (int ch = 0; ch < channel_count_; ++ch) plugin_io_[ch] = buffers[ch];
  const size_t insert_count = insert_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < insert_count; ++i) {
    PluginInstance* insert = insert_slots_[i].load(std::memory_order_acquire);
    if (insert != nullptr)
      insert->process(plugin_io_.data(), plugin_io_.data(), frames);
  }

  for (int ch = 0; ch < channel_count_; ++ch) {
    float peak = 0.0f;
    for (uint32_t i = 0; i < frames; ++i)
      peak = std::max(peak, std::fabs(buffers[ch][i]));
    float previous = peaks_[ch].load(std::memory_order_relaxed);
    if (peak > previous) peaks_[ch].store(peak, std::memory_order_relaxed);
  }
}

float ChannelStrip::read_peak(int channel) {
  return peaks_[channel].exchange(0.0f, std::memory_order_relaxed);
}

bool ChannelStrip::add_insert(std::unique_ptr<PluginInstance> plugin) {
  const size_t index = insert_count_.load(std::memory_order_relaxed);
  if (index >= kMaxInserts || plugin == nullptr) return false;

  plugin->set_channel_layout(channel_count_);
  if (sample_rate_ > 0.0) plugin->activate(sample_rate_, max_block_frames_);

  PluginInstance* raw = plugin.get();
  owned_inserts_.push_back(std::move(plugin));

  // Publish the slot before the count, so the audio thread can never see a
  // count that reaches a slot it cannot read yet.
  insert_slots_[index].store(raw, std::memory_order_release);
  insert_count_.store(index + 1, std::memory_order_release);
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
    retired_.push_back(std::move(*it));
    owned_inserts_.erase(it);
  }
}

PluginInstance* ChannelStrip::insert_at(size_t index) const {
  if (index >= insert_count_.load(std::memory_order_acquire)) return nullptr;
  return insert_slots_[index].load(std::memory_order_acquire);
}

}  // namespace nirbija
