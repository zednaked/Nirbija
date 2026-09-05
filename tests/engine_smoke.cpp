// Opens a JACK client, builds a few channels, moves a fader through the command
// queue, and shuts down. Skips (exit 0) when no JACK/PipeWire server is running,
// so the test is usable on a headless build machine.

#include <chrono>
#include <cstdio>
#include <thread>

#include "core/engine.h"

int main() {
  nirbija::Engine engine;
  if (!engine.start("nirbija-smoke")) {
    std::printf("no JACK server available, skipping\n");
    return 77;
  }

  std::printf("sample rate %.0f Hz, block %u frames\n", engine.sample_rate(),
              engine.block_frames());

  if (engine.add_channel("ch1", 2) != 0 || engine.add_channel("ch2", 1) != 1) {
    std::fprintf(stderr, "failed to add channels\n");
    return 1;
  }

  nirbija::EngineCommand fader;
  fader.kind = nirbija::EngineCommand::Kind::SetGain;
  fader.channel = 0;
  fader.value = 0.5f;
  if (!engine.post(fader)) {
    std::fprintf(stderr, "command queue full\n");
    return 1;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // The song position is a count, not frames times tempo: halving the tempo
  // a second in must not throw the position back half a bar, and the beat
  // must keep moving at the new rate.
  {
    nirbija::EngineCommand play;
    play.kind = nirbija::EngineCommand::Kind::SetPlaying;
    play.value = 1.0f;
    engine.post(play);
    nirbija::EngineCommand tempo;
    tempo.kind = nirbija::EngineCommand::Kind::SetTempo;
    tempo.value = 120.0f;
    engine.post(tempo);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const double before = engine.transport_beats();
    tempo.value = 60.0f;
    engine.post(tempo);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const double after = engine.transport_beats();
    // 50 ms at 60 BPM is 0.05 beats, plus whatever scheduling adds; a jump
    // from re-deriving the position would be a whole beat either way.
    if (after < before || after - before > 0.5) {
      std::fprintf(stderr, "position jumped on a tempo change: %.3f -> %.3f\n",
                   before, after);
      return 1;
    }
    if (before < 1.0) {
      std::fprintf(stderr, "position did not advance while playing: %.3f\n",
                   before);
      return 1;
    }
  }

  engine.stop();
  std::printf("ok\n");
  return 0;
}
