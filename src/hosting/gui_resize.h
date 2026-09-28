// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

// How the window that embeds a plugin editor takes part in resizing. The
// PluginGui interface in core/plugin.h knows only attach/detach/idle and a
// preferred size; the editors that can resize (CLAP, VST3) also implement
// this, and the window finds it with a dynamic_cast on the PluginGui it was
// handed. LV2 editors size themselves through ui:resize into preferred_size
// and do not implement it.
//
// Expected use, from the window's idle tick, right after PluginGui::idle():
//   if (auto* r = dynamic_cast<hosting::ResizablePluginGui*>(gui)) {
//     int w, h;
//     if (r->take_resize_request(&w, &h)) { resize window to w x h; r->resized(w, h); }
//   }
// and, when the user drags a resizable window's edge:
//   r->constrain_size(&w, &h); resize window to w x h; r->resized(w, h);

#include <functional>

namespace nirbija::hosting {

class ResizablePluginGui {
 public:
  virtual ~ResizablePluginGui() = default;

  // Whether the user may resize the window at all. Only meaningful while the
  // editor is attached.
  virtual bool resizable() const = 0;

  // The plugin asked for its client area to be this big. Returned once per
  // request. The window resizes itself and then calls resized().
  virtual bool take_resize_request(int* width, int* height) = 0;

  // Snaps a size the user is dragging towards to one the plugin can draw at.
  // False when the plugin has no opinion; the size is then used as is.
  virtual bool constrain_size(int* width, int* height) const {
    (void)width;
    (void)height;
    return false;
  }

  // The embedding window is now this big; the editor lays out to it.
  virtual void resized(int width, int height) = 0;

  // Optional, for formats whose contract wants the window resized inside the
  // plugin's own request (VST3's IPlugFrame::resizeView). The handler resizes
  // the window synchronously, may adjust the size, and returns whether it
  // did; the editor is then told the final size in the same call. Without a
  // handler the request goes through take_resize_request instead.
  using ResizeHandler = std::function<bool(int* width, int* height)>;
  virtual void set_resize_handler(ResizeHandler handler) { (void)handler; }
};

}  // namespace nirbija::hosting
