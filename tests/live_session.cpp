// Manual tool: brings up one channel with a synth on it, wires a MIDI source
// into the channel and the master bus to the speakers, and prints the level
// while you play. Not a ctest target — it needs hardware and a human.
//
//   nirbija_live [synth name] [midi source substring] [seconds]

#include <jack/jack.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "core/engine.h"
#include "core/plugin.h"

namespace {

std::unique_ptr<nirbija::PluginInstance> find_plugin(const std::string& name) {
  for (auto& backend : nirbija::make_all_backends()) {
    for (const auto& descriptor : backend->scan()) {
      if (descriptor.name != name) continue;
      auto instance = backend->instantiate(descriptor);
      if (instance != nullptr) return instance;
    }
  }
  return nullptr;
}

// Connects the first port matching `pattern` to `target`, reporting what it
// picked so a wrong guess is visible rather than silent.
bool connect_matching(jack_client_t* client, const char* type, unsigned long flags,
                      const std::string& pattern, const std::string& target,
                      bool source_is_pattern) {
  const char** ports = jack_get_ports(client, nullptr, type, flags);
  bool connected = false;

  for (const char** port = ports; port != nullptr && *port != nullptr; ++port) {
    const std::string name = *port;
    if (name.find(pattern) == std::string::npos) continue;
    if (name.find("nirbija") != std::string::npos) continue;  // never loop back

    const std::string from = source_is_pattern ? name : target;
    const std::string to = source_is_pattern ? target : name;
    if (jack_connect(client, from.c_str(), to.c_str()) == 0) {
      std::printf("  %s -> %s\n", from.c_str(), to.c_str());
      connected = true;
      break;
    }
  }

  if (ports != nullptr) jack_free(ports);
  return connected;
}

std::string find_own_port(jack_client_t* client, const char* type,
                          unsigned long flags, const std::string& suffix) {
  const char** ports = jack_get_ports(client, "nirbija-live", type, flags);
  std::string found;
  for (const char** port = ports; port != nullptr && *port != nullptr; ++port) {
    const std::string name = *port;
    if (name.size() >= suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      found = name;
      break;
    }
  }
  if (ports != nullptr) jack_free(ports);
  return found;
}

}  // namespace

int main(int argc, char* argv[]) {
  const std::string synth_name = argc > 1 ? argv[1] : "Odin2";
  const std::string midi_pattern = argc > 2 ? argv[2] : "FM-1";
  const int seconds = argc > 3 ? std::stoi(argv[3]) : 60;

  nirbija::Engine engine;
  if (!engine.start("nirbija-live")) {
    std::fprintf(stderr, "no JACK server available\n");
    return 1;
  }
  std::printf("engine: %.0f Hz, %u frames\n", engine.sample_rate(),
              engine.block_frames());

  const size_t channel = engine.add_channel("synth", 2);
  if (channel == nirbija::kMaxChannels) {
    std::fprintf(stderr, "could not add a channel\n");
    return 1;
  }

  auto synth = find_plugin(synth_name);
  if (synth == nullptr) {
    std::fprintf(stderr, "plugin not found: %s\n", synth_name.c_str());
    return 1;
  }
  if (!engine.graph().channel(channel).add_insert(std::move(synth))) {
    std::fprintf(stderr, "could not load %s as an insert\n", synth_name.c_str());
    return 1;
  }
  std::printf("loaded %s on channel 1\n", synth_name.c_str());

  std::printf("connections:\n");
  const std::string midi_in =
      find_own_port(engine.client(), JACK_DEFAULT_MIDI_TYPE, JackPortIsInput,
                    "1_midi_in");
  if (midi_in.empty() ||
      !connect_matching(engine.client(), JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput,
                        midi_pattern, midi_in, true)) {
    std::fprintf(stderr, "  no MIDI source matching \"%s\"\n", midi_pattern.c_str());
  }

  for (const char* side : {"master_out_l", "master_out_r"}) {
    const std::string out = find_own_port(engine.client(), JACK_DEFAULT_AUDIO_TYPE,
                                          JackPortIsOutput, side);
    if (out.empty()) continue;
    connect_matching(engine.client(), JACK_DEFAULT_AUDIO_TYPE,
                     JackPortIsInput | JackPortIsPhysical,
                     std::string(side).back() == 'l' ? "_FL" : "_FR", out, false);
  }

  std::printf("\nplay something. %d seconds.\n", seconds);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  float loudest = 0.0f;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const float peak = engine.graph().read_master_peak(0);
    loudest = std::max(loudest, peak);

    // A crude meter, so the terminal shows the same thing the speakers do.
    const int bars = static_cast<int>(peak * 40.0f);
    std::printf("\r[%-40s] %.4f  peak %.4f",
                std::string(std::max(0, std::min(40, bars)), '#').c_str(), peak,
                loudest);
    std::fflush(stdout);
  }

  std::printf("\nloudest master peak: %.6f\n", loudest);
  engine.stop();
  return loudest > 1e-4f ? 0 : 1;
}
