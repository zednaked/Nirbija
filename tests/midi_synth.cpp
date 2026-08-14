// Sends a note to a real synth plugin through a channel strip and checks that
// audio comes out. This is the whole MIDI path end to end, minus JACK: strip →
// insert → plugin event queue → plugin.

#include <algorithm>
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

// Advances a rolling transport, the way the engine does per block. A sequencer
// has no clock of its own and does nothing without one. The first blocks are
// stopped on purpose: a sequencer starts on the transition into play, not on
// finding the transport already moving, so a test that rolls from the first
// block never sees it start.
// Where the shared transport has got to. Reset before a chain that needs to
// see the transport start.
int block_index = 0;

nirbija::TransportInfo transport_at(int block) {
  constexpr int kStoppedBlocks = 2;

  nirbija::TransportInfo transport;
  transport.playing = block >= kStoppedBlocks;
  transport.tempo_bpm = 120.0;
  transport.frame =
      static_cast<uint64_t>(std::max(0, block - kStoppedBlocks)) * kBlock;
  transport.seconds = static_cast<double>(transport.frame) / kSampleRate;
  transport.beats = transport.seconds * transport.tempo_bpm / 60.0;
  transport.changed = block == kStoppedBlocks;
  return transport;
}

// Runs `blocks` blocks of silence-in and reports the loudest one, so a synth
// with a slow attack still registers.
float loudest_over(nirbija::ChannelStrip& strip, int blocks) {
  Block block;
  float loudest = 0.0f;
  for (int i = 0; i < blocks; ++i) {
    block.clear();
    const nirbija::TransportInfo transport = transport_at(block_index++);
    strip.process(block.ptrs, kBlock, nullptr, 0, &transport);
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

  // --- MIDI produced by one insert reaching the next -------------------------
  // A step sequencer's whole output is MIDI. If the host does not read it and
  // pass it down the chain, it plays into nothing.
  {
    std::unique_ptr<nirbija::PluginInstance> sequencer;
    std::unique_ptr<nirbija::PluginInstance> voice;
    for (auto& backend : nirbija::make_all_backends()) {
      for (const auto& descriptor : backend->scan()) {
        if (descriptor.name == "MIDI Step Sequencer8x8" && sequencer == nullptr)
          sequencer = backend->instantiate(descriptor);
        if (descriptor.name == wanted && voice == nullptr)
          voice = backend->instantiate(descriptor);
      }
    }

    if (sequencer == nullptr) {
      std::printf("no step sequencer installed, skipping the chain check\n");
    } else {
      // A sequencer starts with an empty grid, so it would play nothing however
      // right the chain is. Filling a few steps is what makes this a test of
      // the host rather than of the plugin's defaults.
      int filled = 0;
      for (const auto& parameter : sequencer->parameters()) {
        // Follow the host clock rather than free-running, which is the whole
        // point of the transport being wired up.
        if (parameter.name == "Sync") sequencer->set_parameter(parameter.id, 1.0);

        if (parameter.name.rfind("Grid S:", 0) != 0) continue;
        if (parameter.name.find("N: 1") == std::string::npos) continue;
        sequencer->set_parameter(parameter.id, 100.0);
        ++filled;
      }
      if (filled == 0) fail("found no grid steps to fill on the sequencer");

      // Back to a stopped transport, so the chain sees play being pressed.
      block_index = 0;

      nirbija::ChannelStrip chain("chain", 2);
      if (voice == nullptr) {
        fail("could not make a second instance of the synth");
        return 1;
      }
      if (!chain.add_insert(std::move(sequencer)))
        fail("could not add the sequencer to the chain");
      if (!chain.add_insert(std::move(voice)))
        fail("could not add the synth below the sequencer");
      chain.prepare(kSampleRate, kBlock);
      std::printf("  chain has %zu inserts\n", chain.insert_count());

      // The sequencer runs on its own clock, so this waits rather than sending
      // anything: a couple of seconds covers a step at any sane tempo.
      const float sound =
          loudest_over(chain, static_cast<int>(kSampleRate * 2 / kBlock));
      if (sound <= 1e-4f)
        fail("the step sequencer never reached the synth below it");
      else
        std::printf("  sequencer drove the synth: peak %.6f\n", sound);
    }
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok: %s, silence %.6f, note %.6f, after note-off %.6f\n",
              wanted.c_str(), before, after, tail);
  return 0;
}
