// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// The two halves of lining a take up with its film that can go wrong without
// a screen: reading when the picture's first frame was taken, and turning the
// distance to the sound's first frame into ffmpeg's arguments.

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

#include "screen_capture.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

// The value right after `flag`, or empty when the flag is not there.
QString after(const QStringList& arguments, const QString& flag) {
  const qsizetype at = arguments.indexOf(flag);
  return at < 0 || at + 1 >= arguments.size() ? QString() : arguments.at(at + 1);
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  using nirbija::ScreenCapture;

  QTemporaryDir dir;
  const QString ts = dir.path() + "/screen.mkv.ts";
  {
    QFile file(ts);
    if (!file.open(QIODevice::WriteOnly)) return 1;
    // What gpu-screen-recorder 6 writes.
    file.write("monotonic_microsec\trealtime_microsec\n66486035052\t1790726798747658\n");
  }
  quint64 usecs = 0;
  if (!ScreenCapture::readFirstFrame(ts, &usecs) || usecs != 66486035052ull)
    fail("the first frame's monotonic time was not read");
  if (ScreenCapture::readFirstFrame(dir.path() + "/none.ts", &usecs))
    fail("a missing timestamp file read as a time");
  {
    QFile file(ts);
    if (!file.open(QIODevice::WriteOnly)) return 1;
    file.write("monotonic_microsec\trealtime_microsec\n");
  }
  if (ScreenCapture::readFirstFrame(ts, &usecs))
    fail("a timestamp file with no time read as one");

  // Sound that began after the picture waits for its moment.
  const QStringList later = ScreenCapture::muxArguments("v.mkv", "m.wav", 0.25, "o.mp4");
  if (after(later, "-itsoffset") != "0.250000") fail("later sound was not delayed");
  if (later.contains("-ss")) fail("later sound was also trimmed");
  if (later.indexOf("-itsoffset") > later.lastIndexOf("-i"))
    fail("the delay is not on the sound's input");

  // Sound that began first loses its head, so the film opens on the picture.
  const QStringList earlier =
      ScreenCapture::muxArguments("v.mkv", "m.wav", -0.279, "o.mp4");
  if (after(earlier, "-ss") != "0.279000") fail("earlier sound was not trimmed");
  if (earlier.contains("-itsoffset")) fail("earlier sound was also delayed");
  if (after(earlier, "-ss").isEmpty() || earlier.indexOf("-ss") > earlier.lastIndexOf("-i"))
    fail("the trim is not on the sound's input");

  // The picture is copied, never encoded twice; the film is the last word.
  if (after(later, "-c:v") != "copy") fail("the picture is re-encoded");
  if (later.last() != "o.mp4") fail("the output is not where it was asked");

  if (failures == 0) std::printf("screen capture: ok\n");
  return failures == 0 ? 0 : 1;
}
