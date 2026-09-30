// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace nirbija {

// The screen, filmed alongside a take and joined to it when the take stops.
//
// The picture comes from gpu-screen-recorder: the GPU encodes it, so the CPU
// the audio thread needs is not spent on video, and it reads the monitor
// through KMS, so there is no portal dialog in the way of pressing Rec. The
// sound does not come from a capture at all. The take already has the master,
// sample for sample, and that is what goes into the film - no second trip
// through the sound server that could drop out on its own.
//
// Lining the two up is a matter of one clock. gpu-screen-recorder writes when
// its first frame was taken, in CLOCK_MONOTONIC microseconds; the engine keeps
// when the take's first frame was due, on JACK's clock, which is the same one.
// The difference is how far the sound sits from the start of the picture.
class ScreenCapture : public QObject {
  Q_OBJECT

 public:
  explicit ScreenCapture(QObject* parent = nullptr);
  ~ScreenCapture() override;

  // Starts filming `monitor` (a connector name, "eDP-1") into `take_dir`.
  // False when the recorder is not installed or is already running.
  bool start(const QString& take_dir, const QString& monitor);

  // Stops the picture and, once the recorder has closed its file, joins it to
  // `master_wav`, whose first frame was due at `audio_first_usecs`. The film
  // lands in the take folder, named after it; the loose picture is removed.
  void stop(const QString& master_wav, quint64 audio_first_usecs);

  bool running() const { return process_ != nullptr; }

  static bool available();

  // The first frame's CLOCK_MONOTONIC microseconds from the ".ts" file
  // gpu-screen-recorder writes beside its output.
  static bool readFirstFrame(const QString& ts_path, quint64* usecs);

  // ffmpeg's arguments to put `audio` under `video`, the audio's first frame
  // `offset_seconds` after the picture's (before it, when negative).
  static QStringList muxArguments(const QString& video, const QString& audio,
                                  double offset_seconds, const QString& out);

 signals:
  // Something to tell the person: a failure, or where the film went.
  void message(const QString& text);

 private:
  void mux();

  QProcess* process_ = nullptr;
  QString take_dir_;
  QString video_;
  QString master_wav_;
  quint64 audio_first_usecs_ = 0;
  bool stopping_ = false;
};

}  // namespace nirbija
