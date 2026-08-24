// A pad sampler: load a file, fire a note, hear it; Rec from the strip input
// lands on a pad and plays back; names survive a state round trip.

#include <sndfile.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/sampler.h"

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kBlock = 256;
constexpr double kRate = 48000.0;

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

void expect(bool condition, const std::string& what) {
  if (!condition) fail(what);
}

fs::path write_tone() {
  const fs::path path = fs::temp_directory_path() / "nirbija-sampler.wav";
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

void note_on(nirbija::SamplerInstance& sampler, int note, int velocity,
             uint32_t frame = 0) {
  nirbija::MidiEvent event;
  event.frame = frame;
  event.size = 3;
  event.data[0] = 0x90;
  event.data[1] = static_cast<uint8_t>(note);
  event.data[2] = static_cast<uint8_t>(velocity);
  sampler.queue_midi(event);
}

void note_off(nirbija::SamplerInstance& sampler, int note, uint32_t frame = 0) {
  nirbija::MidiEvent event;
  event.frame = frame;
  event.size = 3;
  event.data[0] = 0x80;
  event.data[1] = static_cast<uint8_t>(note);
  event.data[2] = 0;
  sampler.queue_midi(event);
}

float run(nirbija::SamplerInstance& sampler, float input = 0.0f) {
  std::vector<float> in_l(kBlock, input), in_r(kBlock, input);
  std::vector<float> out_l(kBlock), out_r(kBlock);
  const float* ins[2] = {in_l.data(), in_r.data()};
  float* outs[2] = {out_l.data(), out_r.data()};
  sampler.process(ins, outs, kBlock);
  float peak = 0.0f;
  for (float sample : out_l) peak = std::max(peak, std::fabs(sample));
  return peak;
}

}  // namespace

int main() {
  const fs::path tone = write_tone();

  // --- silent until a pad has audio ------------------------------------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    note_on(sampler, 36, 100);
    expect(run(sampler) < 1e-6f, "an empty pad sounded");
  }

  // --- load a file, fire the pad's note, hear it -----------------------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    expect(sampler.load(0, tone.string()), "could not load the tone");
    expect(sampler.pad_has_audio(0), "load did not mark the pad as having audio");
    expect(run(sampler) < 1e-6f, "played without a note");
    note_on(sampler, 36, 127);
    expect(run(sampler) > 0.2f, "a loaded pad did not play on its note");
  }

  // --- the wrong note is silence ---------------------------------------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.load(0, tone.string());
    note_on(sampler, 60, 127);
    expect(run(sampler) < 1e-6f, "a note no pad owns still triggered");
  }

  // --- one-shot ignores note-off ---------------------------------------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.load(0, tone.string());
    note_on(sampler, 36, 127);
    run(sampler);
    note_off(sampler, 36);
    float peak = 0.0f;
    for (int i = 0; i < 8; ++i) peak = std::max(peak, run(sampler));
    expect(peak > 0.1f, "one-shot stopped on note-off");
  }

  // --- hold mode stops after note-off ----------------------------------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.load(0, tone.string());
    sampler.set_parameter(5, 0.0);  // one-shot off on focused pad 0
    note_on(sampler, 36, 127);
    expect(run(sampler) > 0.2f, "hold mode did not start");
    note_off(sampler, 36);
    run(sampler);  // fade
    run(sampler);
    expect(run(sampler) < 1e-5f, "hold mode kept sounding after note-off");
  }

  // --- Rec from the strip input lands on the focused pad ---------------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.set_parameter(1, 1.0);  // rec
    for (int i = 0; i < 8; ++i) run(sampler, 0.6f);
    sampler.set_parameter(1, 0.0);
    run(sampler, 0.0f);  // process applies the stop
    expect(sampler.commit_take(), "commit_take found nothing after Rec");
    expect(sampler.pad_has_audio(0), "the take did not land on pad 0");
    note_on(sampler, 36, 127);
    expect(run(sampler) > 0.4f, "the recorded pad did not play back");
  }

  // --- Rec monitors the input; idle does not pass it through -----------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    expect(run(sampler, 0.8f) < 1e-6f, "idle sampler passed the strip input");
    sampler.set_parameter(1, 1.0);
    expect(run(sampler, 0.8f) > 0.7f, "Rec did not monitor the input");
    sampler.set_parameter(1, 0.0);
    run(sampler);
    sampler.commit_take();
  }

  // --- two pads, two notes, both speak ---------------------------------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.load(0, tone.string());
    sampler.load(1, tone.string());
    note_on(sampler, 36, 100);
    note_on(sampler, 38, 100);
    nirbija::MidiEvent out[8];
    expect(sampler.take_midi_output(out, 8) == 0,
           "the sampler emitted MIDI; it is an instrument");
    expect(run(sampler) > 0.3f, "two pads together were silent");
    expect((sampler.sounding_mask() & 0x3) == 0x3,
           "sounding_mask did not show both pads");
  }

  // --- note_names publishes the factory kit ----------------------------------
  {
    nirbija::SamplerInstance sampler;
    const auto names = sampler.note_names();
    expect(names.size() == 16, "note_names was not 16 pads");
    expect(names[0].key == 36 && names[0].name == "Kick",
           "pad 0 was not Kick on 36");
    expect(names[1].key == 38 && names[1].name == "Snare",
           "pad 1 was not Snare on 38");
    sampler.set_pad_name(0, "Thump");
    expect(sampler.note_names()[0].name == "Thump", "rename did not stick");
  }

  // --- pitch up finishes sooner ----------------------------------------------
  {
    nirbija::SamplerInstance slow;
    slow.set_channel_layout(2);
    slow.activate(kRate, kBlock);
    slow.load(0, tone.string());
    note_on(slow, 36, 127);
    int slow_blocks = 0;
    while (run(slow) > 1e-4f && slow_blocks < 200) ++slow_blocks;

    nirbija::SamplerInstance fast;
    fast.set_channel_layout(2);
    fast.activate(kRate, kBlock);
    fast.load(0, tone.string());
    fast.set_parameter(6, 12.0);  // +12 semitones on focused pad
    note_on(fast, 36, 127);
    int fast_blocks = 0;
    while (run(fast) > 1e-4f && fast_blocks < 200) ++fast_blocks;
    expect(fast_blocks < slow_blocks / 2 + 4,
           "an octave up did not play out roughly twice as fast");
  }

  // --- trim start/end cuts the take ------------------------------------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.set_parameter(1, 1.0);
    for (int i = 0; i < 8; ++i) run(sampler, 0.8f);
    for (int i = 0; i < 8; ++i) run(sampler, 0.0f);
    sampler.set_parameter(1, 0.0);
    run(sampler);
    sampler.commit_take();
    sampler.set_trim(0, 0.5, 1.0);  // silent half
    note_on(sampler, 36, 127);
    float peak = 0.0f;
    for (int i = 0; i < 8; ++i) peak = std::max(peak, run(sampler));
    expect(peak < 0.05f, "trim did not cut the loud half");
  }

  // --- state round trip keeps audio and names --------------------------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.load(0, tone.string());
    sampler.set_pad_name(0, "Ping");
    sampler.set_parameter(0, 0.5);  // gain
    sampler.set_parameter(6, -5.0);
    const auto blob = sampler.save_state();

    nirbija::SamplerInstance restored;
    restored.set_channel_layout(2);
    restored.activate(kRate, kBlock);
    expect(restored.load_state(blob), "load_state rejected its own blob");
    expect(restored.pad_name(0) == "Ping", "name did not survive the blob");
    expect(std::fabs(restored.parameter_value(0) - 0.5) < 1e-4,
           "gain did not survive the blob");
    expect(std::fabs(restored.parameter_value(6) + 5.0) < 1e-3,
           "pitch did not survive the blob");
    expect(restored.pad_has_audio(0), "audio did not survive the blob");
    expect(restored.pad_path(0) == tone.string(),
           "the file path did not survive the blob");
    note_on(restored, 36, 127);
    expect(run(restored) > 0.1f, "the restored pad was silent");
  }

  // --- a Rec take has no path and travels as audio in the blob ---------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.set_parameter(1, 1.0);
    for (int i = 0; i < 8; ++i) run(sampler, 0.6f);
    sampler.set_parameter(1, 0.0);
    run(sampler);
    sampler.commit_take();
    expect(sampler.pad_path(0).empty(), "a Rec take remembered a file path");
    const auto blob = sampler.save_state();
    nirbija::SamplerInstance restored;
    restored.set_channel_layout(2);
    restored.activate(kRate, kBlock);
    expect(restored.load_state(blob), "rec blob was refused");
    expect(restored.pad_path(0).empty(), "the rec blob invented a path");
    note_on(restored, 36, 127);
    expect(run(restored) > 0.4f, "the rec take did not survive without a file");
  }

  // --- relative paths resolve against a session folder -----------------------
  {
    const fs::path dir = fs::temp_directory_path() / "nirbija-kit";
    fs::create_directories(dir);
    const fs::path wav = dir / "kick.wav";
    fs::copy_file(tone, wav, fs::copy_options::overwrite_existing);
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.load(0, wav.string());
    // Pretend the session stored a relative name, Koala-style.
    // save_state writes whatever load() stored; rewrite via load_state.
    auto blob = sampler.save_state();
    // Point the stored path at just the filename, then resolve.
    nirbija::SamplerInstance named;
    named.set_channel_layout(2);
    named.activate(kRate, kBlock);
    named.load(0, "kick.wav");  // missing from cwd
    expect(!named.pad_has_audio(0), "a missing relative path should stay silent");
    named.resolve_paths(dir.string());
    expect(named.pad_has_audio(0), "resolve_paths did not find kick.wav");
    note_on(named, 36, 127);
    expect(run(named) > 0.1f, "resolved kick was silent");
    fs::remove_all(dir);
    (void)blob;
  }

  // --- empty blob is factory, not failure ------------------------------------
  {
    nirbija::SamplerInstance sampler;
    expect(sampler.load_state({}), "empty blob was rejected");
    expect(sampler.pad_name(0) == "Kick", "empty blob cleared the factory names");
  }

  // --- Rec on pad 3 leaves pad 0 empty ---------------------------------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.set_parameter(2, 4.0);  // focus pad 4 (1-based)
    sampler.set_parameter(1, 1.0);
    for (int i = 0; i < 4; ++i) run(sampler, 0.5f);
    sampler.set_parameter(1, 0.0);
    run(sampler);
    sampler.commit_take();
    expect(!sampler.pad_has_audio(0), "Rec on pad 3 filled pad 0");
    expect(sampler.pad_has_audio(3), "Rec on focused pad 3 did not land");
  }

  fs::remove(tone);
  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
