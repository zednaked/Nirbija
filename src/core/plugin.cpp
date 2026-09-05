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
#include "core/drone.h"


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
            ChordInstance::make_descriptor(),
            DroneInstance::make_descriptor()};
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
    if (desc.uid == "nirbija.drone") return std::make_unique<DroneInstance>();
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
