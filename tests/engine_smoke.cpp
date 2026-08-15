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
  engine.stop();
  std::printf("ok\n");
  return 0;
}
