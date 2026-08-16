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

A Hyprland window rule for class `nirbija-plugin` should float editor windows.

`nirbija --version` reports the version and which backends the binary carries;
`nirbija --help` lists the environment variables it reads.

## Install

```sh
cmake --install build --prefix ~/.local
```

Lays down the binary, the launcher and the icon — four files, nothing else.
See `packaging/README.md` for the desktop and Hyprland side of it.

## Packaging

```sh
packaging/dist.sh src        # source tarball, from what git has committed
packaging/dist.sh bin        # binary tree, for a machine with the same Qt
packaging/dist.sh appimage   # self-contained, Qt bundled in
```

Everything lands in `dist/`. The binary tree links against the Qt and JACK of
the machine that built it, so it travels only to an identical distro; anything
else wants the AppImage or the source.

## Using it

Every control answers a drag, the wheel and the keyboard: Shift makes a drag ten
times finer, one notch of the wheel is a decibel on a fader, and Tab reaches the
faders, pans, sends and slots in turn. `F1` lists the lot.

The whole interface is sized off one number, so a 4K panel or a touchscreen
needs no editing:

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
