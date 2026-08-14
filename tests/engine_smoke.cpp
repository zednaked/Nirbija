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
    return 0;
  }

  std::printf("sample rate %.0f Hz, block %u frames\n", engine.sample_rate(),
              engine.block_frames());

  engine.graph().add_channel("ch 1", 2);
  engine.graph().add_channel("ch 2", 2);

  if (!engine.post({nirbija::EngineCommand::Kind::SetGain, 0, 0.5f})) {
    std::fprintf(stderr, "command queue full\n");
    return 1;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  engine.stop();
  std::printf("ok\n");
  return 0;
}
