# Changelog

## 0.4.0 — unreleased

### A drone instrument, with an editor made of strings

`nirbija.drone` is a built-in instrument for drone music: six strings that
never stop sounding, each an interval from one root and a few cents off so the
set beats against itself, tuned just by default so a fifth is exactly 3:2.
There is no note-on and no note-off - the performer rides a swell that brings
the whole set up and down over seconds, and a MIDI note re-roots the drone and
lets the strings glide there. Drift and Tide make the set wander slowly on its
own; a filter breathes with the same tide; the room sits after the swell so
pulling the drone down leaves the tail hanging.

Every gain and every pitch is smoothed per sample, so a string can be dragged
while it sounds without a click - `tests/drone_test.cpp` measures the largest
sample step across the swell and fails above what six low tones make on their
own. The editor draws the strings vibrating in their harmonic mode, touched
where they should sound and slid sideways to bend; `design/drone.md` has the
rest. `tests/editor_probe.cpp` is the manual tool that brought the mixer up,
opened the editor and took the pictures the layout was checked against.

The editor's **STRINGS** button holds seven sets of strings - tanpura, sub and
octaves, the harmonic series among them - that swap the strings and the tuning
under a sounding drone and leave root, swell and weather alone. A step
sequencer above the drone in the same strip re-roots it step by step with no
new code; `sessions/jam-drone-seq.json` shows it. And the drone is the first
built-in plugin to take ECOSYSTEM.md's route out: `build/clap/Nirbija
Drone.clap` is the same DSP as a CLAP, checked by loading it back through the
host's own CLAP backend (`tests/drone_clap.cpp`). That check also found the
CLAP backend never said whether a plugin takes notes; it does now.

### The looper's editor, redrawn, and a sign that says when to play

`LooperEditor.qml` gets the same treatment the drone's editor got: a window
that glows in the colour of what the tape is doing, a chip in the header with
the state, the loop's own bar.beat and its level, the tape in an inset panel,
and the rides - Feedback and Gain - as tall bars on the right with pitch,
speed and tone as smaller ones below. The bar itself moved out of
`DroneEditor.qml` into `ParamBar.qml` so both editors draw the same control;
it learned to fill from the middle for a value that rests at its centre, and
to drag relative rather than jump, for a feedback that must not be slammed
mid-take.

The part that matters on stage: a quantised Rec press does not write until
the bar comes round, and a release keeps writing until the next one, and the
old editor said "recording" through both gaps. `LooperInstance` now mirrors
`writing()` - whether the head is actually on the tape - and
`beats_to_boundary()` - how long until a pending press lands - and the editor
counts those down in a sign over the tape: REC IN 3, LOOP CLOSES IN 2,
OVERDUB ENDS IN 1, and ONE IN 4 while it plays, next to a row of beat lamps
that light as the head passes and a flare on the one and on every punch.
Yellow is always "about to", red is "being written", green is the cycle
going round. `tests/looper_test.cpp` walks a press and a release across the
bar and checks the mirrors tell the two moments apart. `editor_probe` takes
`play=1` and `at=<ms>` so an armed looper can be photographed mid-bar, and
`editor=0` to photograph the strip behind it.

The dot on a Looper's insert slot in the strip follows the same rule: yellow
while a press waits for the bar, red only while the head is on the tape,
green while the loop plays. It used to go red on the press, a bar early.

### The step sequencer's editor, redrawn the same way

`StepGrid.qml` gets what the looper's and the drone's editors got. The window
glows in the colour of what the sequencer is doing - red while it records,
yellow under Fill, green while it runs - and a chip in the header says so in a
word, next to the focused lane's own bar.beat (a lane in 7 reads 2.3 where the
song reads 4.1: the polymeter, made visible), which pattern plays and which is
queued, and the name of the instrument the notes reach. A sequencer with
nothing below it on the strip says `→ nothing below` in yellow and the status
line explains, because that was the one way to end up with a grid that plays
and nothing heard.

The grid sits in an inset panel with a row of step lamps under it: one per
step of the focused lane, all of them, so a 64-step lane is seen entire while
the grid shows sixteen; bars are the tall lamps, beats the medium ones, the
page on screen the bright stretch, and a tap on the lamps turns to that page.
The panel flares on the lane's one and on a Rec press. The four macros a
performer rides - Density, Chaos, Probability, Ratchet - are the same bars the
drone's swell and the looper's feedback are, on the right, Density filling
from the middle so ×1 reads as nothing added; Swing and Transpose sit below as
smaller ones. A pattern is switched with a click and queued for the top of
the bar with a right-click, ringed yellow until it lands. The rows under the
grid are named - PATTERN, RUN, SCALE, LANE, STEP - with the lane's division
as buttons instead of a slider, and MAP binds any bar, Rec, Fill, the
pattern number or a lane setting to a knob on the strip's MIDI input, the
way the other two editors do.

The dot on a Step Sequencer's insert slot in the strip goes red while Rec is
down, the same as a looper writing or a sampler taking; it used to say
nothing. `editor_probe` took the pictures: `nirbija.stepseq` with `play=1`,
`200=0 15=1` for the skyline recording, `editor=0 15=1` for the strip.

The header chip also follows the strip now. It read who sat below the
sequencer once, when the window opened, and again only after a click in the
grid, so removing the instrument from the strip with the editor open left the
chip saying its name and the lanes carrying its pads. The 50 ms poll asks each
tick and repaints only when the answer differs, so the chip goes to
`→ nothing below` the moment the slot empties, and a kit loaded into a sampler
below names the lanes without reopening the window. `editor_probe` gained
`below=<uid>` and `remove_below=<ms>` to stage exactly that.

### The sampler's editor, redrawn the same way

`SamplerEditor.qml` gets what the drone's, the looper's and the step
sequencer's editors got. The window glows in the colour of what the sampler
is doing - red while it takes, yellow while it counts in or waits for the
bar, green while a pad sounds - as bright as the kit is loud, and a chip in
the header says so in a word, next to the pad in hand and its key, the last
note or CC the controller sent, and the kit's own level apart from the strip's.

The pads sit in an inset panel and each draws its own sound as a small
waveform, so a kit is read at a glance instead of by name; a pad that sounds
draws it green with a head crossing it, a pad being taken fills red as the
buffer fills, and the pad Rec is waiting for breathes yellow. The pad in
hand is drawn wide underneath as a tape with the looper's handles - tall
tabs to trim, rings to fade - a playhead while it sounds, and a sign over it
while a take is coming: the count, REC ON THE BAR, TAKING ONTO KICK. The
tape flares when Rec punches in or out. The rows under it are named - PAD
with the name, the key on a chip that drags or rolls a semitone at a time
(a pad that already had the key takes this one's, so no two share one),
ONE-SHOT or HOLD; REC ON with now, beat, bar; KIT with the packs - and the
rides on the right are the same bars the other editors use: the pad's
Volume and the kit's Gain tall, Pitch and Pan below filling from the middle.

`SamplerInstance` mirrors what the editor needed to draw: `level()`, the
chip's own block peak; `armed()`, Rec down and waiting for the grid, which
the old editor called "recording" a bar early; `rec_fill()`, how much of the
take buffer is written; `pad_position()`, where each voice is in its pad;
and `pad_version()`, bumped whenever a pad's audio changes hands, so the
sixteen thumbnails are refetched only when one of them must be.
`Mixer.assignSamplerPadNote()` exposes the note swap the MIDI-learn path
already used. `editor_probe` takes `pack=<file>` and `hit=<pad>`, so a
sampler can be photographed with a kit on its pads and one of them sounding.

### A kit loaded from a session file went silent on the next start

The sampler kept a pad's path exactly as the file that named it spelled it,
so a kit loaded from `sessions/jam-sampler.json` stored `samples/kick.wav`,
relative. That resolved against the folder of the file being read - fine for
the jam - and the autosave then carried the same relative name into the user's
data folder, next to no samples at all. The next start found every pad empty,
and the mixer played nothing.

A pad remembers the full path of the file it actually found now. A kit that
travels is still found: a name that points nowhere is looked for next to the
file that names it, by its last folder and file name, so a moved
`samples/kick.wav` comes back. `tests/sampler_test.cpp` holds both.

### Numbers in state blobs no longer depend on the locale

Qt sets the C library to the user's locale on startup, and under pt_BR every
`%.4f` the built-in plugins printed became `0,5000`. Read back by a parser that
stops at the comma, that is 0; a gate of 0 clamps to its floor, is written as
`0,0500`, and stays there - every lane of every sequencer in a Brazilian
session had a 5 % gate by the second save. The sequencer, the script plugin,
the fx pad and the file player format numbers with `to_chars` now, the parser
forgives the files already written with a comma, and the mixer pins
`LC_NUMERIC` to C for whatever a hosted plugin prints for itself.
`tests/state_locale.cpp` runs the writers under a comma locale when the
machine has one.


### Editing a sequence no longer pops the master

Touch a step in the sequencer grid and, a second later, the master went quiet
for a few blocks and came back. That was the autosave: reading every plugin's
state parked the graph, and a parked graph wrote zeros — no fade, no ramp. On a
pad, a reverb tail, a held note, a hard cut to nothing and a hard cut back is a
pop, and it landed a second after the last edit, every time.

The park was there because of a reading of the LV2 spec that turned out to be
backwards. `save()` may be called concurrently with `run()`; it is `restore()`
that wants the instance quiet. CLAP and VST3 save on the main thread with the
plugin active, and the built-in plugins read atomics. So a save now runs with
the mixer playing. A plugin that genuinely cannot be read that way at this
moment — a looper with Rec down, a sampler with a take waiting to be published —
says so through `PluginInstance::save_needs_quiet()`, and only then, and only
for that save, is the graph parked. `tests/session_autosave_live.cpp` holds
both halves against a real audio server.

When the graph does park — a state load, a looper mid-take — the master fades
out over 5 ms and back in the same way, and the strips' direct outs get the
same curve. Those used to repeat their last block for the whole park, a buzz
on the hardware outs while the master was silent.

### Nothing in the mixer steps any more

The same audit found every other switch that cut in one sample: a strip's mute,
solo, an insert's bypass, a send level, the master fader (one gain per block —
a zipper when moved), dim, mute and mono. Each is a slope now: mute and bypass a
straight 10 ms line, bypass a real crossfade with the plugin running through
it, everything summed into a destination walked across the block and limited
to a full swing in no less than 10 ms. `tests/graph_smooth.cpp` watches the
master sample by sample through each of them for a jump.

### A brickwall on the master

Eight lanes into a synth passes full scale without trying, and past full scale
the converter clips. The master has a limiter now: 1.5 ms of lookahead, a
ceiling of -0.3 dBFS, transparent below it and a hard stop at it. LIM in the
top bar, on by default for a session that never said otherwise, lit hot while
it is holding something back. It costs 1.5 ms on the master and nothing else.

### The song position is a count

Beats were derived from the frame counter times the tempo, so changing the
tempo moved the song — two minutes in, 120 to 121 BPM jumped two beats
forward; 120 to 119 jumped back and left the sequencer silent until the
position caught up. The audio thread counts beats per block now, and the
position display reads that count.

### Room for note-offs

A block's MIDI ran through queues of 128, 256 and 64 events. Eight lanes of
ratchets can produce more, and whatever did not fit was dropped — note-offs
included, and a note-off dropped is a note that never stops: on a synth, a
wall of voices and then clipping. The queues are 1024 deep the whole way down,
and the last eighth of every one is kept for note-offs.

### Denormals off, xruns counted

The audio callback now flushes denormals on every block — a reverb tail
decaying to nothing used to cost a hundred times more per sample exactly where
the music went quiet — and the server's dropouts are counted and shown in the
status bar, so a click can be told apart from a missed period.


### The same leak, in three more places

The fix before this one closed a retire list that never drained with no audio
server. It closed the two it found by reading, not the class of thing — and the
class had six members. A sweep for the pattern turned up three more: the
sampler, the file player and the Lua script plugin all swap a buffer under the
audio thread and wait on a generation counter that only `process()` advances.

The sampler is the one that hurt. Loading a sample into a pad retires the pad's
previous buffer, and a pad holds up to eight seconds of stereo — about 3 MB.
Measured with the audio server down: **thirty loads into one pad kept 58.7 MB**,
none of it reachable, all of it held until the process exited. After the fix,
3.0 MB — the one sample that is actually in the pad.

Nobody inside a plugin can tell "there is no audio thread" from "there is one
that has not reached its first block yet", and freeing in the second case is a
use-after-free. So the question is answered by the only one who knows: the host
says, through `PluginInstance::reclaim_retired(bool)`, on the idle poll that
already calls `host_idle()`.

`tests/sampler_test.cpp` holds both directions — nothing retired when there is
no audio thread, everything retired when there is one that has not rendered.

The test that guards it was itself wrong on the first try, and the sanitizer
build is what said so: it measured RSS, and RSS does not fall under ASan, which
keeps freed memory in quarantine. It counts retired buffers now — the contract
itself rather than a proxy for it.

### A plugin removed with no audio server was never freed

An insert pulled out of a strip is not freed on the spot — the audio thread may
still be inside it, so it waits in a retired list until two renders have gone
by. The generation that gate counts only advances at the end of a render, and
renders come from the JACK callback. With no audio server there is no callback,
so the gate never opened: everything removed was held until the process exited.

Not a corner: the app comes up fully usable with no audio server, says so in the
status bar, and invites you to restart once one is there. Auditioning plugins in
the meantime — add, listen to nothing, remove, try the next — grew the process
by a whole plugin instance each time. A step sequencer is 260 KB before counting
whatever the plugin itself allocated.

The gate now takes the one question that settles it: whether a JACK client
exists at all. With none there is no audio thread, so nothing can be inside a
retired insert and it goes immediately. `tests/insert_reclaim.cpp` holds all
three states, including the one that makes the obvious fix wrong — a strip built
while audio is already running also sits at generation 0 until its first block,
and the audio thread may be inside one of its inserts by then, so "generation is
0" must not be read as "safe to free".

Found auditing under ASan and UBSan, which the whole headless suite now passes.

### Plugin editors float on their own under Hyprland

Installing meant pasting a window rule into the compositor's config before the
first plugin editor opened at a sane size — and the instructions for it sat in
`packaging/README.md`, which is exactly where nobody looks until something is
already wrong. Tiled, an editor stretches to fill the tile: the plugin keeps
drawing at its own size and the rest is dead space around it.

The app now asks for the rule itself, at startup, over Hyprland's IPC socket.
Nothing to paste, nothing to install, and `NIRBIJA_NO_WM_RULES=1` for anyone
who would rather own their compositor's config outright.

Hyprland is the only compositor that takes a rule at runtime, so it is the only
one handled; everywhere else the rule stays manual and `packaging/README.md`
still carries it, Sway's form included. The request goes in as Lua: 0.56 moved
the config over and its `keyword` command now refuses a windowrule with
`keyword can't work with non-legacy parsers. Use eval.` — `keyword windowrulev2`
is kept as the fallback for older builds. The rule is named, so relaunching
replaces it instead of stacking a second copy.

While proving it: on a systemd distro `NIRBIJA_DEBUG_EMBED` and the other
traces go to the journal, not to the terminal. Qt routes its logging there when
stderr is not a tty, so redirecting stderr catches an empty file and the flag
reads as broken. `journalctl --user -f`, or `QT_FORCE_STDERR_LOGGING=1`. Said
now in `--help` and in both READMEs.

### A ratchet used to die after its own first pulse

A step's ratchet — up to 8 rapid retriggers packed into one step, the
sequencer's own way of doing a roll — only ever played its first hit once
the lane's gate was under 1.0. Every lane's own factory gate is 0.5, so
this was not an edge case: it was every ratchet, on every pattern anyone
had built, everywhere but a test that happened to force gate to full.

The gate closing between two pulses on purpose — the same silence a short
gate leaves between two ordinary steps — looked identical to the voice
having been cancelled from outside, and the second pulse's own check for
that treated them the same way. The two are different questions now: the
note a ratchet keeps retriggering is remembered on its own, separately
from whether that note happens to be sounding at this exact instant.
Found building `sessions/jam-breakbeat.json`, a stress-test session that
leans on exactly this for its snare roll.

### A sampler of its own

**Sampler**, sixteen pads inside the host. Rec from the strip's input onto the
focused pad, or load a file onto it. One-shot or hold, trim, pitch, pan. The
pads publish their names, so a step sequencer sitting above the chip writes
Kick and Snare instead of MIDI 36. The first built-in instrument: MIDI in,
audio out, nothing else to install. A pad loaded from a file remembers
the path, like the File Player: the session is a list of samples, not a
blob of floats. Rec takes still travel inside the blob. `sessions/jam-sampler.json`
points at `sessions/samples/` — four grooves, a reverb bus, Play.

### The sampler forgives a bad take

**Undo** on a pad swaps back to whatever it held before its last Rec, Load or
Clear — one press, not a history to dig through, because what a bad take
wants is "let me hear the old one," and a second press swaps forward again if
the old one wasn't it after all.

**Count-in** puts a bar of clicks ahead of Rec, so the phrase you meant to
catch does not start with the sound of the button being pressed. Borrowed
from the looper, which solved this the same way.

### Packs: the kit, on its own

**Save Pack** and **Open Pack** in the Sampler editor write and read just the
sixteen pads — not gain, not quantize, not count-in, not whatever sits in
front of the Sampler in the chain. Swap the kit under a sequencer without
resetting how Rec behaves, or carry a kit between sessions the way a sample
pack travels. `sessions/packs/` ships two: **808 Trap** and **Techno Clang**,
built by the same tiny synth as `jam-sampler.json`'s house kit
(`sessions/drum_synth.py`, factored out so both generator scripts share it).

### The sampler's waveform is the trim now

Drag the tall handles to trim a pad, the round ones just inside them to fade
it in and out — the looper's own waveform editor, brought over pad by pad
instead of one long tape. The two numeric start/end sliders are gone; the
waveform was always the more honest picture of what they meant.

Fade is new under the hood too: each pad gets its own fade-in and fade-out,
each capped at half the trim window the same way the looper's is, so a short
pad with both turned up crossfades through the middle instead of one eating
the other's tail. Travels with save_state, a pack, and the fixed 64-sample
anti-click envelope everything already had — a musical fade on top of the
click guard, not instead of it.

### The sampler takes a MIDI controller

**MAP** in the Sampler editor, the same gesture as the looper and the FX pad:
tap Rec, Clear, Undo, Count-in, a knob, then turn the control. Tap a pad,
then hit a pad on the controller, and that pad's MIDI note is the one that
just arrived — so an SMC-PAD (or anything else with sixteen pads) can be
wired by hitting them, rather than by typing note numbers. Two pads that
would share a note swap instead, so the grid never has two keys for the
same hit. MAP stays on so the next pad can follow.

A note that names a pad also focuses it, so the knobs and Rec follow
whichever pad the controller just played.

The editor listens to every MIDI source while it is open, and stacks those
same sources onto the sampler's strip so a pad still plays after the
window closes.

Bluetooth LE MIDI is taken from BlueZ directly. PipeWire advertises the
SMC-PAD as a JACK port but never AcquireNotify's the GATT characteristic,
so the port exists and stays silent; we open the notify FD ourselves and
decode the BLE packets. The SMC-PAD's performance preset speaks notes
0–15 on channel 10, not General MIDI 36–51 — those hits land on the 4×4
instead of vanishing. Second and last rows of that grid are swapped to
match the hardware. The header says `note 13` (or `CC 20` if the
controller is sending knobs). The hit itself is a bright wash, not a
faint border.

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

- The landing page says what the mixer does now: nine entries rather than
  six, with the sequencers, the Lua plugin, the sampler and strip files.
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
