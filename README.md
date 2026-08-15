# Nirbija

A Linux plugin host and mixer in the spirit of AUM. JACK/PipeWire audio,
Qt6 QML UI, LV2 + CLAP + VST3.

## Build

Needs C++20, CMake ≥ 3.28, JACK (`pipewire-jack` is fine), libsndfile, lilv,
Qt6 Quick, and X11.

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

## Tests

```sh
ctest --test-dir build --output-on-failure
```

Tests that need a JACK server or a named plugin **skip** (CTest 77) instead
of passing. Offline DSP tests (`graph_routing`, `file_player`, `looper`) run
anywhere.

## Licence

GPLv3. See `LICENSE`.
