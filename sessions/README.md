# Ambient jam demos

Session files in the same format as `~/.local/share/Nirbija/Nirbija/session.json`.
Plugins start at factory defaults — open the editor, pick a preset, play notes
from the on-screen keyboard.

The live autosave was **not** touched.

## How to load (without wiping your current jam)

`File → Load session…` replaces the mixer **and** becomes the new autosave.
To try a demo in a sandbox (recommended):

```sh
sessions/run-jam.sh jam-pad-hall
```

That sets `NIRBIJA_SESSION` to the demo file and bind-mounts Cardinal's
module data so the rack is not empty. Same thing by hand:

```sh
NIRBIJA_SESSION="$PWD/sessions/jam-pad-hall.json" \
  __GLX_VENDOR_LIBRARY_NAME=mesa \
  ./build/src/ui/nirbija
```

## The five recipes

| File | Tempo | What it tests |
|---|---|---|
| `jam-pad-hall.json` | 72 | padthv1 + Odin2 + looper/chorus → Dragonfly Hall send |
| `jam-modular-fog.json` | 80 | Cardinal Synth + ChowKick + file player/tape → Cardinal FX + Room |
| `jam-tape-drone.json` | 60 | Surge XT CLAP + Odin2 + Airwindows/looper → Plate + CHOW Tape |
| `jam-shimmer-grain.json` | 88 | padthv1/wolf-shaper + Odin/phaser + reverse delay → Hall + ZamVerb |
| `jam-kick-cloud.json` | 84 | ChowKick + padthv1/tape + stepseq/Odin2 → Hall + Airwindows |

Sends use `busName` so they rebind after load. Some channels use
`destinationKind: bus` (serial into the FX) instead of a send (parallel).

## jam-black-pearl.json

Not ambient: one channel, `nirbija.stepseq` (the native 8-lane step
sequencer) feeding AVLdrums' `Black Pearl` LV2 kit. Tempo 100. Eight lanes,
each a fixed voice for the whole session:

| Lane | Voice | Note |
|---|---|---|
| 0 | Kick | 36 |
| 1 | Snare | 38 |
| 2 | Closed Hat | 42 |
| 3 | Open Hat | 46 |
| 4 | Clap | 39 |
| 5 | Rim/Sidestick | 37 |
| 6 | Floor Tom | 41 |
| 7 | Cowbell | 56 |

All sixteen pattern banks come pre-loaded, so switching banks mid-jam
changes the whole groove instead of just a fill. The first eight are
straight genre loops; the second eight stay on the same 16-step/4-4 grid
but layer in cross-rhythms, additive groupings and ratchet fills instead of
changing meter (lane length/division are shared across every bank, so an
actual time-signature change isn't on the table — the polyrhythm lives in
where the hits fall, not in the grid itself).

| Bank | Groove |
|---|---|
| 0 | Four to the floor (house) |
| 1 | Boom bap |
| 2 | Breakbeat / funk break |
| 3 | Dembow / reggaeton |
| 4 | Halftime / trap |
| 5 | Afrobeat / amapiano-ish |
| 6 | Techno, driving |
| 7 | Jungle / DnB break |
| 8 | 3-vs-4 hemiola (kick in 4, rim in 3) |
| 9 | 5-over-4 (rim outlines 5 evenly across the bar) |
| 10 | 7-feel (rim/cowbell outline 7, Tool-style cross-accent) |
| 11 | Ratchet technical fills (triplet/16th-roll flourishes) |
| 12 | Quintuplet stutters (ratcheted hat on every quarter) |
| 13 | Additive 2+3+3 grouping |
| 14 | Asymmetric 5+4+4+3 groove |
| 15 | Tool tribute — uneven phrasing, cross accents, a ratcheted tom fill |

Switch banks live from the sequencer's own pattern/next-pattern controls —
a change queued mid-bar takes over on the next bar line, so it stays on the
beat. Requires AVLdrums (`avldrums.lv2`, package usually named
`avldrums.lv2` or similar) — without it the channel's second insert just
won't resolve, but the sequencer and its patterns are unaffected.

## jam-goth-pearl.json

Same base as `jam-black-pearl.json` (`nirbija.stepseq` → AVLdrums Black
Pearl, same 8 lanes/voices, same 16-step grid) but every bank is a gothic
drum style instead of a straight genre loop. Tempo 128 — a goth-rock
default; slow it down for the dirges, speed it up for deathrock.

| Bank | Style |
|---|---|
| 0 | Sisters of Mercy — tribal four-on-the-floor stomp |
| 1 | Batcave tribal |
| 2 | Deathrock stomp |
| 3 | Post-punk motorik (Joy Division / Killing Joke) |
| 4 | Cold wave / drum-machine minimal |
| 5 | Bauhaus dirge (halftime doom) |
| 6 | Fields of the Nephilim gallop |
| 7 | Gothic industrial stomp |
| 8 | Siouxsie tribal groove |
| 9 | Alien Sex Fiend — tribal war-drums |
| 10 | Christian Death deathrock |
| 11 | Cure / Curve gloom groove |
| 12 | Funeral march / doom dirge (very sparse) |
| 13 | Killing Joke aggressive tribal |
| 14 | This Corrosion — tribal war chant |
| 15 | Ambient goth atmosphere (barely there) |

Same caveat as the other stepseq jam: lane voice/length/division are
shared across every bank, so the gothic character comes entirely from hit
placement, not from retuning the kit or changing meter per bank.

## jam-lucretia.json

Inspired by The Sisters of Mercy's "Lucretia My Reflection" (Floodland,
1987) — not a sample-accurate transcription, built from a fan bass tab and
a couple of BPM/key lookups, so treat it as a jam starting point, not a
cover. Tempo 130, key A major with the chord progression A–C–D–E (the C
major is a borrowed bIII, which is where the song's darker color comes
from over an otherwise major key). Three channels:

- **Avalanche** — `nirbija.stepseq` → Black Pearl, in the same
  Doktor-Avalanche-style tribal-stomp spirit as `jam-goth-pearl.json`.
  Bank 0 is the verse groove, bank 1 is a busier chorus lift (extra kick
  syncopation, tom fills, cowbell, continuous hats).
- **Bass** — `nirbija.stepseq` → Odin2, a single lane holding the
  transcribed riff: a 16-step pulse on A, four hits on C, then a
  descending F–E–E–D–D–D–D–D–D–F–E–E turnaround, over two bars (32
  steps).
- **Pads** — `nirbija.stepseq` → Odin2 (a second instance), three lanes
  holding the root/third/fifth of each chord, one bar per chord over a
  four-bar (64-step) loop, tied so the chord doesn't retrigger every 16th.

No Dragonfly/Airwindows on this machine, so atmosphere comes from
`nirbija.fxpad`'s own Reverb pad on a shared **Room** bus (95% wet), fed by
a send from each channel (drums 30%, bass 22%, pads 85%) rather than one
FX Pad instance per channel — three always-on reverb instances measured at
roughly 3-4x the CPU of one shared bus on this machine (i7-4770HQ), which
was the real cause of the reported lag, not the two Odin2 instances.

## jam-more.json

Inspired by The Sisters of Mercy's "More" (Vision Thing, 1990) — read from a
fan guitar-arranged-for-synth tab, not a certified transcription, but the
riff it's built from is genuinely simple: a constant pedal note under a
dyad that slides a whole tone and back, which is exactly why this one was
picked as the easier, more loopable jam next to `jam-lucretia.json`.
Tempo 129. Built with the shared-bus reverb from the start (see the
`jam-lucretia.json` note above about why per-channel `nirbija.fxpad` is
too expensive to repeat) — four channels:

- **Avalanche** — `nirbija.stepseq` → Black Pearl. Bank 0 is the verse,
  bank 1 is the arena-sized chorus lift, bank 2 is a sparse breakdown
  (just the downbeat kick, quiet rim, and a floor-tom roll building back
  in) for a dynamics move mid-jam.
- **Pedal** — `nirbija.stepseq` → Odin2, one lane, a constant one-bar
  pulse on the pedal note (the tab's low string, held on one fret the
  whole way through).
- **Riff** — `nirbija.stepseq` → Odin2 (a second instance), two lanes
  holding the moving dyad from the tab's other two strings: one bar on
  the first position, one bar slid a whole tone down, tied so it holds
  rather than retriggering every 16th.
- **Room** (bus) — one shared `nirbija.fxpad` Reverb (90% wet), fed by a
  send from each of the three channels.

## Cardinal data

Cardinal LV2 is in `~/.lv2`, but the binary still looks for `/usr/share/cardinal`.
User-local copy is at `~/.local/share/cardinal`. One-shot to expose it:

```sh
sudo mkdir -p /usr/share/cardinal
sudo cp -a ~/.local/share/cardinal/. /usr/share/cardinal/
```

Or install the distro package later: `sudo pacman -S cardinal-data cardinal-lv2`.
