#pragma once

#include <QTimer>
#include <QWindow>

#include <memory>

#include "core/plugin.h"

namespace nirbija {

// A native window that hosts a plugin's own editor as a child. The editor is
// an X11 window, so this only works when the host is running on X11 or
// XWayland — on a native Wayland surface there is no window id to hand over.
class PluginWindow : public QWindow {
  Q_OBJECT

 public:
  PluginWindow(std::unique_ptr<PluginGui> gui, const QString& title);
  ~PluginWindow() override;

  // False when the editor refused to embed, in which case the window is not
  // worth showing.
  bool open();

 protected:
  void exposeEvent(QExposeEvent* event) override;

 private:
  std::unique_ptr<PluginGui> gui_;
  QTimer idle_timer_;
  bool attached_ = false;
};

}  // namespace nirbija
