#include "plugin_window.h"

// Qt headers come first on purpose: Xlib defines a `Status` macro that breaks
// QTextStream if X11 is included ahead of it.
#include <QDebug>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace nirbija {
namespace {

// What to open at when the editor will not say how big it wants to be. LV2 has
// no way to ask before the editor exists.
constexpr int kFallbackWidth = 640;
constexpr int kFallbackHeight = 420;

Display* as_display(void* handle) { return static_cast<Display*>(handle); }

}  // namespace

PluginWindow::PluginWindow(std::unique_ptr<PluginGui> gui, const QString& title,
                           QObject* parent)
    : QObject(parent), gui_(std::move(gui)), title_(title) {
  // Editors expect to be pumped on the main thread; some only repaint here.
  // The same tick drains this window's X events, since nothing else will.
  timer_.setInterval(16);
  connect(&timer_, &QTimer::timeout, this, &PluginWindow::pump);
}

PluginWindow::~PluginWindow() { close(); }

bool PluginWindow::open() {
  if (window_ != 0) return true;

  display_ = XOpenDisplay(nullptr);
  if (display_ == nullptr) return false;

  Display* display = as_display(display_);
  const int screen = DefaultScreen(display);

  window_ = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0,
                                kFallbackWidth, kFallbackHeight, 0,
                                BlackPixel(display, screen),
                                BlackPixel(display, screen));
  if (window_ == 0) {
    close();
    return false;
  }

  XStoreName(display, window_, title_.toUtf8().constData());

  // A window class the window manager can target. Without it these are
  // anonymous X windows, and a tiling compositor has no way to be told to float
  // a plugin editor.
  const QByteArray instance_name = title_.toUtf8();
  XClassHint hint{};
  hint.res_name = const_cast<char*>(instance_name.constData());
  hint.res_class = const_cast<char*>("nirbija-plugin");
  XSetClassHint(display, window_, &hint);

  // Without this the window manager kills the whole connection when the user
  // closes the window, taking the mixer with it.
  delete_atom_ = XInternAtom(display, "WM_DELETE_WINDOW", False);
  Atom delete_atom = delete_atom_;
  XSetWMProtocols(display, window_, &delete_atom, 1);

  // SubstructureNotify is how the child editor's arrival is noticed, which is
  // the moment it can be sized to fill the window.
  XSelectInput(display, window_, StructureNotifyMask | SubstructureNotifyMask);

  XMapWindow(display, window_);
  XFlush(display);

  // The plugin inspects the parent as soon as it is handed over, so the map has
  // to have actually happened rather than merely been requested.
  XEvent event;
  XIfEvent(
      display, &event,
      [](Display*, XEvent* e, XPointer arg) -> Bool {
        return (e->type == MapNotify &&
                e->xmap.window == *reinterpret_cast<Window*>(arg))
                   ? True
                   : False;
      },
      reinterpret_cast<XPointer>(&window_));

  if (!gui_->attach(static_cast<uintptr_t>(window_))) {
    close();
    return false;
  }
  attached_ = true;

  // The plugin creates its editor window but does not always map it, and an
  // unmapped child draws nothing however right the rest of the handshake was.
  // Its own size is also more trustworthy than what get_size reports, which may
  // be in scaled units, so the host window follows the child rather than the
  // other way round.
  adoptChild();
  XFlush(display);

  if (!qEnvironmentVariableIsEmpty("NIRBIJA_DEBUG_EMBED")) reportChildren();

  // Editors that lay out late report their real size a moment after they are
  // handed the parent, so this catches up with them once.
  QTimer::singleShot(250, this, [this] { adoptChild(); });

  timer_.start();
  return true;
}

void PluginWindow::close() {
  timer_.stop();
  if (attached_) {
    gui_->detach();
    attached_ = false;
  }
  if (display_ != nullptr) {
    Display* display = as_display(display_);
    if (window_ != 0) XDestroyWindow(display, window_);
    XCloseDisplay(display);
  }
  window_ = 0;
  display_ = nullptr;
}

// Maps the editor's window and sizes this one to match it.
void PluginWindow::adoptChild() {
  if (display_ == nullptr || window_ == 0) return;
  Display* display = as_display(display_);

  Window root = 0;
  Window parent = 0;
  Window* children = nullptr;
  unsigned int count = 0;
  if (XQueryTree(display, window_, &root, &parent, &children, &count) == 0) return;

  XWindowAttributes own{};
  const bool have_own = XGetWindowAttributes(display, window_, &own) != 0;

  for (unsigned int i = 0; i < count; ++i) {
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, children[i], &attributes) == 0) continue;

    // The host window takes the editor's size — but only when it actually
    // differs. Re-issuing the same resize breeds ConfigureNotify events that
    // arrive back in the pump, and answering those with another resize is the
    // feedback loop that flickered every editor and livelocked DrumGizmo.
    if (attributes.width > 0 && attributes.height > 0 && have_own &&
        (own.width != attributes.width || own.height != attributes.height)) {
      XSizeHints hints{};
      hints.flags = PSize | PMinSize | PMaxSize;
      hints.width = hints.min_width = hints.max_width = attributes.width;
      hints.height = hints.min_height = hints.max_height = attributes.height;
      XSetWMNormalHints(display, window_, &hints);

      XResizeWindow(display, window_, static_cast<unsigned>(attributes.width),
                    static_cast<unsigned>(attributes.height));
    }
    if (attributes.map_state != IsViewable) XMapWindow(display, children[i]);
  }

  if (children != nullptr) XFree(children);
}

// Diagnostics for embedding trouble: says whether the plugin created a child
// window under ours at all, and how big it is.
void PluginWindow::reportChildren() const {
  if (display_ == nullptr || window_ == 0) return;
  Display* display = as_display(display_);

  Window root = 0;
  Window parent = 0;
  Window* children = nullptr;
  unsigned int count = 0;
  if (XQueryTree(display, window_, &root, &parent, &children, &count) == 0) {
    qWarning("embed: XQueryTree failed on 0x%lx", window_);
    return;
  }

  qWarning("embed: parent 0x%lx has %u child window(s)", window_, count);
  for (unsigned int i = 0; i < count; ++i) {
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, children[i], &attributes) == 0) continue;
    qWarning("embed:   child 0x%lx %dx%d at %d,%d map_state=%d", children[i],
             attributes.width, attributes.height, attributes.x, attributes.y,
             attributes.map_state);

    // An editor often nests another window inside its own; if that one is
    // unmapped the visible result is still a black rectangle.
    Window sub_root = 0;
    Window sub_parent = 0;
    Window* grandchildren = nullptr;
    unsigned int sub_count = 0;
    if (XQueryTree(display, children[i], &sub_root, &sub_parent, &grandchildren,
                   &sub_count) != 0) {
      for (unsigned int j = 0; j < sub_count; ++j) {
        XWindowAttributes sub{};
        if (XGetWindowAttributes(display, grandchildren[j], &sub) == 0) continue;
        qWarning("embed:     grandchild 0x%lx %dx%d map_state=%d",
                 grandchildren[j], sub.width, sub.height, sub.map_state);
      }
      if (grandchildren != nullptr) XFree(grandchildren);
    }
  }
  if (children != nullptr) XFree(children);
}

void PluginWindow::pump() {
  if (display_ == nullptr) return;
  Display* display = as_display(display_);

  // Sizing flows one way only: the window follows the editor. Stretching the
  // editor whenever the window changed was the other half of a feedback loop —
  // each side answering the other's ConfigureNotify with a new resize, all
  // inside this drain, which never emptied. The cap is the second belt: even a
  // plugin that floods events cannot keep this loop from returning.
  bool child_changed = false;
  for (int drained = 0; drained < 64 && XPending(display) > 0; ++drained) {
    XEvent event;
    XNextEvent(display, &event);

    switch (event.type) {
      case ClientMessage:
        if (static_cast<Atom>(event.xclient.data.l[0]) == delete_atom_) {
          close();
          emit closed();
          return;
        }
        break;

      case ConfigureNotify:
      case CreateNotify:
      case MapNotify:
        if (event.xany.window != window_) child_changed = true;
        break;

      default:
        break;
    }
  }

  // One adoption per tick, after the drain, so a burst of child events costs
  // one resize decision instead of one per event.
  if (child_changed) adoptChild();

  if (attached_) gui_->idle();
}

}  // namespace nirbija
