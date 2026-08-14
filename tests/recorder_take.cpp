// Records a take from a live engine and reads the files back. A recorder that
// writes a file is not the same as a recorder that writes the audio, so this
// checks the samples, not just that something appeared on disk.

#include <sndfile.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/engine.h"
#include "core/plugin.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

std::unique_ptr<nirbija::PluginInstance> find_synth(const std::string& name) {
  for (auto& backend : nirbija::make_all_backends())
    for (const auto& descriptor : backend->scan())
      if (descriptor.name == name && descriptor.has_midi_input)
        return backend->instantiate(descriptor);
  return nullptr;
}

struct FileSummary {
  bool opened = false;
  sf_count_t frames = 0;
  int channels = 0;
  float peak = 0.0f;
  bool has_non_finite = false;
};

FileSummary summarise(const fs::path& path) {
  FileSummary summary;
  SF_INFO info{};
  SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
  if (file == nullptr) return summary;

  summary.opened = true;
  summary.frames = info.frames;
  summary.channels = info.channels;

  std::vector<float> block(4096 * info.channels);
  sf_count_t read = 0;
  while ((read = sf_readf_float(file, block.data(), 4096)) > 0) {
    for (sf_count_t i = 0; i < read * info.channels; ++i) {
      if (!std::isfinite(block[i])) summary.has_non_finite = true;
      summary.peak = std::max(summary.peak, std::fabs(block[i]));
    }
  }
  sf_close(file);
  return summary;
}

}  // namespace

int main() {
  nirbija::Engine engine;
  if (!engine.start("nirbija-rec")) {
    std::printf("no JACK server available, skipping\n");
    return 0;
  }

  auto synth = find_synth("Odin2");
  if (synth == nullptr) {
    std::printf("Odin2 not installed, skipping\n");
    return 0;
  }

  const size_t channel = engine.add_channel("synth", 2);
  if (channel == nirbija::kMaxChannels) {
    fail("could not add a channel");
    return 1;
  }
  nirbija::ChannelStrip& strip = engine.graph().channel(channel);
  if (!strip.add_insert(std::move(synth))) {
    fail("could not load the synth");
    return 1;
  }
  strip.set_armed(true);

  const fs::path root = fs::temp_directory_path() / "nirbija-recorder-test";
  fs::remove_all(root);

  const std::string take = engine.start_recording(root.string());
  if (take.empty()) {
    fail("recording did not start");
    return 1;
  }

  // Silence first, then a note, so the files have something to distinguish.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  nirbija::MidiEvent note;
  note.frame = 0;
  note.size = 3;
  note.data[0] = 0x90;
  note.data[1] = 60;
  note.data[2] = 100;

  // Queued straight into the plugin: this test is about the recorder, and the
  // MIDI path has its own.
  nirbija::PluginInstance* insert = strip.insert_at(0);
  for (int i = 0; i < 8; ++i) {
    insert->queue_midi(note);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  const double elapsed = engine.recorded_seconds();
  const bool overran = engine.recording_overran();
  engine.stop_recording();
  engine.stop();

  if (elapsed < 0.5) fail("the recorder counted almost no time");
  if (overran) fail("the writer could not keep up with the audio thread");

  // One file per armed channel plus the master.
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(take)) files.push_back(entry.path());
  std::sort(files.begin(), files.end());

  if (files.size() != 2) {
    fail("expected 2 files, got " + std::to_string(files.size()));
    return 1;
  }

  for (const auto& path : files) {
    const FileSummary summary = summarise(path);
    if (!summary.opened) {
      fail("could not read back " + path.filename().string());
      continue;
    }
    if (summary.channels != 2)
      fail(path.filename().string() + " is not stereo");
    if (summary.has_non_finite)
      fail(path.filename().string() + " contains NaN or inf");
    if (summary.frames < static_cast<sf_count_t>(engine.sample_rate() * 0.4))
      fail(path.filename().string() + " is shorter than the take");
    if (summary.peak <= 1e-4f)
      fail(path.filename().string() + " is silent");

    std::printf("  %s: %lld frames, peak %.4f\n", path.filename().string().c_str(),
                static_cast<long long>(summary.frames), summary.peak);
  }

  fs::remove_all(root);

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok: %.2f s recorded\n", elapsed);
  return 0;
}
