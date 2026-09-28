// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "plugin_window.h"

// Qt headers come first on purpose: Xlib defines a `Status` macro that breaks
// QTextStream if X11 is included ahead of it.
#include <QDebug>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QSocketNotifier>
#include <QWindow>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <poll.h>

#include <algorithm>
#include <mutex>

#include "hosting/gui_resize.h"

namespace nirbija {
namespace {

// What to open at when the editor will not say how big it wants to be. LV2 has
// no way to ask before the editor exists.
constexpr int kFallbackWidth = 640;
constexpr int kFallbackHeight = 420;
// A child this small has not been laid out yet. DrumGizmo creates its X
// window at 1x1 and only later follows the parent; treating that as the
// editor's size collapses the host window.
constexpr int kMinEditorEdge = 32;

Display* as_display(void* handle) { return static_cast<Display*>(handle); }

// Xlib's default error handler exits the whole process. A plugin editor that
// trips one — a GL context refused by the display is the classic — must not
// take the mixer down with it: log it, let the editor stay black, keep playing.
int log_x_error(Display*, XErrorEvent* error) {
  qWarning("plugin editor X error: request %d.%d code %d — the editor may stay "
           "black. If this is a GL editor, try __GLX_VENDOR_LIBRARY_NAME=mesa.",
           error->request_code, error->minor_code, error->error_code);
  return 0;
}

std::once_flag x_handler_installed;

}  // namespace

PluginWindow::PluginWindow(std::unique_ptr<PluginGui> gui, const QString& title,
                           QObject* parent)
    : QObject(parent), gui_(std::move(gui)), title_(title) {
  // Editors expect to be pumped on the main thread; some only repaint here.
  // 30 Hz, the same beat as the mixer's own meters: an editor that repaints
  // on idle is not smoother at sixty, and the X events no longer wait for
  // this clock - see drainEvents().
  timer_.setInterval(33);
  connect(&timer_, &QTimer::timeout, this, &PluginWindow::pump);
}

PluginWindow::~PluginWindow() { close(); }

bool PluginWindow::open() {
  if (window_ != 0) return true;

  // Installed once, before any plugin can err. This only covers Xlib users —
  // the plugin editors and these windows; Qt itself speaks xcb and is not
  // affected.
  std::call_once(x_handler_installed, [] { XSetErrorHandler(&log_x_error); });

  display_ = XOpenDisplay(nullptr);
  if (display_ == nullptr) return false;

  // A dead connection would otherwise also exit the process. Marking the
  // window dead lets the pump stop touching it instead.
  XSetIOErrorExitHandler(
      as_display(display_),
      [](Display*, void* self) {
        static_cast<PluginWindow*>(self)->connection_lost_ = true;
      },
      this);

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
  frame_width_ = kFallbackWidth;
  frame_height_ = kFallbackHeight;

  XStoreName(display, window_, title_.toUtf8().constData());

  // A window class the window manager can target. Without it these are
  // anonymous X windows, and a tiling compositor has no way to be told to float
  // a plugin editor.
  const QByteArray instance_name = title_.toUtf8();
  XClassHint hint{};
  hint.res_name = const_cast<char*>(instance_name.constData());
  hint.res_class = const_cast<char*>("nirbija-plugin");
  XSetClassHint(display, window_, &hint);

  // Without WM_DELETE_WINDOW the window manager kills the whole connection
  // when the user closes the window, taking the mixer with it. WM_TAKE_FOCUS
  // is how keyboard focus reaches the editor: the manager tells this side
  // the window was chosen, and this side hands the focus to the editor's own
  // window (takeFocus), which is where the keys should go.
  delete_atom_ = XInternAtom(display, "WM_DELETE_WINDOW", False);
  take_focus_atom_ = XInternAtom(display, "WM_TAKE_FOCUS", False);
  protocols_atom_ = XInternAtom(display, "WM_PROTOCOLS", False);
  Atom protocols[2] = {static_cast<Atom>(delete_atom_),
                       static_cast<Atom>(take_focus_atom_)};
  XSetWMProtocols(display, window_, protocols, 2);
  XWMHints wm_hints{};
  wm_hints.flags = InputHint;
  wm_hints.input = True;
  XSetWMHints(display, window_, &wm_hints);

  // A plugin editor belongs to the mixer's window: transient for it, so the
  // manager keeps it above the mixer, minimises the two together and does
  // not list the editor as an application of its own.
  setTransientForMainWindow();

  // SubstructureNotify is how the child editor's arrival is noticed, which is
  // the moment it can be sized to fill the window. StructureNotify on the
  // frame itself is how a resize by hand is seen.
  XSelectInput(display, window_, StructureNotifyMask | SubstructureNotifyMask);

  XMapWindow(display, window_);
  XFlush(display);

  // The plugin inspects the parent as soon as it is handed over, so the map has
  // to have actually happened rather than merely been requested. Timed: a
  // missing MapNotify must not freeze the mixer.
  //
  // Blocked on the connection's own descriptor rather than spun on: the poll
  // sleeps until the server has something to say, where the old loop burned a
  // core for the whole half second whenever the notify never came.
  {
    XEvent event{};
    QElapsedTimer wait;
    wait.start();
    bool mapped = false;
    const int fd = ConnectionNumber(display);
    while (!mapped) {
      if (XCheckTypedWindowEvent(display, window_, MapNotify, &event)) {
        mapped = true;
        break;
      }
      const int left = 500 - static_cast<int>(wait.elapsed());
      if (left <= 0) break;

      XFlush(display);
      pollfd waiting{};
      waiting.fd = fd;
      waiting.events = POLLIN;
      if (poll(&waiting, 1, std::min(left, 50)) < 0) break;
    }
    if (!mapped)
      qWarning("plugin editor: MapNotify timed out, attaching anyway");
  }

  if (!gui_->attach(static_cast<uintptr_t>(window_))) {
    close();
    return false;
  }
  attached_ = true;

  // Editors that can resize (CLAP, VST3) say so through this second
  // interface; LV2's cannot and get a frame of fixed size.
  resizable_ = dynamic_cast<hosting::ResizablePluginGui*>(gui_.get());
  user_resizable_ = resizable_ != nullptr && resizable_->resizable();
  if (resizable_ != nullptr) {
    // VST3 wants the frame resized inside its own request; the handler does
    // it on the spot and the editor is told the size it actually got.
    resizable_->set_resize_handler([this](int* width, int* height) {
      if (window_ == 0 || width == nullptr || height == nullptr) return false;
      resizeTo(*width, *height);
      *width = frame_width_;
      *height = frame_height_;
      return true;
    });
  }

  // DrumGizmo (and a few other LV2 editors) create the child at 1x1 and only
  // tell the real size through ui:resize. Following the child's X geometry
  // then shrinks the host to a speck, which the window manager treats as a
  // close. Honour the requested size first; adoptChild only follows a child
  // that has actually been laid out.
  applyPreferredSize();
  adoptChild();
  XFlush(display);

  if (!qEnvironmentVariableIsEmpty("NIRBIJA_DEBUG_EMBED")) reportChildren();

  // Editors that lay out late report their real size a moment after they are
  // handed the parent, so this catches up with them once.
  QTimer::singleShot(250, this, [this] {
    applyPreferredSize();
    adoptChild();
  });

  // Events arrive when the server has them, not on a clock.
  notifier_ = std::make_unique<QSocketNotifier>(ConnectionNumber(display),
                                                QSocketNotifier::Read);
  connect(notifier_.get(), &QSocketNotifier::activated, this,
          [this] { drainEvents(); });

  timer_.start();
  return true;
}

void PluginWindow::close() {
  timer_.stop();
  notifier_.reset();
  if (attached_) {
    if (resizable_ != nullptr) resizable_->set_resize_handler({});
    gui_->detach();
    attached_ = false;
  }
  resizable_ = nullptr;
  if (display_ != nullptr) {
    Display* display = as_display(display_);
    if (window_ != 0) XDestroyWindow(display, window_);
    XCloseDisplay(display);
  }
  window_ = 0;
  display_ = nullptr;
}

// The mixer's own window, found through Qt: the app is forced onto xcb, so a
// QWindow's winId is the X window the manager knows.
void PluginWindow::setTransientForMainWindow() {
  if (display_ == nullptr || window_ == 0) return;
  for (QWindow* candidate : QGuiApplication::topLevelWindows()) {
    if (candidate == nullptr || !candidate->isVisible()) continue;
    if (candidate->type() != Qt::Window) continue;
    const WId id = candidate->winId();
    if (id == 0) continue;
    XSetTransientForHint(as_display(display_), window_, static_cast<Window>(id));
    return;
  }
}

void PluginWindow::applySizeHints(int width, int height) {
  Display* display = as_display(display_);
  XSizeHints hints{};
  if (user_resizable_) {
    // Only a floor: the frame may be dragged, and the editor is asked to
    // lay out to whatever size that lands on (see drainEvents).
    hints.flags = PSize | PMinSize;
    hints.width = width;
    hints.height = height;
    hints.min_width = kMinEditorEdge;
    hints.min_height = kMinEditorEdge;
  } else {
    hints.flags = PSize | PMinSize | PMaxSize;
    hints.width = hints.min_width = hints.max_width = width;
    hints.height = hints.min_height = hints.max_height = height;
  }
  XSetWMNormalHints(display, window_, &hints);
}

void PluginWindow::resizeTo(int width, int height) {
  if (display_ == nullptr || window_ == 0) return;
  if (width < kMinEditorEdge || height < kMinEditorEdge) return;
  Display* display = as_display(display_);

  frame_width_ = width;
  frame_height_ = height;
  XWindowAttributes own{};
  if (XGetWindowAttributes(display, window_, &own) != 0 &&
      own.width == width && own.height == height)
    return;

  applySizeHints(width, height);
  XResizeWindow(display, window_, static_cast<unsigned>(width),
                static_cast<unsigned>(height));
}

void PluginWindow::applyPreferredSize() {
  if (gui_ == nullptr) return;
  int width = 0;
  int height = 0;
  if (!gui_->preferred_size(&width, &height)) return;
  resizeTo(width, height);
}

unsigned long PluginWindow::firstChild() const {
  if (display_ == nullptr || window_ == 0) return 0;
  Display* display = as_display(display_);
  Window root = 0;
  Window parent = 0;
  Window* children = nullptr;
  unsigned int count = 0;
  if (XQueryTree(display, window_, &root, &parent, &children, &count) == 0)
    return 0;
  const Window first = count > 0 ? children[0] : 0;
  if (children != nullptr) XFree(children);
  return first;
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

  for (unsigned int i = 0; i < count; ++i) {
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, children[i], &attributes) == 0) continue;

    // The host window takes the editor's size — but only when it actually
    // differs. Re-issuing the same resize breeds ConfigureNotify events that
    // arrive back in the pump, and answering those with another resize is the
    // feedback loop that flickered every editor and livelocked DrumGizmo.
    // Tiny children are the "not laid out yet" placeholder, not a size.
    // A frame the user may drag does not follow its child either: there the
    // child follows the frame, and following back is the same loop.
    if (!user_resizable_ && attributes.width >= kMinEditorEdge &&
        attributes.height >= kMinEditorEdge)
      resizeTo(attributes.width, attributes.height);
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

// The window manager chose this window: the keys go to the editor's own
// window, which is the one that knows what to do with them. The frame itself
// otherwise kept the focus and the plugin never saw a keystroke.
void PluginWindow::takeFocus(unsigned long timestamp) {
  if (display_ == nullptr || window_ == 0) return;
  Display* display = as_display(display_);
  const Window child = firstChild();
  XSetInputFocus(display, child != 0 ? child : window_, RevertToParent,
                 static_cast<Time>(timestamp));
  XFlush(display);
}

// Everything the server has queued for this connection, handled now. Runs
// from the socket notifier as events arrive and once per idle tick as a belt:
// Xlib reads ahead into its own queue, and what is already there does not
// show on the descriptor again.
void PluginWindow::drainEvents() {
  if (display_ == nullptr || window_ == 0) return;
  Display* display = as_display(display_);

  // Sizing flows one way only: the window follows the editor, or - for an
  // editor that allows it - the editor follows the frame the user dragged.
  // Never both: each side answering the other's ConfigureNotify with a new
  // resize, all inside this drain, is the loop that never emptied. The cap
  // is the second belt: even a plugin that floods events cannot keep this
  // loop from returning.
  bool child_changed = false;
  bool frame_changed = false;
  for (int drained = 0; drained < 256 && XPending(display) > 0; ++drained) {
    XEvent event;
    XNextEvent(display, &event);

    switch (event.type) {
      case ClientMessage:
        if (event.xclient.window != window_) break;
        if (static_cast<Atom>(event.xclient.message_type) != protocols_atom_) break;
        if (static_cast<Atom>(event.xclient.data.l[0]) == delete_atom_) {
          qWarning("editor '%s': fechada pelo window manager (WM_DELETE)",
                   qUtf8Printable(title_));
          close();
          emit closed();
          return;
        }
        if (static_cast<Atom>(event.xclient.data.l[0]) == take_focus_atom_)
          takeFocus(static_cast<unsigned long>(event.xclient.data.l[1]));
        break;

      case ConfigureNotify:
        if (event.xany.window == window_ ||
            event.xconfigure.window == window_) {
          if (event.xconfigure.width != frame_width_ ||
              event.xconfigure.height != frame_height_)
            frame_changed = true;
        } else {
          child_changed = true;
        }
        break;

      case CreateNotify:
      case MapNotify:
        if (event.xany.window != window_) child_changed = true;
        break;

      default:
        break;
    }
  }

  // One adoption per drain, after it, so a burst of child events costs one
  // resize decision instead of one per event.
  if (child_changed) adoptChild();

  // The user dragged the frame. The plugin may snap the size to one it can
  // draw at; then it lays out to what the frame is.
  if (frame_changed && user_resizable_ && resizable_ != nullptr && attached_) {
    XWindowAttributes own{};
    if (XGetWindowAttributes(display, window_, &own) != 0) {
      int width = own.width;
      int height = own.height;
      resizable_->constrain_size(&width, &height);
      frame_width_ = own.width;
      frame_height_ = own.height;
      if (width != own.width || height != own.height) resizeTo(width, height);
      resizable_->resized(frame_width_, frame_height_);
    }
  }
}

void PluginWindow::pump() {
  if (display_ == nullptr) return;
  if (connection_lost_) {
    qWarning("editor '%s': conexao X morreu (IO error)", qUtf8Printable(title_));
    // The display died under us; everything X-side is gone already, but the
    // plugin's own editor teardown (detach) does not depend on X11 being
    // alive and must still run, or it never will for this instance's life.
    timer_.stop();
    notifier_.reset();
    if (attached_ && gui_ != nullptr) gui_->detach();
    attached_ = false;
    window_ = 0;
    display_ = nullptr;
    emit closed();
    return;
  }

  drainEvents();
  if (window_ == 0) return;  // the drain closed it

  // Non-zero from idle() is the plugin asking to close: a CLAP editor that
  // shut itself, an LV2 instance replaced under its UI.
  if (attached_ && gui_->idle() != 0) {
    qWarning("editor '%s': plugin asked to close", qUtf8Printable(title_));
    close();
    emit closed();
    return;
  }

  // The editor asked to be a different size - a CLAP plugin switching skins,
  // say. Polled right after idle(), which is when the request is filed.
  if (attached_ && resizable_ != nullptr) {
    int width = 0;
    int height = 0;
    if (resizable_->take_resize_request(&width, &height)) {
      resizeTo(width, height);
      resizable_->resized(frame_width_, frame_height_);
    }
  }
  XFlush(as_display(display_));
}

}  // namespace nirbija
