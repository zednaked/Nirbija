// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
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

// 0.2 s of 441 Hz at the engine rate: 88.2 cycles, so the end of the file
// does not meet its start and a loop without a seam would jump.
fs::path write_seam_tone() {
  const fs::path path = fs::temp_directory_path() / "nirbija-fileplayer-seam.wav";
  SF_INFO info{};
  info.samplerate = 48000;
  info.channels = 1;
  info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
  SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);
  std::vector<float> tone(48000 / 5);
  for (size_t i = 0; i < tone.size(); ++i)
    tone[i] = 0.5f * std::sin(2.0 * M_PI * 441.0 * i / 48000.0);
  sf_writef_float(file, tone.data(), static_cast<sf_count_t>(tone.size()));
  sf_close(file);
  return path;
}

// Runs `blocks` and reports the largest sample-to-sample step in the left
// channel, counting from the last sample of the block before. `transport`
// is handed in as is so a test can stop, start or rewind between calls.
float step_of(nirbija::FilePlayerInstance& player, int blocks,
              const nirbija::TransportInfo& transport, float* last) {
  std::vector<float> left(256), right(256);
  float* outs[2] = {left.data(), right.data()};
  float worst = 0.0f;
  for (int b = 0; b < blocks; ++b) {
    player.set_transport(transport);
    player.process(nullptr, outs, 256);
    for (float sample : left) {
      worst = std::max(worst, std::fabs(sample - *last));
      *last = sample;
    }
  }
  return worst;
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

  // A 441 Hz sine at 0.5 moves at most 0.029 a sample. Every seam below has
  // to stay in that neighbourhood; the hard versions of each jumped by up
  // to the whole amplitude.
  {
    const fs::path seam = write_seam_tone();
    nirbija::FilePlayerInstance p;
    p.set_channel_layout(2);
    p.activate(48000.0, 256);
    if (!p.load(seam.string())) fail("could not load the seam tone");
    nirbija::TransportInfo t;
    t.playing = true;
    float last = 0.0f;
    step_of(p, 20, t, &last);  // the start fade, and into the file

    // The loop wrap.
    const float wrap = step_of(p, 400, t, &last);
    if (wrap > 0.05f) fail("the loop seam steps by " + std::to_string(wrap));

    // A gain change lands as a ramp.
    p.set_parameter(0, 0.1);
    const float gained = step_of(p, 4, t, &last);
    if (gained > 0.05f) fail("a gain change steps by " + std::to_string(gained));
    p.set_parameter(0, 1.0);
    step_of(p, 4, t, &last);

    // Stop mid-wave: a fade, then silence.
    t.playing = false;
    const float stopped = step_of(p, 2, t, &last);
    if (stopped > 0.05f) fail("a transport stop steps by " + std::to_string(stopped));
    if (peak_of(p, 4, false) > 1e-6f) fail("a stopped player kept sounding");

    // Start again: a fade in, not a step.
    t.playing = true;
    last = 0.0f;
    const float started = step_of(p, 2, t, &last);
    if (started > 0.05f) fail("a transport start steps by " + std::to_string(started));

    // A jump to the start of the song rewinds under a crossfade.
    step_of(p, 30, t, &last);
    t.changed = true;
    t.frame = 0;
    const float rewound = step_of(p, 1, t, &last);
    if (rewound > 0.05f) fail("a rewind steps by " + std::to_string(rewound));
    t.changed = false;

    // Loading another file under the playing one crossfades into it.
    if (!p.load(tone.string())) fail("could not reload the tone");
    const float swapped = step_of(p, 2, t, &last);
    if (swapped > 0.05f) fail("a file swap steps by " + std::to_string(swapped));
    if (peak_of(p, 8, true) < 0.3f) fail("the swapped-in file is silent");
    fs::remove(seam);
  }

  fs::remove(tone);
  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
