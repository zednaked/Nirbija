# Changelog

## 0.2.0 — 2026-08-16

### The picker knows what a plugin is

All three formats state their own classification and none of it was being
read. LV2 has a class hierarchy with a label, CLAP a feature list, VST3 a
`subCategories` string. The picker filters by **instrument, effect, midi,
analyser and utility**, and shows what the plugin calls itself beside the
maker — so `reverb` finds Dragonfly without knowing it is called Dragonfly.

Where a label is unreadable the ports decide: nothing with no audio ports at
all is an audio effect, whatever it claims. On the machine this was built on,
286 plugins come out as 148 effects, 45 midi, 44 analysers, 31 utilities and
18 instruments, with nothing left unclassified.

### Fixed

- **A plugin editor built on Qt Widgets aborted the host.** padthv1 caught
  it: the first `QWidget` a plugin constructs calls `qFatal` unless the
  application object is a `QApplication`, and `qFatal` cannot be recovered
  from. The app is a `QApplication` now — the interface is still QML and
  draws no widget of its own.
- **Right-click on an insert or output slot** asked for the menu and clicked
  at the same time, so the editor opened over the menu.
- **Clicking anything drawn over the add-strip square** added a channel,
  which is where a stray empty strip in a freshly loaded session came from.

The last two share a cause: a Qt pointer handler sits on the delivery path
rather than in the stacking order, so it answers taps belonging to whatever
is drawn on top of it.

### Also

- `sessions/try-picker-kinds.json`, a session holding one plugin of each kind.
- `ECOSYSTEM.md` and `design/`, on what the free plugin world is missing.
- The AppImage is 61 MB rather than 55: Qt Widgets travels with it now.

## 0.1.0 — 2026-08-16

First public build. A plugin host and mixer for Linux: a row of channel
strips, plugins in the strip, and a session that reopens where you left it.

### Hosting

- **CLAP, LV2 and VST3** behind one interface. Scan, instantiate, process,
  parameters, state.
- **Native plugin editors** embedded in the host, for all three formats. On
  Linux that means an X11 window the host creates itself, a plugin event loop
  the host pumps, and a child window the host maps — all three, or the editor
  never paints.
- The VST3 backend is written against the bare **pluginterfaces**: no Steinberg
  source is compiled. The host implements `IHostApplication`,
  `IComponentHandler`, `IBStream`, `IParameterChanges`, `IEventList`,
  `IPlugFrame` and `Linux::IRunLoop` itself.
- A **generic parameter editor** for plugins with no editor of their own, or
  when you would rather have sliders than the plugin's own artwork.

### Mixer

- Channel strips with gain, pan, mute, solo, inserts, sends and meters.
- Any strip can feed another, so a bus is just a channel you route into.
- Master strip; inserts on buses and on the master.
- A **navigator** for sessions with more strips than screen.
- Everything answers a drag, the wheel and the keyboard. `F1` lists it.
- The whole interface scales off `NIRBIJA_UI_SCALE`, so 4K and touch need no
  editing.

### Audio and MIDI

- JACK client, running equally on **PipeWire's JACK layer**.
- Realtime thread allocates nothing, locks nothing and does no I/O: structural
  changes are prepared on the UI thread and handed over by message; the discard
  goes back through a garbage queue.
- **MIDI** from JACK ports to plugins, routed per channel, plus a MIDI matrix,
  **MIDI learn**, and an on-screen keyboard.
- **Transport** and a metronome; MIDI flows between plugins in a chain.
- **Multitrack recorder** — armed channels and the master, to WAV/FLAC.
- **Looper** with quantised launch, and a **file player**.

### Session

- The graph and each plugin's opaque state blob, saved and restored.
- Autosave, plus named save and load.
- Sends bind by bus name, so they survive a reload.

### Packaging

- `cmake --install` lays down the binary, the launcher and the icon.
- `packaging/dist.sh` builds the source tarball, a binary tree and an AppImage.
- `nirbija --version` reports the version and which backends are compiled in.

### Known limits

- Linux only, and the editors need X11 or XWayland. `NIRBIJA_ALLOW_WAYLAND=1`
  keeps a Wayland session and loses embedded editors.
- OpenGL plugin editors may need `__GLX_VENDOR_LIBRARY_NAME=mesa`, depending on
  the machine's GLX vendor. The host says so when an LV2 UI refuses to start.
- Editor windows are sized by the compositor. A floating-window rule for the
  class `nirbija-plugin` is worth having.
