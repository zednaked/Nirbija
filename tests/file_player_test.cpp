// Writes a short tone to disk, loads it into the file player, and checks it
// plays, loops, resamples, and survives a state round trip.

#include <sndfile.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/file_player.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

// A 0.25 s 440 Hz tone at 44.1 kHz — deliberately not the engine rate, so
// resampling is on the path.
fs::path write_tone() {
  const fs::path path = fs::temp_directory_path() / "nirbija-fileplayer.wav";
  SF_INFO info{};
  info.samplerate = 44100;
  info.channels = 1;
  info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;

  SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);
  std::vector<float> tone(44100 / 4);
  for (size_t i = 0; i < tone.size(); ++i)
    tone[i] = 0.5f * std::sin(2.0 * M_PI * 440.0 * i / 44100.0);
  sf_writef_float(file, tone.data(), static_cast<sf_count_t>(tone.size()));
  sf_close(file);
  return path;
}

float peak_of(nirbija::FilePlayerInstance& player, int blocks, bool playing) {
  std::vector<float> left(256), right(256);
  float* outs[2] = {left.data(), right.data()};

  nirbija::TransportInfo transport;
  transport.playing = playing;
  float peak = 0.0f;
  for (int i = 0; i < blocks; ++i) {
    player.set_transport(transport);
    player.process(nullptr, outs, 256);
    for (float sample : left) peak = std::max(peak, std::fabs(sample));
  }
  return peak;
}

}  // namespace

int main() {
  const fs::path tone = write_tone();

  nirbija::FilePlayerInstance player;
  player.set_channel_layout(2);
  player.activate(48000.0, 256);

  if (!player.load(tone.string())) {
    fail("could not load the tone");
    return 1;
  }

  if (peak_of(player, 4, false) > 1e-6f)
    fail("played while the transport was stopped");
  if (peak_of(player, 8, true) < 0.3f)
    fail("no audio while playing");

  // 0.25 s of file, 2 s of playback: only looping gets through that.
  const float looped = peak_of(player, 48000 * 2 / 256, true);
  if (looped < 0.3f) fail("looping stopped the sound");

  player.set_parameter(1, 0.0);  // loop off
  peak_of(player, 48000 * 2 / 256, true);  // play past the end
  if (peak_of(player, 4, true) > 1e-6f)
    fail("kept playing past the end with loop off");

  // State: path and parameters survive, position resets on rewind.
  const auto blob = player.save_state();
  nirbija::FilePlayerInstance restored;
  restored.set_channel_layout(2);
  restored.activate(48000.0, 256);
  if (!restored.load_state(blob)) fail("load_state rejected its own blob");
  if (restored.path() != tone.string()) fail("path did not survive the state");
  if (restored.parameter_value(1) != 0.0) fail("loop setting did not survive");

  fs::remove(tone);
  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
