#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// Teclado partido em duas zonas: abaixo do split, cada tecla dispara o
// acorde diatônico do grau da escala corrente; no split e acima, a nota
// passa como melodia, quantizada à escala se pedido. `design/chord.md`.
//
// Diferente do Step Sequencer, este plugin não gera nada sozinho — só
// reage ao que chega em `queue_midi`, que já roda na thread de áudio, como
// o Arpeggiator. Cada nota de entrada é indexada direto (0-127), sem
// busca: `origin_` lembra em qual zona a tecla foi tocada, para o
// note-off soltar a coisa certa mesmo que o split mude enquanto a tecla
// está presa.
class ChordInstance : public PluginInstance {
 public:
  ChordInstance();

  static PluginDescriptor make_descriptor();

  static constexpr int kMaxVoices = 4;

  enum Scale : int {
    Major = 0,  // Ionian
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    NaturalMinor,  // Aeolian
    Locrian,
    HarmonicMinor,
    MelodicMinor,
    PentatonicMajor,
    PentatonicMinor,
    Blues,
    ScaleCount,
  };

  // PluginInstance ------------------------------------------------------------
  void set_channel_layout(int) override {}
  bool activate(double sample_rate, uint32_t max_block_frames) override;
  void deactivate() override;

  void queue_midi(const MidiEvent& event) override;
  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override;
  size_t take_midi_output(MidiEvent* out, size_t capacity) override;

  std::vector<ParameterInfo> parameters() const override;
  double parameter_value(uint32_t id) const override;
  void set_parameter(uint32_t id, double value) override;

  std::vector<uint8_t> save_state() const override;
  bool load_state(const std::vector<uint8_t>& blob) override;

  const PluginDescriptor& descriptor() const override { return descriptor_; }

 private:
  static constexpr size_t kMaxEvents = 128;

  enum class Origin : uint8_t { None = 0, Trigger, Passthrough };

  void emit(uint32_t frame, uint8_t status, uint8_t data1, uint8_t data2);
  void handle_trigger_on(uint32_t frame, uint8_t note, uint8_t velocity);
  void handle_trigger_off(uint32_t frame, uint8_t note);
  void handle_passthrough_on(uint32_t frame, uint8_t note, uint8_t velocity);
  void handle_passthrough_off(uint32_t frame, uint8_t note);

  PluginDescriptor descriptor_;

  // --- set by the UI thread, read by the audio thread ------------------------
  std::atomic<int> root_{0};
  std::atomic<int> scale_{Major};
  std::atomic<int> split_{60};
  std::atomic<int> octave_{0};
  std::atomic<int> inversion_{0};
  std::atomic<int> voices_{3};
  std::atomic<int> spread_{0};
  std::atomic<int> passthrough_{0};
  std::atomic<int> channel_{0};

  // --- audio thread only -------------------------------------------------
  // Direct-indexed by the input note (0-127): no search, no growth.
  std::array<Origin, 128> origin_{};
  std::array<std::array<uint8_t, kMaxVoices>, 128> trigger_voices_{};
  std::array<uint8_t, 128> trigger_voice_count_{};
  std::array<int16_t, 128> passthrough_pitch_{};

  std::array<MidiEvent, kMaxEvents> events_{};
  size_t event_count_ = 0;
};

}  // namespace nirbija
