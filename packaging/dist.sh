#!/usr/bin/env bash
# Builds the things a release hands out. Run from anywhere.
#
#   dist.sh src        source tarball, from what git has committed
#   dist.sh bin        binary tree, for a machine with the same Qt
#   dist.sh appimage   self-contained AppImage, Qt bundled in
#   dist.sh all        all three
#
# Everything lands in dist/ at the top of the repo.
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
out=$repo/dist
# One source of truth for the version: project() in the top-level CMakeLists.
version=$(sed -n 's/^project(nirbija VERSION \([0-9.]*\).*/\1/p' "$repo/CMakeLists.txt")
arch=$(uname -m)

[ -n "$version" ] || { echo "dist: cannot read the version out of CMakeLists.txt" >&2; exit 1; }

say() { printf '\n== %s\n' "$*"; }

# --- source ------------------------------------------------------------------

# git archive rather than a copy of the working tree: a release tarball should
# be exactly what is committed, with no build output and no local scratch.
do_src() {
  say "source tarball"
  cd "$repo"
  if [ -n "$(git status --porcelain)" ]; then
    echo "dist: the working tree is dirty - the tarball will hold HEAD, not what is on disk" >&2
  fi
  local tar=$out/nirbija-$version-src.tar.gz
  git archive --format=tar --prefix="nirbija-$version/" HEAD | gzip -9 > "$tar"
  echo "$tar"
}

# --- build -------------------------------------------------------------------

build_release() {
  say "building"
  cmake -S "$repo" -B "$repo/build-release" \
    -DCMAKE_BUILD_TYPE=Release -DNIRBIJA_TESTS=OFF > /dev/null
  cmake --build "$repo/build-release" -j"$(nproc)"
}

# --- binary tree -------------------------------------------------------------

# Links against the Qt and JACK of the machine that built it, so this is for
# an identical distro. Anything else wants the AppImage.
do_bin() {
  build_release
  say "binary tarball"
  local stage=$out/nirbija-$version-$arch
  rm -rf "$stage"
  DESTDIR="$stage" cmake --install "$repo/build-release" --prefix /usr > /dev/null

  # Relocatable: the prefix is wherever the user unpacks it, so install.sh
  # rewrites Exec= rather than leaving the .desktop pointing at /usr/bin.
  cat > "$stage/install.sh" <<'EOF'
#!/usr/bin/env sh
# Installs this tree into ~/.local, so no root and nothing outside $HOME.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
prefix=${1:-$HOME/.local}
mkdir -p "$prefix/bin" "$prefix/share/applications" \
         "$prefix/share/icons/hicolor/scalable/apps" \
         "$prefix/share/icons/hicolor/256x256/apps"
cp "$here/usr/bin/nirbija" "$prefix/bin/"
# Only the Exec line: Icon= and StartupWMClass= also end in "nirbija" and
# must keep naming the icon and the window class, not a path.
sed "/^Exec=/ s|nirbija\$|$prefix/bin/nirbija|" \
    "$here/usr/share/applications/Nirbija.desktop" \
    > "$prefix/share/applications/Nirbija.desktop"
cp "$here/usr/share/icons/hicolor/scalable/apps/nirbija.svg" \
   "$prefix/share/icons/hicolor/scalable/apps/"
cp "$here/usr/share/icons/hicolor/256x256/apps/nirbija.png" \
   "$prefix/share/icons/hicolor/256x256/apps/"
command -v update-desktop-database > /dev/null 2>&1 &&
  update-desktop-database "$prefix/share/applications" || true
command -v gtk-update-icon-cache > /dev/null 2>&1 &&
  gtk-update-icon-cache -qtf "$prefix/share/icons/hicolor" || true
echo "installed into $prefix - make sure $prefix/bin is on your PATH"
EOF
  chmod +x "$stage/install.sh"

  # What the machine it was built on happened to link against, so a failure to
  # start on another box can be read rather than guessed at.
  { echo "nirbija $version  $arch"; echo; ldd "$stage/usr/bin/nirbija"; } \
    > "$stage/LINKED-AGAINST.txt"
  cp "$repo/README.md" "$repo/LICENSE" "$stage/"

  local tar=$out/nirbija-$version-$arch.tar.gz
  tar -C "$out" -czf "$tar" "nirbija-$version-$arch"
  rm -rf "$stage"
  echo "$tar"
}

# --- appimage ----------------------------------------------------------------

# linuxdeploy walks the binary's dependencies and copies them in; its qt plugin
# adds the QML modules and platform plugins, which a plain copy always misses.
fetch_tool() {
  # One `local` per line: bash expands every word of a `local` command before
  # it assigns any of them, so $name would still be unset on the same line.
  local name=$1
  local url=$2
  local dest=$out/tools/$name
  mkdir -p "$out/tools"
  if [ ! -x "$dest" ]; then
    # stderr: stdout is the path, and the caller captures it.
    echo "fetching $name" >&2
    curl -fL "$url" -o "$dest"
    chmod +x "$dest"
  fi
  echo "$dest"
}

do_appimage() {
  build_release
  say "AppImage"
  local base=https://github.com/linuxdeploy/linuxdeploy
  local ld ldqt
  ld=$(fetch_tool "linuxdeploy-$arch.AppImage" \
       "$base/releases/download/continuous/linuxdeploy-$arch.AppImage")
  ldqt=$(fetch_tool "linuxdeploy-plugin-qt-$arch.AppImage" \
       "$base-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$arch.AppImage")

  local root=$out/AppDir
  rm -rf "$root"
  DESTDIR="$root" cmake --install "$repo/build-release" --prefix /usr > /dev/null

  # QMAKE is not optional: left to itself the qt plugin takes whichever qmake is
  # first on PATH, here a Qt 5 one, finds none of the Qt 6 this actually links
  # against and stops with "Could not find Qt modules to deploy".
  local qmake
  qmake=$(command -v qmake6 || command -v qmake) ||
    { echo "dist: no qmake6 on PATH - the qt plugin needs it" >&2; exit 1; }

  # QML_SOURCES_PATHS lets the qt plugin scan the imports the QML really uses,
  # so those modules travel inside the image instead of being looked for on
  # whatever machine runs it.
  # NO_STRIP because linuxdeploy carries its own, older, binutils: its strip
  # does not know the SHT_RELR sections a current linker emits and fails on
  # every system library it is handed. Nothing is lost but a few MB.
  #
  # QML_MODULES_PATHS points at the module the build generates, so the qt
  # plugin stops reporting `Nirbija` as a missing import - the QML itself
  # travels in the binary's resources, but the scanner still wants to see it.
  QMAKE=$qmake \
  NO_STRIP=1 \
  QML_SOURCES_PATHS=$repo/src/ui/qml \
  QML_MODULES_PATHS=$repo/build-release/qml \
  OUTPUT="nirbija-$version-$arch.AppImage" \
    "$ld" --appdir "$root" --plugin qt \
      -d "$root/usr/share/applications/Nirbija.desktop" \
      -i "$root/usr/share/icons/hicolor/256x256/apps/nirbija.png" \
      --output appimage
  mv "nirbija-$version-$arch.AppImage" "$out/"
  rm -rf "$root"
  echo "$out/nirbija-$version-$arch.AppImage"
}

# --- ------------------------------------------------------------------------

mkdir -p "$out"
case "${1:-all}" in
  src)      do_src ;;
  bin)      do_bin ;;
  appimage) do_appimage ;;
  all)      do_src; do_bin; do_appimage ;;
  *) echo "usage: dist.sh [src|bin|appimage|all]" >&2; exit 1 ;;
esac

say "in $out"
ls -la "$out"
