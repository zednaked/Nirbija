// Manual tool: opens one plugin's editor in a host window so the embedding can
// be looked at. Not a ctest target — it needs a real X11 display and a human.
//
//   nirbija_gui_probe "Dragonfly Hall Reverb"

#include <QGuiApplication>
#include <QTimer>

#include <cstdio>

#include "core/plugin.h"
#include "platform.h"
#include "plugin_window.h"

int main(int argc, char* argv[]) {
  nirbija::force_x11_platform();

  QGuiApplication app(argc, argv);
  std::printf("platform: %s\n", app.platformName().toUtf8().constData());
  const QStringList args = app.arguments();
  if (args.size() < 2) {
    std::fprintf(stderr, "usage: %s <plugin name> [seconds]\n",
                 argv[0]);
    return 2;
  }
  const std::string wanted = args[1].toStdString();
  const int seconds = args.size() > 2 ? args[2].toInt() : 8;

  std::unique_ptr<nirbija::PluginInstance> instance;
  for (auto& backend : nirbija::make_all_backends()) {
    for (const auto& descriptor : backend->scan()) {
      if (descriptor.name != wanted) continue;
      instance = backend->instantiate(descriptor);
      if (instance != nullptr) break;
    }
    if (instance != nullptr) break;
  }

  if (instance == nullptr) {
    std::fprintf(stderr, "plugin not found: %s\n", wanted.c_str());
    return 1;
  }

  // An editor usually wants a running plugin behind it.
  instance->set_channel_layout(2);
  instance->activate(48000.0, 256);

  std::unique_ptr<nirbija::PluginGui> gui = instance->create_gui();
  if (gui == nullptr) {
    std::fprintf(stderr, "%s ships no editor this host can embed\n",
                 wanted.c_str());
    return 1;
  }

  nirbija::PluginWindow window(std::move(gui), args[1]);
  std::fflush(stdout);
  if (!window.open()) {
    std::fprintf(stderr, "%s refused to embed its editor\n", wanted.c_str());
    return 1;
  }

  std::printf("embedded %s, closing in %d s\n", wanted.c_str(), seconds);
  QTimer::singleShot(seconds * 1000, &app, &QGuiApplication::quit);
  return app.exec();
}
