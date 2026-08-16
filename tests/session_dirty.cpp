// A plugin that changes its own state — a sampler handed a new kit from its
// own window — goes through none of MixerModel's setters. Nothing marked the
// session dirty, so the autosave never fired and the change only survived if
// something unrelated happened to save afterwards.
//
// This drives the real path: an insert reports state_dirty, the level poll
// picks it up, and the session becomes dirty on its own.

#include <QGuiApplication>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QCoreApplication>

#include <cstdio>
#include <string>

#include "mixer_model.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

// Stands in for any plugin whose editor rewrote its state behind the host's
// back. Everything else about it is inert.
class DirtyPlugin : public nirbija::PluginInstance {
 public:
  void set_channel_layout(int) override {}
  bool activate(double, uint32_t) override { return true; }
  void deactivate() override {}
  void process(const float* const*, float* const*, uint32_t) override {}

  std::vector<nirbija::ParameterInfo> parameters() const override { return {}; }
  double parameter_value(uint32_t) const override { return 0.0; }
  void set_parameter(uint32_t, double) override {}

  std::vector<uint8_t> save_state() const override { return {}; }
  bool load_state(const std::vector<uint8_t>&) override { return true; }

  const nirbija::PluginDescriptor& descriptor() const override { return desc_; }

  void announce() { dirty_ = true; }
  bool take_state_dirty() override {
    const bool was = dirty_;
    dirty_ = false;
    return was;
  }

 private:
  bool dirty_ = false;
  // Named rather than positional: a new field in PluginDescriptor should not
  // silently shift what this test thinks it is saying.
  nirbija::PluginDescriptor desc_{.format = nirbija::PluginFormat::Internal,
                                  .uid = "test.dirty",
                                  .name = "Dirty",
                                  .vendor = "Nirbija",
                                  .kind = nirbija::PluginKind::Effect,
                                  .audio_inputs = 2,
                                  .audio_outputs = 2};
};

// The poll runs on a 33 ms timer; give it a few turns of the event loop.
bool wait_for_dirty(nirbija::MixerModel& mixer, int milliseconds) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < milliseconds) {
    QCoreApplication::processEvents();
    if (mixer.dirty()) return true;
  }
  return mixer.dirty();
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

  mixer.addChannel(QStringLiteral("Ch"), 2);
  nirbija::ChannelStrip& strip = mixer.engineForTests().graph().channel(0);

  auto plugin = std::make_unique<DirtyPlugin>();
  DirtyPlugin* raw = plugin.get();
  if (!strip.add_insert(std::move(plugin))) {
    fail("could not add the stand-in insert");
    return 1;
  }

  mixer.saveSession();
  if (mixer.dirty()) {
    fail("the session was still dirty right after a save");
    return 1;
  }

  // Nothing on the model is touched: only the plugin says it moved.
  raw->announce();

  if (!wait_for_dirty(mixer, 500))
    fail("a plugin's own state change never reached the session");

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
