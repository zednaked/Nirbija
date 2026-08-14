// Sends a note to a real synth plugin through a channel strip and checks that
// audio comes out. This is the whole MIDI path end to end, minus JACK: strip →
// insert → plugin event queue → plugin.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/channel_strip.h"
#include "core/plugin.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kBlock = 256;

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

nirbija::MidiEvent note(uint8_t status, uint8_t key, uint8_t velocity) {
  nirbija::MidiEvent event;
  event.frame = 0;
  event.size = 3;
  event.data[0] = status;
  event.data[1] = key;
  event.data[2] = velocity;
  return event;
}

struct Block {
  Block() : data(2, std::vector<float>(kBlock, 0.0f)) {
    ptrs[0] = data[0].data();
    ptrs[1] = data[1].data();
  }

  void clear() {
    for (auto& channel : data) std::fill(channel.begin(), channel.end(), 0.0f);
  }

  float peak() const {
    float peak = 0.0f;
    for (const auto& channel : data)
      for (float sample : channel) peak = std::max(peak, std::fabs(sample));
    return peak;
  }

  bool has_non_finite() const {
    for (const auto& channel : data)
      for (float sample : channel)
        if (!std::isfinite(sample)) return true;
    return false;
  }

  std::vector<std::vector<float>> data;
  float* ptrs[2];
};

// Runs `blocks` blocks of silence-in and reports the loudest one, so a synth
// with a slow attack still registers.
float loudest_over(nirbija::ChannelStrip& strip, int blocks) {
  Block block;
  float loudest = 0.0f;
  for (int i = 0; i < blocks; ++i) {
    block.clear();
    strip.process(block.ptrs, kBlock);
    if (block.has_non_finite()) {
      fail("synth produced NaN or inf");
      return 0.0f;
    }
    loudest = std::max(loudest, block.peak());
  }
  return loudest;
}

}  // namespace

int main(int argc, char* argv[]) {
  const std::string wanted = argc > 1 ? argv[1] : "Odin2";

  std::unique_ptr<nirbija::PluginInstance> synth;
  for (auto& backend : nirbija::make_all_backends()) {
    for (const auto& descriptor : backend->scan()) {
      if (descriptor.name != wanted) continue;
      synth = backend->instantiate(descriptor);
      if (synth != nullptr) break;
    }
    if (synth != nullptr) break;
  }

  if (synth == nullptr) {
    std::printf("%s not installed, skipping\n", wanted.c_str());
    return 0;
  }
  if (!synth->descriptor().has_midi_input) {
    std::printf("%s reports no MIDI input, skipping\n", wanted.c_str());
    return 0;
  }

  nirbija::ChannelStrip strip("synth", 2);
  if (!strip.add_insert(std::move(synth))) {
    fail("could not add the synth as an insert");
    return 1;
  }
  strip.prepare(kSampleRate, kBlock);

  // Silence before the note proves the strip is not just passing noise through.
  const float before = loudest_over(strip, 4);
  if (before > 1e-4f)
    fail("synth was already making sound before any note was sent");

  {
    const nirbija::MidiEvent on = note(0x90, 60, 100);
    Block block;
    strip.process(block.ptrs, kBlock, &on, 1);
  }

  const float after = loudest_over(strip, 24);
  if (after <= 1e-4f) fail("note-on produced no audio");

  {
    const nirbija::MidiEvent off = note(0x80, 60, 0);
    Block block;
    strip.process(block.ptrs, kBlock, &off, 1);
  }

  // The release tail can be long, so this only checks the note is decaying,
  // not that it has already reached silence.
  const float tail = loudest_over(strip, 64);
  if (tail >= after) fail("note-off did not start a release");

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok: %s, silence %.6f, note %.6f, after note-off %.6f\n",
              wanted.c_str(), before, after, tail);
  return 0;
}
