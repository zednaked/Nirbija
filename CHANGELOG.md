# Changelog

## 0.4.0 — unreleased

### The interface resizes

**Ctrl+=** and **Ctrl+-** while the mixer is open, **Ctrl+0** back to where it
started. A toast says where it landed, because without one the keys feel dead
at either end of the range.

Every token derived from the scale stopped being a constant, so the tree
relays itself rather than keeping the sizes it was built with. The chosen size
is remembered per machine, not in the session: how big the interface should be
is a fact about the screen in front of you, and a jam opened on a laptop has
no business resizing the desktop it came from. `NIRBIJA_UI_SCALE` still sets
it at startup and wins when it is given.

### Also

- The landing page says what the mixer does now: eight entries rather than
  six, with the sequencers, the Lua plugin and strip files.

## 0.3.0 — 2026-08-16

### Instruments of its own

Three MIDI plugins ship inside the host, filling what `ECOSYSTEM.md` counted
as the gap: of the MIDI plugins installed on a typical Linux machine, nearly
all process notes that already exist and almost none make any.

- **Step Sequencer** — sixteen steps, snapped to the transport, with a grid
  to play it on rather than eighty-five sliders. Monophonic, which stops
  being a limit once they chain: `sessions/jam-goth.json` stacks four ahead
  of DrumGizmo and gets a four-voice kit out of it.
- **Arpeggiator** — up, down, up-down, down-up, as played, random and chord,
  with an octave stack, gate and latch. Up-down turns without striking the
  ends twice, which is the difference between a figure and a stutter.
- **Script** — a MIDI plugin written in Lua, in the mixer, without compiling
  anything. The script never runs on the audio thread: it compiles lookup
  tables on the UI thread and the realtime side only indexes them. Sandboxed
  by allow list, with an instruction budget.

None of the three needed anything new from the host. `set_transport`,
`take_midi_output` and the MIDI chain in `channel_strip.cpp` were already
carrying notes from one insert to the next.

### Strips you can keep

**Save strip…** and **Load strip…**: a chain you liked, in a file, with every
plugin's state — a sequencer arrives with its pattern, a synth with its
patch. A strip file is a session holding one channel, written and read by the
same code sessions use, so the two cannot drift.

A plugin the sender had and the receiver does not is the ordinary case for a
file that travelled: the strip still arrives, with a hole, and the hole is
named on screen.

### Following

An **`ext`** button and a `clock_in` port: start, stop, song position and
tempo can come from an external MIDI clock. Position is counted in ticks
rather than frames, so a wobbling tempo estimate moves the tempo without
moving the song underneath it.

### Fixed

- **Duplicating a strip** copied the name and the fader and left the chain
  empty, which is not a copy of anything. It brings the plugins and their
  state now.
- **Two strips could wear the same colour.** The accent came from the graph
  slot, and channels and buses draw slots from separate pools; four channels
  and two buses was enough to collide twice with two colours unused.
- padthv1 is out of the demo sessions: driven with notes it raises its own Qt
  machinery inside the host and falls over in it. Reproducible in a process
  that builds no Qt application at all, so it is the plugin, not the host.

### Also

- `sessions/jam-techno.json`, where every built-in plugin carries a real
  part, and `sessions/jam-goth.json`.
- Lua 5.4 is a new build dependency.

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
