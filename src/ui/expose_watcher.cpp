// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "expose_watcher.h"

#include <QEvent>

namespace nirbija {

ExposeWatcher::ExposeWatcher(QObject* parent) : QObject(parent) {}

void ExposeWatcher::setWindow(QWindow* window) {
  if (window == window_) return;
  if (window_ != nullptr) window_->removeEventFilter(this);
  window_ = window;
  if (window_ != nullptr) window_->installEventFilter(this);
  emit windowChanged();
  refresh();
}

bool ExposeWatcher::eventFilter(QObject* watched, QEvent* event) {
  if (watched == window_ &&
      (event->type() == QEvent::Expose || event->type() == QEvent::Hide ||
       event->type() == QEvent::Show))
    refresh();
  return QObject::eventFilter(watched, event);
}

void ExposeWatcher::refresh() {
  // Without a window there is nothing to be hidden by; say shown, so a
  // caller that binds to this before the window exists is not switched off.
  const bool now = window_ == nullptr || window_->isExposed();
  if (now == exposed_) return;
  exposed_ = now;
  emit exposedChanged();
}

}  // namespace nirbija
