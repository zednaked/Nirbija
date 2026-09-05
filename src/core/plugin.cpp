#include "core/plugin.h"

#if NIRBIJA_HAVE_LV2
#include "hosting/lv2_backend.h"
#endif
#if NIRBIJA_HAVE_CLAP
#include "hosting/clap_backend.h"
#endif
#if NIRBIJA_HAVE_VST3
#include "hosting/vst3_backend.h"
#endif

#include "core/file_player.h"
#include "core/looper.h"
#include "core/arpeggiator.h"
#include "core/script_plugin.h"
#include "core/step_sequencer.h"
#include "core/fx_pad.h"
#include "core/keyboard_instrument.h"
#include "core/sampler.h"
#include "core/chord.h"

#include <algorithm>
#include <charconv>

namespace nirbija {

PluginKind kind_from_ports(int audio_inputs, int audio_outputs,
                           bool has_midi_input) {
  if (audio_inputs == 0 && audio_outputs == 0)
    return has_midi_input ? PluginKind::MidiEffect : PluginKind::Unknown;
  if (audio_inputs == 0 && audio_outputs > 0)
    return has_midi_input ? PluginKind::Instrument : PluginKind::Utility;
  if (audio_outputs == 0) return PluginKind::Analyzer;
  return PluginKind::Effect;
}

bool parse_number(std::string_view text, double* out) {
  // Leading blanks are the one thing from_chars will not skip, and a state
  // blob written with a space after its separator is not corrupt.
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                           text.front() == '\r' || text.front() == '\n'))
    text.remove_prefix(1);
  if (text.empty()) return false;

  // A number written under a locale whose decimal mark is a comma. The
  // writers here no longer produce one, but sessions written before they
  // stopped are on disk, and "0,5" read as 0 is how every gate in them
  // collapsed to the floor.
  char fixed[64];
  if (text.size() < sizeof(fixed) &&
      text.find(',') != std::string_view::npos &&
      text.find('.') == std::string_view::npos) {
    for (size_t i = 0; i < text.size(); ++i)
      fixed[i] = text[i] == ',' ? '.' : text[i];
    text = std::string_view(fixed, text.size());
  }

  double value = 0.0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{}) return false;
  if (out != nullptr) *out = value;
  return true;
}

std::string format_number(double value, int decimals) {
  char buffer[64];
  const auto result = std::to_chars(
      buffer, buffer + sizeof(buffer), value, std::chars_format::fixed,
      std::clamp(decimals, 0, 20));
  if (result.ec != std::errc{}) return "0";
  return std::string(buffer, result.ptr);
}

namespace {

// The plugins built into the host itself. First in scan order so they head the
// picker rather than drowning among two hundred effects.
class InternalBackend : public PluginBackend {
 public:
  PluginFormat format() const override { return PluginFormat::Internal; }

  std::vector<PluginDescriptor> scan() override {
    return {FilePlayerInstance::make_descriptor(), LooperInstance::make_descriptor(),
            FxPadInstance::make_descriptor(),
            StepSequencerInstance::make_descriptor(),
            ArpeggiatorInstance::make_descriptor(),
            ScriptInstance::make_descriptor(),
            KeyboardInstrumentInstance::make_descriptor(),
            SamplerInstance::make_descriptor(),
            ChordInstance::make_descriptor()};
  }

  std::unique_ptr<PluginInstance> instantiate(
      const PluginDescriptor& desc) override {
    if (desc.uid == "nirbija.fileplayer")
      return std::make_unique<FilePlayerInstance>();
    if (desc.uid == "nirbija.looper") return std::make_unique<LooperInstance>();
    if (desc.uid == "nirbija.fxpad") return std::make_unique<FxPadInstance>();
    if (desc.uid == "nirbija.stepseq")
      return std::make_unique<StepSequencerInstance>();
    if (desc.uid == "nirbija.arp")
      return std::make_unique<ArpeggiatorInstance>();
    if (desc.uid == "nirbija.script")
      return std::make_unique<ScriptInstance>();
    if (desc.uid == "nirbija.keyboard")
      return std::make_unique<KeyboardInstrumentInstance>();
    if (desc.uid == "nirbija.sampler")
      return std::make_unique<SamplerInstance>();
    if (desc.uid == "nirbija.chord")
      return std::make_unique<ChordInstance>();
    return nullptr;
  }
};

}  // namespace

std::vector<std::unique_ptr<PluginBackend>> make_all_backends() {
  std::vector<std::unique_ptr<PluginBackend>> backends;
  backends.push_back(std::make_unique<InternalBackend>());
#if NIRBIJA_HAVE_LV2
  backends.push_back(make_lv2_backend());
#endif
#if NIRBIJA_HAVE_CLAP
  backends.push_back(make_clap_backend());
#endif
#if NIRBIJA_HAVE_VST3
  backends.push_back(make_vst3_backend());
#endif
  return backends;
}

}  // namespace nirbija
