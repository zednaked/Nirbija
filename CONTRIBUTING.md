# Contributing

The guard on this project is a git hook, not CI. Once per clone:

```sh
git config core.hooksPath .githooks
cmake -S . -B build-asan -G Ninja \
  -DNIRBIJA_UI=OFF -DNIRBIJA_TESTS=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
```

From then on every `git push` compiles and runs the headless suite under
AddressSanitizer and UndefinedBehaviorSanitizer — 25 seconds, incremental.
`NIRBIJA_SKIP_CHECK=1 git push` skips it.

It is worth the 25 seconds because this pass finds what review does not. It is
how we learned that the step sequencer blew the stack under ASan — and that the
largest module in the project was therefore outside the coverage — and that an
insert removed with no audio server running was never freed.

Without a `build-asan/` tree the hook warns and lets the push through, rather
than blocking someone who just cloned.

`.github/workflows/ci.yml` runs the same pass on a clean tree, but on demand
only (`gh workflow run "build e testes"`): Actions bills the account's quota on
a private repository, and the hook already covers the day to day.
