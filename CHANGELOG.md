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

### FX pads have an amount

Each pad is a depth, not a switch. Drag it for how much of the effect — crush
holds longer and folds shorter, dirty drives harder, stutter slices smaller —
instead of the same preset quieter. Pitch, Filter, Comb and Ring are bipolar:
the middle is off, up and down go opposite ways.

A session that only stored 0 or 1 still loads; those values were already the
ends of the same range.

### The looper keeps the take you already have

Leaving Rec down used to keep writing onto the end of the first pass, so the
phrase sat at the head of a growing tape and sounded like it was fading out.
It now closes at Length and stays in overdub: the loop plays, Rec still
layers, and unity feedback no longer scales the old layer at all.

A tap on the Feedback label no longer jumps the value to nearly zero, which
is what then ate the take on every pass.

### The step sequencer can record

**Rec**, in the step editor: play a MIDI instrument into the same slot and
what you play lands on the nearest step instead of only passing through —
pitch, velocity and how long you held it, a hold that crosses a step
boundary becoming a tie across however many steps it spanned rather than a
retrigger. The pattern itself goes quiet while it's armed, so a live take
and sixteen already-programmed steps are never fighting for the same
output. A step you didn't touch keeps what it had, so a second pass can fix
just the ones that landed wrong without redoing the rest.

### The computer keyboard is an instrument now

**Computer Keyboard**, a fourth built-in MIDI source: A W S E D F T G Y H U
J K O L P ; play an octave and a half, GarageBand's own "Musical Typing"
layout, so anyone who has used one before needs no explanation. Z/X shift
the octave, C/V the velocity, both work whether or not a key is held.
Chords are polyphonic, OS key-repeat does not retrigger the same note held
down, and it passes through whatever arrives from earlier in the chain the
same way the sequencer does — feed it into the step sequencer's new Record
and a bassline typed on a laptop lands quantised on the grid with no MIDI
hardware anywhere in the room.

### The editors stay out of the way now

Step Sequencer, Looper, FX Pad, Script and Computer Keyboard open as tool
windows, not dialogs: the mixer behind stays live, so a fader, another
strip, or Play is still reachable with one of these open. Drag the empty
background to put it wherever it's out of the way — it stays there the
next time that same editor opens, the way a real window remembers where it
was left. A hosted plugin's own editor — Odin, anything else with a native
GUI — already worked this way; ours were the odd ones out.

A second click on the same chip now closes its editor instead of just
reopening it in place — the toggle a chip is expected to have, once the
editor is not modal and the chip is reachable again while it's open.

The Computer Keyboard listens regardless of focus, not just regardless of
whether its editor is open: clicking a fader, dragging a knob, anything
elsewhere in the window, no longer costs it the keys. A text field, the
rename box, the Lua editor still get every letter untouched — this only
ever adds a second listener, never steals from the first. A live source
going quiet because something else got clicked never made sense, and nothing
else built in here worked that way either.

Step Sequencer, Looper and FX Pad no longer disable the whole mixer the
moment they open. That switch was a leftover from when they were modal —
pointer handlers on a fader ignore a modal dimmer's MouseArea, so the mixer
had to be switched off by hand to keep a drag on the editor from also
grabbing whatever sat underneath it. Going non-modal never touched that
switch, so it kept firing: opening any of the three still turned off every
fader, every chip, everything, including the chip that would have closed
it again. Each editor's own background already eats a stray press the same
way — that was always enough on its own — so the switch was not just
stale, it was actively wrong.

### The grid reads the chip below it

Pad names on the step sequencer are no longer a General MIDI guess — MIDI 39
was "clap" even when the sampler sitting under it was something else. Grid
now asks the **next insert** for the names it publishes: CLAP `note-name`,
and LV2 midnam, which is how **Black Pearl** and the other AVL kits tell
the host that 36 is Kick Drum. The title says `→ Black Pearl Drumkit` so it
is obvious who is being addressed.

Two instruments on one strip is two sequencers. The list is never merged:
an Odin and a kit sharing one sequencer would have collided, so they do not
share. **pads** assigns a window of that chip onto the eight lanes. ▲ / ▼
on the rail is a view: hits stay on their pitch. A note painted in Skyline
has a row in Grid — the same MIDI key, even if it is not one of the kit's
pads — and scrolling never retunes it. Without a map, Grid is the same
C2–C6 span Skyline draws. Wheel scrolls the window; Shift+wheel walks one
pad.

### Also

- The landing page says what the mixer does now: eight entries rather than
  six, with the sequencers, the Lua plugin and strip files.
- The metronome is not Play. Turning the click on walks the grid for the
  looper and sounds the tick; sequencers stay quiet until Play is on.
- Looper undo peels the last phrase between silences, not the whole Rec
  pass, so a held take with two licks in it keeps the first one.
- Clear on the looper drops Rec, so it does not start a new take on the
  empty tape.
- The looper has a Count button: one bar of clicks, then Rec starts. The
  count runs with Play off. The button is a latch — turn it off to Rec at
  once, or to cancel a count already running.
- The FX pad has MAP: tap a pad, turn a knob on the strip's MIDI input,
  and that pad is bound. MAP stays on so the next pad can follow. The
  binds travel with the session and with a saved strip — they used to
  keep a graph slot that was new every launch, so a restart forgot them.
- The looper has the same MAP: Rec, Play, Clear, Count, Reverse, Once,
  Replace, Length, Speed, Feedback, Pitch, Tone and Gain. A footswitch
  can punch Rec; a knob can ride feedback. The binds stay with the strip.
- A toggle pad (127 then 0, or a note) learned onto Rec/Play flips on
  the press and ignores the off, so a latching button does not punch
  in and straight back out. Knobs stay continuous.

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
