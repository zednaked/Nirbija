#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/fx_pad.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

float run(nirbija::FxPadInstance& fx, float level) {
  constexpr uint32_t kBlock = 256;
  std::vector<float> in_l(kBlock, level), in_r(kBlock, level);
  std::vector<float> out_l(kBlock), out_r(kBlock);
  const float* ins[2] = {in_l.data(), in_r.data()};
  float* outs[2] = {out_l.data(), out_r.data()};
  nirbija::TransportInfo transport;
  transport.playing = true;
  transport.rolling = true;
  transport.tempo_bpm = 120.0;
  fx.set_transport(transport);
  fx.process(ins, outs, kBlock);
  float peak = 0.0f;
  for (float s : out_l) peak = std::max(peak, std::fabs(s));
  return peak;
}

}  // namespace

int main() {
  nirbija::FxPadInstance fx;
  fx.set_channel_layout(2);
  fx.activate(48000.0, 256);

  if (run(fx, 0.5f) < 0.45f) fail("dry signal does not pass through");

  fx.set_pad(nirbija::FxPadInstance::Dirty, true);
  float dirty = 0.0f;
  for (int i = 0; i < 8; ++i) dirty = std::max(dirty, run(fx, 0.2f));
  if (dirty < 0.25f) fail("dirty did not saturate");

  fx.set_pad(nirbija::FxPadInstance::Dirty, false);
  for (int i = 0; i < 8; ++i) run(fx, 0.0f);
  if (run(fx, 0.0f) > 1e-3f) fail("silence in is not silence out with pads off");

  fx.set_hold(true);
  fx.set_pad(nirbija::FxPadInstance::Crush, true);
  if (!fx.pad_on(nirbija::FxPadInstance::Crush)) fail("hold did not keep crush down");
  fx.set_hold(false);
  if (fx.pad_on(nirbija::FxPadInstance::Crush)) fail("dropping hold left crush on");

  const auto blob = fx.save_state();
  fx.set_pad(nirbija::FxPadInstance::Reverb, true);
  fx.set_hold(true);
  if (!fx.load_state(blob)) fail("load_state refused its own blob");
  if (fx.hold()) fail("restored hold from a blob that had it off");
  if (fx.pad_on(nirbija::FxPadInstance::Reverb))
    fail("a pad turned on after save survived a load of the old state");

  fx.set_hold(true);
  fx.set_pad(nirbija::FxPadInstance::Dirty, true);
  fx.set_pad(nirbija::FxPadInstance::Reverb, true);
  const auto kept = fx.save_state();
  nirbija::FxPadInstance restored;
  restored.activate(48000.0, 256);
  if (!restored.load_state(kept)) fail("load_state refused a blob with pads on");
  if (!restored.hold()) fail("hold did not survive the session");
  if (!restored.pad_on(nirbija::FxPadInstance::Dirty) ||
      !restored.pad_on(nirbija::FxPadInstance::Reverb))
    fail("latched pads did not come back");

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
