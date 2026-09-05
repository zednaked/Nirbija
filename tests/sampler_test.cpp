// A pad sampler: load a file, fire a note, hear it; Rec from the strip input
// lands on a pad and plays back; names survive a state round trip.

#include <sndfile.h>

#include <cmath>
#include <cstdio>
#include <memory>
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

// Every left-channel sample across enough blocks to cover `total_frames`,
// for tests that need to look at one exact sample rather than a block peak.
std::vector<float> run_capture(nirbija::SamplerInstance& sampler,
                               uint32_t total_frames) {
  std::vector<float> captured;
  captured.reserve(total_frames);
  std::vector<float> in_l(kBlock, 0.0f), in_r(kBlock, 0.0f);
  std::vector<float> out_l(kBlock), out_r(kBlock);
  const float* ins[2] = {in_l.data(), in_r.data()};
  float* outs[2] = {out_l.data(), out_r.data()};
  while (captured.size() < total_frames) {
    sampler.process(ins, outs, kBlock);
    for (float sample : out_l) captured.push_back(sample);
  }
  captured.resize(total_frames);
  return captured;
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

    // Once found, the pad remembers the whole path: the autosave that this
    // state lands in sits in the user's data folder, next to no samples,
    // and the next start must find the kit without any folder to resolve
    // against.
    expect(fs::path(named.pad_path(0)).is_absolute(),
           "a resolved pad kept its relative name: " + named.pad_path(0));
    nirbija::SamplerInstance restarted;
    restarted.set_channel_layout(2);
    restarted.activate(kRate, kBlock);
    expect(restarted.load_state(named.save_state()),
           "the resolved state was refused");
    restarted.resolve_paths("/nonexistent/data/folder");
    expect(restarted.pad_has_audio(0),
           "an absolute pad path did not survive a restart elsewhere");

    // And a kit that travelled - the folder moved, the absolute name now
    // pointing nowhere - is found again next to the file that names it, by
    // the last folder and the file name.
    const fs::path moved = fs::temp_directory_path() / "nirbija-kit-moved";
    fs::create_directories(moved / "nirbija-kit");
    fs::copy_file(wav, moved / "nirbija-kit" / "kick.wav",
                  fs::copy_options::overwrite_existing);
    const auto travelled = named.save_state();
    fs::remove_all(dir);
    nirbija::SamplerInstance arrived;
    arrived.set_channel_layout(2);
    arrived.activate(kRate, kBlock);
    expect(arrived.load_state(travelled), "the travelled state was refused");
    arrived.resolve_paths(moved.string());
    expect(arrived.pad_has_audio(0),
           "a moved kit was not found next to its new session");
    fs::remove_all(moved);
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

  // --- undo puts back what Clear threw away, and toggles ---------------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    expect(!sampler.pad_can_undo(0), "a fresh pad already had an undo");
    sampler.load(0, tone.string());
    sampler.clear_pad(0);
    expect(!sampler.pad_has_audio(0), "clear_pad left audio on the pad");
    expect(sampler.pad_can_undo(0), "clear_pad left nothing to undo");
    expect(sampler.undo_pad(0), "undo_pad refused after a clear");
    expect(sampler.pad_has_audio(0), "undo did not bring the audio back");
    expect(sampler.pad_path(0) == tone.string(),
           "undo did not bring the path back");
    note_on(sampler, 36, 127);
    expect(run(sampler) > 0.2f, "the pad undo restored did not play");
    // A second undo is a redo: it swaps forward to the cleared pad again.
    expect(sampler.undo_pad(0), "the second undo_pad call failed");
    expect(!sampler.pad_has_audio(0), "undo did not toggle back to cleared");
  }

  // --- undo puts back the file a Rec take just recorded over ------------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.load(0, tone.string());
    sampler.set_parameter(1, 1.0);  // rec onto the same, focused pad 0
    for (int i = 0; i < 8; ++i) run(sampler, 0.6f);
    sampler.set_parameter(1, 0.0);
    run(sampler);
    sampler.commit_take();
    expect(sampler.pad_path(0).empty(), "the rec take kept the old file's path");
    expect(sampler.undo_pad(0), "undo_pad refused after Rec overwrote a file pad");
    expect(sampler.pad_path(0) == tone.string(),
           "undo did not bring back the file pad Rec recorded over");
  }

  // --- count-in clicks before Rec actually starts, then punches in -----------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    nirbija::TransportInfo transport;
    transport.tempo_bpm = 120.0;
    transport.numerator = 4;
    sampler.set_transport(transport);

    sampler.set_parameter(10, 1.0);  // count-in on
    sampler.set_parameter(1, 1.0);   // ask for Rec
    const float first_block_peak = run(sampler, 0.5f);
    expect(sampler.counting_in(), "count-in did not start on Rec");
    expect(!sampler.recording(), "Rec started before the count finished");
    expect(first_block_peak > 0.05f, "the count-in click was not audible");

    int blocks = 0;
    while (sampler.counting_in() && blocks < 1000) {
      run(sampler, 0.5f);
      ++blocks;
    }
    expect(!sampler.counting_in(), "the count-in never finished");
    expect(sampler.recording(), "Rec did not punch in once the count finished");
    for (int i = 0; i < 4; ++i) run(sampler, 0.6f);
    sampler.set_parameter(1, 0.0);
    run(sampler);
    expect(sampler.commit_take(), "the counted-in take produced nothing");
    expect(sampler.pad_has_audio(0), "the counted-in take did not land");
  }

  // --- turning count-in off mid-count cancels the pending take ---------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    nirbija::TransportInfo transport;
    transport.tempo_bpm = 120.0;
    transport.numerator = 4;
    sampler.set_transport(transport);

    sampler.set_parameter(10, 1.0);
    sampler.set_parameter(1, 1.0);
    run(sampler, 0.5f);
    expect(sampler.counting_in(), "count-in did not start");
    sampler.set_parameter(10, 0.0);  // count-in off mid-count
    run(sampler, 0.5f);
    expect(!sampler.counting_in(), "count-in kept running after being turned off");
    expect(!sampler.recording(), "canceling the count still started Rec");
  }

  // --- count-in survives a state round trip -----------------------------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_parameter(10, 1.0);
    const auto blob = sampler.save_state();
    nirbija::SamplerInstance restored;
    expect(restored.load_state(blob), "load_state rejected a count-in blob");
    expect(restored.parameter_value(10) >= 0.5,
           "count-in did not survive the blob");

    nirbija::SamplerInstance old_blob_reader;
    expect(old_blob_reader.load_state({}),
           "an old, count-in-less blob should still load");
    expect(old_blob_reader.parameter_value(10) < 0.5,
           "an old blob should default count-in to off");
  }

  // --- a pack carries the pads but leaves the instrument's own knobs alone --
  {
    nirbija::SamplerInstance source;
    source.set_channel_layout(2);
    source.activate(kRate, kBlock);
    source.load(0, tone.string());
    source.set_pad_name(0, "Packed");
    source.set_parameter(0, 0.4);   // gain
    source.set_parameter(3, 2.0);   // quantize
    source.set_parameter(10, 1.0);  // count-in
    const auto pack = source.save_pads();

    nirbija::SamplerInstance target;
    target.set_channel_layout(2);
    target.activate(kRate, kBlock);
    target.set_parameter(0, 1.7);  // a gain the pack must not touch
    target.set_parameter(3, 0.0);  // a quantize the pack must not touch
    expect(target.load_pads(pack), "load_pads rejected its own blob");
    expect(target.pad_name(0) == "Packed", "the pack's pad name did not arrive");
    expect(target.pad_has_audio(0), "the pack's audio did not arrive");
    expect(std::fabs(target.parameter_value(0) - 1.7) < 1e-4,
           "load_pads touched the instrument's own gain");
    expect(target.parameter_value(3) == 0.0,
           "load_pads touched the instrument's own quantize");
    expect(target.parameter_value(10) < 0.5,
           "load_pads touched the instrument's own count-in");
    note_on(target, 36, 127);
    expect(run(target) > 0.1f, "the packed pad was silent after loading");

    // The two formats do not answer to each other's magic.
    expect(!target.load_pads(source.save_state()),
           "load_pads accepted a whole-instrument save_state blob");
    expect(!target.load_state(pack),
           "load_state accepted a pads-only pack blob");
  }

  // --- fade-in slows the ramp-up past the anti-click envelope -----------------
  {
    nirbija::SamplerInstance flat;
    flat.set_channel_layout(2);
    flat.activate(kRate, kBlock);
    flat.load(0, tone.string());
    note_on(flat, 36, 127);
    const auto flat_out = run_capture(flat, 200);

    nirbija::SamplerInstance faded;
    faded.set_channel_layout(2);
    faded.activate(kRate, kBlock);
    faded.load(0, tone.string());
    faded.set_fades(0, 1.0, 0.0);  // the whole first half of the take
    note_on(faded, 36, 127);
    const auto faded_out = run_capture(faded, 200);

    expect(std::fabs(faded_out[150]) < std::fabs(flat_out[150]) * 0.5f,
           "fade-in did not slow the ramp-up past the anti-click window");
  }

  // --- fade-out pulls the tail down before the natural end --------------------
  {
    // Sampled close to the take's own end (11025 frames at the file's own
    // 44100 Hz, resampled onto the 48kHz engine here) but still short of the
    // last 64-sample anti-click release, so this isolates the fade from that.
    const uint32_t near_end = 11700;

    nirbija::SamplerInstance flat;
    flat.set_channel_layout(2);
    flat.activate(kRate, kBlock);
    flat.load(0, tone.string());
    note_on(flat, 36, 127);
    const auto flat_out = run_capture(flat, near_end + 100);

    nirbija::SamplerInstance faded;
    faded.set_channel_layout(2);
    faded.activate(kRate, kBlock);
    faded.load(0, tone.string());
    faded.set_fades(0, 0.0, 1.0);  // the whole second half of the take
    note_on(faded, 36, 127);
    const auto faded_out = run_capture(faded, near_end + 100);

    expect(std::fabs(faded_out[near_end]) < std::fabs(flat_out[near_end]) * 0.5f,
           "fade-out did not pull the tail down ahead of the end");
  }

  // --- fades survive save_state, and an older blob without them still loads --
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.load(0, tone.string());
    sampler.set_fades(0, 0.6, 0.4);
    const auto blob = sampler.save_state();

    nirbija::SamplerInstance restored;
    restored.set_channel_layout(2);
    restored.activate(kRate, kBlock);
    expect(restored.load_state(blob), "load_state rejected a blob with fades");
    expect(std::fabs(restored.pad_fade_in(0) - 0.6) < 1e-6,
           "fade-in did not survive save_state/load_state");
    expect(std::fabs(restored.pad_fade_out(0) - 0.4) < 1e-6,
           "fade-out did not survive save_state/load_state");

    // Simulate a blob saved before fades (and count-in) existed by chopping
    // off exactly those two trailing, optional sections.
    const size_t old_size =
        blob.size() - (static_cast<size_t>(nirbija::SamplerInstance::kPads) *
                           2 * sizeof(double) +
                       sizeof(int32_t));
    const std::vector<uint8_t> old_blob(blob.begin(),
                                        blob.begin() + static_cast<std::ptrdiff_t>(old_size));
    nirbija::SamplerInstance from_old;
    from_old.set_channel_layout(2);
    from_old.activate(kRate, kBlock);
    expect(from_old.load_state(old_blob), "an old-shaped blob was rejected");
    expect(from_old.pad_fade_in(0) == 0.0,
           "an old-shaped blob should default fade-in to 0");
    expect(from_old.pad_has_audio(0), "an old-shaped blob lost its audio");
  }

  // --- fades travel with a pack too, not just gain/quantize/count-in ---------
  {
    nirbija::SamplerInstance source;
    source.set_channel_layout(2);
    source.activate(kRate, kBlock);
    source.load(0, tone.string());
    source.set_fades(0, 0.3, 0.7);
    const auto pack = source.save_pads();

    nirbija::SamplerInstance target;
    target.set_channel_layout(2);
    target.activate(kRate, kBlock);
    expect(target.load_pads(pack), "load_pads rejected a blob with fades");
    expect(std::fabs(target.pad_fade_in(0) - 0.3) < 1e-6,
           "fade-in did not travel in a pack");
    expect(std::fabs(target.pad_fade_out(0) - 0.7) < 1e-6,
           "fade-out did not travel in a pack");
  }

  // --- assigning a note already taken swaps, so two pads never share a key --
  {
    nirbija::SamplerInstance sampler;
    expect(sampler.pad_note(0) == 36, "factory pad 0 was not Kick");
    expect(sampler.pad_note(1) == 38, "factory pad 1 was not Snare");
    sampler.assign_note(0, 38);
    expect(sampler.pad_note(0) == 38, "pad 0 did not take the snare note");
    expect(sampler.pad_note(1) == 36, "pad 1 did not receive the swapped note");
    sampler.assign_note(0, 60);
    expect(sampler.pad_note(0) == 60, "pad 0 did not take a free note");
    expect(sampler.pad_note(1) == 36, "a free note still swapped");
  }

  // --- a MIDI hit focuses that pad, so Rec and the knobs follow the stick --
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    expect(sampler.focused() == 0, "focus did not start on pad 0");
    note_on(sampler, 42, 100);  // Closed Hat, pad 2
    run(sampler);
    expect(sampler.focused() == 2, "a MIDI hit did not focus its pad");
    expect(sampler.last_midi_note() == 42, "the last note was not remembered");
  }

  // --- a 4x4 controller note no pad owns still lights the chromatic cell ----
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    note_on(sampler, 13, 127);  // SMC-PAD BLE performance notes sit here
    run(sampler);
    expect(sampler.focused() == 5,
           "a low pad-controller note did not focus its cell");
    sampler.load(8, tone.string());  // chromatic C1+8 = MIDI 44
    note_on(sampler, 44, 127);
    expect(run(sampler) > 0.2f, "an unowned C1-grid note did not play its cell");
    expect(sampler.focused() == 8, "an unowned C1-grid note did not focus its cell");
    expect((sampler.hit_flash_mask() & (1u << 8)) != 0,
           "an unowned C1-grid note did not flash its cell");
    // A note a pad already claims still goes there, not to the chromatic cell.
    note_on(sampler, 38, 127);  // Snare, pad 1, which is also chromatic pad 2
    run(sampler);
    expect(sampler.focused() == 1, "an owned note was stolen by the C1 grid");
  }

  // --- hear_note (editor listening on the control port) lights the pad ------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.hear_note(44, 100, true);
    expect(sampler.last_midi_note() == 44, "hear_note did not store the note");
    expect(sampler.focused() == 8, "hear_note did not focus the chromatic cell");
    run(sampler);
    expect((sampler.hit_flash_mask() & (1u << 8)) != 0,
           "hear_note did not flash the pad");
    sampler.hear_cc(21);
    expect(sampler.last_midi_note() == -1, "a CC did not clear the last note");
    expect(sampler.last_midi_cc() == 21, "hear_cc did not store the CC");
  }

  // --- an empty pad still flashes on a matching note, so wiring a MIDI ------
  // controller can be confirmed by eye even before any sample is loaded ------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    expect(!sampler.pad_has_audio(0), "pad 0 unexpectedly had audio");
    note_on(sampler, 36, 100);  // pad 0's factory note, Kick
    run(sampler);
    expect((sampler.hit_flash_mask() & 0x1) != 0,
           "an empty pad did not flash on its own note");
    expect((sampler.sounding_mask() & 0x1) == 0,
           "an empty pad reported sounding");

    // The flash is not a held light: it clears itself out.
    for (int i = 0; i < 80 && (sampler.hit_flash_mask() & 0x1) != 0; ++i)
      run(sampler);
    expect((sampler.hit_flash_mask() & 0x1) == 0,
           "the hit flash never cleared");
  }

  // --- a loaded pad flashes too, on top of actually sounding -----------------
  {
    nirbija::SamplerInstance sampler;
    sampler.set_channel_layout(2);
    sampler.activate(kRate, kBlock);
    sampler.load(0, tone.string());
    note_on(sampler, 36, 100);
    run(sampler);
    expect((sampler.hit_flash_mask() & 0x1) != 0,
           "a loaded pad did not flash on its own note");
    expect((sampler.sounding_mask() & 0x1) != 0,
           "a loaded pad did not report sounding");
  }

  // Trocar o sample de um pad aposenta o buffer anterior em vez de libera-lo na
  // hora: a thread de audio pode estar lendo dele. O portao dessa espera conta
  // blocos de process(), que so o callback do JACK move - entao sem servidor de
  // audio ele nunca abria, e cada troca ficava retida ate o processo sair. Sao
  // ate 3 MB por sample.
  //
  // Ninguem de dentro do plugin distingue "nao ha thread de audio" de "ha uma
  // que ainda nao chegou ao primeiro bloco", e liberar no segundo caso seria
  // use-after-free. Por isso quem sabe e o host, e ele diz:
  // PluginInstance::reclaim_retired(bool).
  {
    auto sampler = std::make_unique<nirbija::SamplerInstance>();
    // De proposito sem activate() nem process(): e o estado "sem servidor".
    expect(sampler->load(0, tone.string()), "could not load the tone");

    for (int i = 0; i < 30; ++i) {
      sampler->load(0, tone.string());
      sampler->reclaim_retired(/*audio_running=*/false);
    }
    expect(sampler->retired_count() == 0,
           "swapping a pad's sample with no audio server left " +
               std::to_string(sampler->retired_count()) +
               " buffer(s) retired");
  }

  // E o contrario, que e o que impede a correcao obvia e errada: dizendo que ha
  // thread de audio, sem bloco nenhum ter rodado, nada pode ser liberado.
  {
    auto sampler = std::make_unique<nirbija::SamplerInstance>();
    expect(sampler->load(0, tone.string()), "could not load the tone");
    for (int i = 0; i < 5; ++i) {
      sampler->load(0, tone.string());
      sampler->reclaim_retired(/*audio_running=*/true);
    }
    expect(sampler->retired_count() == 5,
           "a sampler that has not rendered yet must keep its retired buffers "
           "(kept " + std::to_string(sampler->retired_count()) + " of 5)");
  }

  fs::remove(tone);
  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
