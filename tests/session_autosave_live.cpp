// The autosave used to park the graph to read every plugin's state: the
// master fell silent for a few blocks a second after every edit, which on a
// step sequencer grid meant a pop a second after every touch. Saving a
// session must not stop the music. Only a plugin that says its state cannot
// be read under process() - a looper mid-take - gets to park, and only then.
//
// Needs an audio server for the graph to render at all; skips without one,
// the same as the other session tests.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

#include "mixer_model.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

class StubPlugin : public nirbija::PluginInstance {
 public:
  explicit StubPlugin(bool quiet) : quiet_(quiet) {}
  void set_channel_layout(int) override {}
  bool activate(double, uint32_t) override { return true; }
  void deactivate() override {}
  void process(const float* const*, float* const*, uint32_t) override {}
  std::vector<nirbija::ParameterInfo> parameters() const override { return {}; }
  double parameter_value(uint32_t) const override { return 0.0; }
  void set_parameter(uint32_t, double) override {}
  std::vector<uint8_t> save_state() const override {
    ++saves;
    return {1, 2, 3};
  }
  bool load_state(const std::vector<uint8_t>&) override { return true; }
  bool save_needs_quiet() const override { return quiet_; }
  const nirbija::PluginDescriptor& descriptor() const override { return desc_; }

  mutable int saves = 0;

 private:
  bool quiet_;
  nirbija::PluginDescriptor desc_{.format = nirbija::PluginFormat::Internal,
                                  .uid = "test.stub",
                                  .name = "Stub",
                                  .vendor = "Nirbija",
                                  .kind = nirbija::PluginKind::Effect,
                                  .audio_inputs = 2,
                                  .audio_outputs = 2};
};

// The autosave fires a second after the last edit; give it that and a
// little more, turning the event loop the whole way.
void pump(int milliseconds) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < milliseconds) QCoreApplication::processEvents();
}

}  // namespace

int main(int argc, char* argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);

  QTemporaryDir dir;
  if (!dir.isValid()) {
    fail("could not make a temporary directory");
    return 1;
  }
  qputenv("NIRBIJA_SESSION", (dir.path() + "/session.json").toLocal8Bit());

  nirbija::MixerModel mixer;
  if (!mixer.running()) {
    std::printf("no audio server available, skipping\n");
    return 0;
  }

  nirbija::AudioGraph& graph = mixer.engineForTests().graph();
  mixer.addChannel(QStringLiteral("Ch"), 2);
  nirbija::ChannelStrip& strip = graph.channel(0);

  auto plugin = std::make_unique<StubPlugin>(false);
  StubPlugin* live = plugin.get();
  if (!strip.add_insert(std::move(plugin))) {
    fail("could not add the stand-in insert");
    return 1;
  }
  mixer.saveSession();
  pump(100);

  // An ordinary edit, then the autosave it schedules. The graph must render
  // every block of that second with its plugins running.
  const uint64_t quiet_before = graph.quiet_generation();
  const uint64_t rendered_before = graph.render_generation();
  const int saves_before = live->saves;
  mixer.setGain(0, 0.5);
  pump(1500);
  if (live->saves == saves_before)
    fail("the autosave never asked the plugin for its state");
  if (graph.render_generation() == rendered_before)
    fail("the graph did not render at all");
  if (graph.quiet_generation() != quiet_before)
    fail("saving a session with well-behaved plugins parked the graph");

  // A plugin that says it needs quiet gets it, for that save alone.
  auto needy = std::make_unique<StubPlugin>(true);
  if (!strip.add_insert(std::move(needy))) {
    fail("could not add the needy insert");
    return 1;
  }
  const uint64_t quiet_mid = graph.quiet_generation();
  mixer.setGain(0, 0.75);
  pump(1500);
  if (graph.quiet_generation() == quiet_mid)
    fail("a plugin that asked for a quiet save did not get one");
  if (graph.parked()) fail("the graph was left parked after the save");

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
