#include "plugin_window.h"

#include <QGuiApplication>

namespace nirbija {
namespace {

// What to open at when the editor will not say how big it wants to be. LV2 has
// no way to ask before the editor exists.
constexpr int kFallbackWidth = 640;
constexpr int kFallbackHeight = 420;

}  // namespace

PluginWindow::PluginWindow(std::unique_ptr<PluginGui> gui, const QString& title)
    : gui_(std::move(gui)) {
  setTitle(title);
  resize(kFallbackWidth, kFallbackHeight);

  // Editors expect to be pumped on the main thread; some only repaint here.
  idle_timer_.setInterval(33);
  connect(&idle_timer_, &QTimer::timeout, this, [this] {
    if (attached_) gui_->idle();
  });
}

PluginWindow::~PluginWindow() {
  idle_timer_.stop();
  if (attached_) gui_->detach();
}

bool PluginWindow::open() {
  // The window has to be created and mapped before the editor goes in: a
  // toolkit like Pugl inspects the parent when it realizes its own view, and
  // fails outright against an unmapped one.
  create();
  show();

  // show() only queues the map request. Plugin toolkits inspect the parent the
  // moment they are handed it, so the round trip has to complete first or they
  // realize against a window the server has not mapped yet.
  QGuiApplication::processEvents();
  QGuiApplication::sync();

  const WId id = winId();
  if (id == 0) return false;

  if (!gui_->attach(static_cast<uintptr_t>(id))) {
    hide();
    return false;
  }
  attached_ = true;

  int width = 0;
  int height = 0;
  if (gui_->preferred_size(&width, &height) && width > 0 && height > 0)
    resize(width, height);

  idle_timer_.start();
  return true;
}

void PluginWindow::exposeEvent(QExposeEvent* event) {
  QWindow::exposeEvent(event);
  if (!isExposed()) return;
  // Some editors only lay themselves out once their parent is mapped, so this
  // is the first useful moment to pump one.
  if (attached_) gui_->idle();
}

}  // namespace nirbija
