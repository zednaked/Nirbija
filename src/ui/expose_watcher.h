// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QWindow>
#include <qqmlintegration.h>

namespace nirbija {

// Whether a window is actually being shown by the compositor. QML's
// Window.visibility reports Minimized, but a workspace switched away under
// Hyprland - or any compositor that simply stops asking for frames - never
// says so, and the meters kept redrawing for a screen nobody could see.
// The expose event is the one honest signal: the platform sends it when the
// window's frames start and stop being wanted.
class ExposeWatcher : public QObject {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(QWindow* window READ window WRITE setWindow NOTIFY windowChanged)
  Q_PROPERTY(bool exposed READ exposed NOTIFY exposedChanged)

 public:
  explicit ExposeWatcher(QObject* parent = nullptr);

  QWindow* window() const { return window_; }
  void setWindow(QWindow* window);
  bool exposed() const { return exposed_; }

 signals:
  void windowChanged();
  void exposedChanged();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void refresh();

  QWindow* window_ = nullptr;
  bool exposed_ = true;
};

}  // namespace nirbija
