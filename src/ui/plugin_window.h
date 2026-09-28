// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

#include "core/plugin.h"

class QSocketNotifier;

namespace nirbija {

namespace hosting {
class ResizablePluginGui;
}

// A top-level X11 window, created with Xlib rather than through Qt, that hosts
// a plugin's own editor as its child.
//
// Qt's own windows turned out not to be usable as a parent here: plugin
// toolkits realize their view against the handle they are given and expect a
// plain X11 window they can own, which is what every working host hands them.
// So this opens its own display connection, pumps its own events, and never
// involves the Qt window system.
//
// Two clocks, neither of them busy: X events are drained when the connection's
// descriptor says there are some (a QSocketNotifier), and the editor's idle()
// runs at 30 Hz. The old single 16 ms timer polled XPending sixty times a
// second per open editor whether or not anything had happened.
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
  // The window is gone: closed through the window manager, or the plugin
  // itself asked for it (idle() returned non-zero), or the connection died.
  void closed();

 private:
  void pump();
  void drainEvents();
  void adoptChild();
  void applyPreferredSize();
  // Resizes the frame. `from_editor` says the size is the editor's own wish,
  // which is the only kind the frame follows for an editor that cannot be
  // resized by hand.
  void resizeTo(int width, int height);
  void applySizeHints(int width, int height);
  void reportChildren() const;
  void takeFocus(unsigned long timestamp);
  void setTransientForMainWindow();
  // The editor's window, if it has made one yet.
  unsigned long firstChild() const;

  std::unique_ptr<PluginGui> gui_;
  // The same object seen through the resizing interface, or null for an
  // editor that has none (LV2).
  hosting::ResizablePluginGui* resizable_ = nullptr;
  bool user_resizable_ = false;
  QString title_;
  QTimer timer_;
  std::unique_ptr<QSocketNotifier> notifier_;

  // Xlib types are kept out of the header so it stays includable anywhere.
  void* display_ = nullptr;
  unsigned long window_ = 0;
  unsigned long delete_atom_ = 0;
  unsigned long take_focus_atom_ = 0;
  unsigned long protocols_atom_ = 0;
  // The frame's size as last set by this side, so a ConfigureNotify that
  // merely echoes it is told apart from one the user dragged.
  int frame_width_ = 0;
  int frame_height_ = 0;
  bool attached_ = false;
  // Set from the Xlib IO-error exit handler when the connection dies.
  bool connection_lost_ = false;
};

}  // namespace nirbija
