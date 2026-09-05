// State blobs are text, and text with numbers in it is at the mercy of the C
// locale: Qt sets it to the user's on startup, and under pt_BR "%.4f" prints
// "0,5000". Read back by a parser that stops at the comma, that is 0 - and a
// gate of 0 clamps to its floor, is saved as "0,0500", and stays there. Every
// writer here has to produce a full stop whatever the locale says, and the
// reader has to forgive the files already written the other way.

#include <clocale>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "core/fx_pad.h"
#include "core/plugin.h"
#include "core/step_sequencer.h"

namespace {

int failures = 0;

void expect(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what.c_str());
    ++failures;
  }
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-4; }

std::string text_of(const std::vector<uint8_t>& blob) {
  return std::string(blob.begin(), blob.end());
}

}  // namespace

int main() {
  double value = 0.0;
  expect(nirbija::parse_number("0.5", &value) && near(value, 0.5),
         "parse_number lost a plain 0.5");
  expect(nirbija::parse_number("0,5000", &value) && near(value, 0.5),
         "parse_number read a comma decimal as " + std::to_string(value));
  expect(nirbija::parse_number("1,0000", &value) && near(value, 1.0),
         "parse_number mishandled 1,0000");
  expect(nirbija::format_number(0.5, 4) == "0.5000",
         "format_number wrote " + nirbija::format_number(0.5, 4));

  // A locale with a comma decimal, if this machine has one. Without one the
  // writers still get the plain check below.
  const char* comma_locales[] = {"pt_BR.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8",
                                 "es_ES.UTF-8", "it_IT.UTF-8"};
  bool comma = false;
  for (const char* name : comma_locales) {
    if (std::setlocale(LC_ALL, name) != nullptr) {
      comma = true;
      break;
    }
  }
  if (comma) {
    char probe[32];
    std::snprintf(probe, sizeof(probe), "%.1f", 0.5);
    comma = probe[1] == ',';
  }
  std::printf("%s\n", comma ? "running under a comma locale"
                            : "no comma locale installed; plain check only");

  // --- the sequencer's gate survives a save --------------------------------
  {
    const auto seq = std::make_unique<nirbija::StepSequencerInstance>();
    seq->set_lane(0, 36, 16, 2, 0, 0, false, 0.5);
    seq->set_lane(1, 38, 16, 2, 0, 0, false, 0.75);
    seq->set_cell(0, 0, 3, 60, 100, true, 0.25f, false, false);
    seq->set_trig(0, 0, 3, 0.125f, 1, 0, 0);
    seq->set_parameter(5, 0.3);  // swing
    const std::vector<uint8_t> blob = seq->save_state();
    const std::string text = text_of(blob);
    expect(text.find(',') == std::string::npos,
           "sequencer state carries a comma: " + text.substr(0, 120));
    expect(text.find("0.5000") != std::string::npos,
           "sequencer state does not spell the gate 0.5000");

    const auto back = std::make_unique<nirbija::StepSequencerInstance>();
    expect(back->load_state(blob), "sequencer refused its own state");
    expect(near(back->lane_gate(0), 0.5),
           "gate came back as " + std::to_string(back->lane_gate(0)));
    expect(near(back->lane_gate(1), 0.75), "second gate lost");
    expect(near(back->cell_probability(0, 0, 3), 0.25), "probability lost");
    expect(near(back->cell_microtiming(0, 0, 3), 0.125), "microtiming lost");
    expect(near(back->swing(), 0.3), "swing lost");
  }

  // --- a file written the old way still reads right ------------------------
  {
    const std::string old =
        "version 2\nswing 0,2500\nlane 0 36 16 2 0 0 0 0,5000 0\n"
        "pstep 0 0 1 60 100 1 0,7500 0 0 0,1250 1 0 0\n";
    const auto seq = std::make_unique<nirbija::StepSequencerInstance>();
    expect(seq->load_state(std::vector<uint8_t>(old.begin(), old.end())),
           "old-style state refused");
    expect(near(seq->lane_gate(0), 0.5), "old-style gate did not read as 0.5");
    expect(near(seq->swing(), 0.25), "old-style swing did not read as 0.25");
    expect(near(seq->cell_probability(0, 0, 1), 0.75),
           "old-style probability did not read as 0.75");
    expect(near(seq->cell_microtiming(0, 0, 1), 0.125),
           "old-style micro did not read as 0.125");
  }

  // --- the fx pad's amounts ----------------------------------------------
  {
    nirbija::FxPadInstance fx;
    fx.set_pad_amount(0, 0.35f);
    const std::vector<uint8_t> blob = fx.save_state();
    expect(text_of(blob).find(',') == std::string::npos,
           "fx pad state carries a comma: " + text_of(blob));
    nirbija::FxPadInstance back;
    expect(back.load_state(blob), "fx pad refused its own state");
    expect(near(back.pad_amount(0), 0.35),
           "fx pad amount came back as " + std::to_string(back.pad_amount(0)));
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
