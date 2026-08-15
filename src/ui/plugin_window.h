#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

#include "core/plugin.h"

namespace nirbija {

// A top-level X11 window, created with Xlib rather than through Qt, that hosts
// a plugin's own editor as its child.
//
// Qt's own windows turned out not to be usable as a parent here: plugin
// toolkits realize their view against the handle they are given and expect a
// plain X11 window they can own, which is what every working host hands them.
// So this opens its own display connection, pumps its own events, and never
// involves the Qt window system.
class PluginWindow : public QObject {
  Q_OBJECT

 public:
  PluginWindow(std::unique_ptr<PluginGui> gui, const QString& title,
               QObject* parent = nullptr);
  ~PluginWindow() override;

  // False when the editor refused to embed, in which case nothing is shown.
  bool open();
  void close();
  bool isOpen() const { return window_ != 0; }

 signals:
  // The user closed the window through the window manager.
  void closed();

 private:
  void pump();
  void adoptChild();
  void reportChildren() const;

  std::unique_ptr<PluginGui> gui_;
  QString title_;
  QTimer timer_;

  // Xlib types are kept out of the header so it stays includable anywhere.
  void* display_ = nullptr;
  unsigned long window_ = 0;
  unsigned long delete_atom_ = 0;
  bool attached_ = false;
  // Set from the Xlib IO-error exit handler when the connection dies.
  bool connection_lost_ = false;
};

}  // namespace nirbija
