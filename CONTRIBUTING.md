# Contributing

The first guard on this project is a git hook; CI is the second. Once per
clone:

```sh
git config core.hooksPath .githooks
cmake --preset asan
```

`CMakePresets.json` holds every tree this project uses: `dev` (Release with
the UI, the one you run), `debug`, `asan`, `asan-ui`, `tsan` and `release`.
`cmake --preset dev && cmake --build --preset dev` is the whole setup.

From then on every `git push` compiles the `asan` tree and runs the **quick**
suite under AddressSanitizer and UndefinedBehaviorSanitizer — the offline DSP
and graph tests, some twenty seconds, incremental. Tests that open every plugin
on the machine or need an audio server carry the `plugins` and `jack` labels
and stay out of the hook: `ctest --preset all` runs them by hand. Every test
has a timeout, so a stuck JACK cannot hold a push. `NIRBIJA_SKIP_CHECK=1 git
push` skips the hook; `NIRBIJA_TSAN=1 git push` also runs the quick suite in
`build-tsan/` when that tree exists (`cmake --preset tsan`).

It is worth the twenty seconds because this pass finds what review does not. It
is how we learned that the step sequencer blew the stack under ASan — and that
the largest module in the project was therefore outside the coverage — and that
an insert removed with no audio server running was never freed.

Without a `build-asan/` tree the hook warns and lets the push through, rather
than blocking someone who just cloned.

`.github/workflows/ci.yml` runs on every push and pull request: the same
sanitizer pass on a clean Ubuntu runner, the quick suite under
ThreadSanitizer, and a build of the Qt interface in an Arch container with
`qmllint` and the offscreen UI tests — the one thing the hook cannot do.

A release is cut on the machine: `packaging/dist.sh all` writes the source
tarball, the binary tarball and the AppImage into `dist/`, and `gh release
create vX.Y.Z dist/*` publishes them with a `SHA256SUMS.txt`.

Formatting is `.clang-format` (Google style, 90 columns) and `.editorconfig`;
format what you touch, not the tree. Comments explain why, in English, in the
files that already do.
