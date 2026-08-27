# Nirbija

A Linux plugin host and mixer in the spirit of AUM. JACK/PipeWire audio,
Qt6 QML UI, LV2 + CLAP + VST3.

<https://zednaked.github.io/nirbija-site/>

## Build

Needs C++20, CMake ≥ 3.28, JACK (`pipewire-jack` is fine), libsndfile, lilv,
Lua 5.4, Qt6 Quick and Widgets, and X11.

```sh
cmake -S . -B build -DNIRBIJA_UI=ON
cmake --build build -j
```

The UI is on by default. The app must run on X11/XWayland so plugin editors
can embed (`QT_QPA_PLATFORM=xcb` is forced). OpenGL editors on this machine
often need `__GLX_VENDOR_LIBRARY_NAME=mesa`.

```sh
__GLX_VENDOR_LIBRARY_NAME=mesa ./build/src/ui/nirbija
```

Plugin editors are X11 windows of their own and want to float rather than tile.
Under Hyprland the app arranges that itself at startup, over the compositor's
IPC socket; `NIRBIJA_NO_WM_RULES=1` turns it off. Other compositors still want
the rule by hand — `packaging/README.md` has it.

`nirbija --version` reports the version and which backends the binary carries;
`nirbija --help` lists the environment variables it reads.

## Install

```sh
cmake --install build --prefix ~/.local
```

Lays down the binary, the launcher and the icon — four files, nothing else.
See `packaging/README.md` for the desktop and Hyprland side of it.

## Strips

A chain worth keeping goes in a file of its own — click a strip's name for
**Save strip…**, and right-click the `+ add strip` square for **Load strip…**.
Every plugin's state travels with it, so a sequencer arrives with its pattern.
A plugin the file wants and this machine does not have is named on screen, and
the rest of the strip still loads.

## Packaging

```sh
packaging/dist.sh src        # source tarball, from what git has committed
packaging/dist.sh bin        # binary tree, for a machine with the same Qt
packaging/dist.sh appimage   # self-contained, Qt bundled in
```

Everything lands in `dist/`. The binary tree links against the Qt and JACK of
the machine that built it, so it travels only to an identical distro; anything
else wants the AppImage or the source.

## Built in

Besides whatever LV2, CLAP and VST3 you have installed, a few plugins live in
the host and need nothing installed at all:

| | |
|---|---|
| **Step Sequencer** | sixteen steps, swing, scales, reverse/pendulum, chance, ties |
| **Arpeggiator** | up, down, up-down, as played, random, chord; octaves and latch |
| **Script** | a MIDI plugin you write in Lua, in the mixer |
| **Looper** | quantised launch, reverse, half/double, multiply, replace, once |
| **FX Pad** | sixteen graduated effects; pitch, filter, comb and ring go both ways |
| **File Player** | a file into a strip |
| **Computer Keyboard** | GarageBand-style typing keyboard; no MIDI hardware needed |
| **Sampler** | sixteen pads; Rec from the strip, or a file; names the step sequencer reads |

They sit in the picker with everything else. A sequencer above a synth — or
the sampler — in the same strip plays it: the MIDI a plugin makes joins what
the next one in the chain receives.

## Using it

Every control answers a drag, the wheel and the keyboard: Shift makes a drag ten
times finer, one notch of the wheel is a decibel on a fader, and Tab reaches the
faders, pans, sends and slots in turn. `F1` lists the lot.

The whole interface is sized off one number. **Ctrl+=** and **Ctrl+-** move it
while the mixer is open, **Ctrl+0** goes back, and the size is remembered for
that machine — a 4K panel and a laptop want different answers and neither is a
property of the session.

`NIRBIJA_UI_SCALE` still sets it at startup, and wins over the remembered one
when it is given:

```sh
NIRBIJA_UI_SCALE=1.25 ./build/src/ui/nirbija
```

## Tests

```sh
ctest --test-dir build --output-on-failure
```

Tests that need a JACK server or a named plugin **skip** (CTest 77) instead
of passing. Offline DSP tests (`graph_routing`, `file_player`, `looper`) run
anywhere.

The QML is checked separately, and is expected to stay silent:

```sh
cmake --build build --target nirbija_ui_qmllint
```

## Notes

| | |
|---|---|
| `PLAN.md` | what was decided and why, phase by phase |
| `PORTING.md` | what a Mac or Windows port would cost, measured |
| `ECOSYSTEM.md` | what the free plugin world is missing, counted |
| `design/` | designs for things not built yet |

## Licence

GPLv3. See `LICENSE`.
