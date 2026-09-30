// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "screen_capture.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <cmath>
#include <csignal>
#include <unistd.h>

namespace nirbija {

namespace {

const QString kRecorder = QStringLiteral("gpu-screen-recorder");

// Below the UI and far below the audio thread: neither the picture nor the
// join is worth a late block.
void lower_priority() { (void)::nice(10); }

}  // namespace

ScreenCapture::ScreenCapture(QObject* parent) : QObject(parent) {}

ScreenCapture::~ScreenCapture() {
  // Closing the mixer mid-take still leaves a playable picture: the recorder
  // is asked to finish its file. The join is skipped; the take has its WAVs.
  if (process_ != nullptr) {
    process_->disconnect(this);
    ::kill(static_cast<pid_t>(process_->processId()), SIGINT);
    process_->waitForFinished(5000);
  }
}

bool ScreenCapture::available() {
  return !QStandardPaths::findExecutable(kRecorder).isEmpty();
}

bool ScreenCapture::start(const QString& take_dir, const QString& monitor) {
  if (process_ != nullptr) return false;
  if (!available()) {
    emit message(tr("Screen not recorded: gpu-screen-recorder is not installed"));
    return false;
  }

  take_dir_ = take_dir;
  video_ = take_dir + QStringLiteral("/screen.mkv");
  stopping_ = false;

  process_ = new QProcess(this);
  process_->setProcessChannelMode(QProcess::MergedChannels);
  process_->setChildProcessModifier(lower_priority);
  // HEVC at 30 frames, only as often as the screen changes: a mixer is mostly
  // still, and a still screen costs next to nothing. No audio of its own.
  const QStringList arguments = {
      QStringLiteral("-w"), monitor,
      QStringLiteral("-c"), QStringLiteral("mkv"),
      QStringLiteral("-k"), QStringLiteral("hevc"),
      QStringLiteral("-f"), QStringLiteral("30"),
      QStringLiteral("-q"), QStringLiteral("high"),
      QStringLiteral("-cursor"), QStringLiteral("yes"),
      QStringLiteral("-write-first-frame-ts"), QStringLiteral("yes"),
      QStringLiteral("-o"), video_};

  connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
    QProcess* done = process_;
    process_ = nullptr;
    const QString said = QString::fromLocal8Bit(done->readAll()).trimmed();
    done->deleteLater();
    if (!stopping_) {
      // It stopped on its own: no monitor by that name, no GPU encoder.
      qWarning("screen: gpu-screen-recorder exited early (%d): %s", code,
               qPrintable(said.right(400)));
      emit message(tr("Screen recording stopped: %1")
                       .arg(said.section('\n', -1).left(160)));
      return;
    }
    mux();
  });
  process_->start(kRecorder, arguments);
  if (!process_->waitForStarted(3000)) {
    emit message(tr("Screen not recorded: gpu-screen-recorder did not start"));
    delete process_;
    process_ = nullptr;
    return false;
  }
  return true;
}

void ScreenCapture::stop(const QString& master_wav, quint64 audio_first_usecs) {
  if (process_ == nullptr) return;
  master_wav_ = master_wav;
  audio_first_usecs_ = audio_first_usecs;
  stopping_ = true;
  // SIGINT is its "stop and save".
  ::kill(static_cast<pid_t>(process_->processId()), SIGINT);
}

bool ScreenCapture::readFirstFrame(const QString& ts_path, quint64* usecs) {
  QFile file(ts_path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
  // A header line, then "<monotonic_microsec>\t<realtime_microsec>".
  file.readLine();
  const QList<QByteArray> fields = file.readLine().trimmed().split('\t');
  bool ok = false;
  const quint64 value = fields.value(0).toULongLong(&ok);
  if (!ok || value == 0) return false;
  *usecs = value;
  return true;
}

QStringList ScreenCapture::muxArguments(const QString& video, const QString& audio,
                                        double offset_seconds, const QString& out) {
  QStringList arguments = {QStringLiteral("-y"), QStringLiteral("-hide_banner"),
                           QStringLiteral("-loglevel"), QStringLiteral("error"),
                           QStringLiteral("-i"), video};
  // Sound that began after the picture is pushed later; sound that began
  // before it has its head cut, so the film starts with the picture.
  const QString amount = QString::number(std::abs(offset_seconds), 'f', 6);
  if (offset_seconds >= 0.0)
    arguments << QStringLiteral("-itsoffset") << amount;
  else
    arguments << QStringLiteral("-ss") << amount;
  arguments << QStringLiteral("-i") << audio
            << QStringLiteral("-map") << QStringLiteral("0:v:0")
            << QStringLiteral("-map") << QStringLiteral("1:a:0")
            << QStringLiteral("-c:v") << QStringLiteral("copy")
            // hvc1 is the tag Apple's players want for HEVC in MP4.
            << QStringLiteral("-tag:v") << QStringLiteral("hvc1")
            << QStringLiteral("-c:a") << QStringLiteral("aac")
            << QStringLiteral("-b:a") << QStringLiteral("320k")
            << QStringLiteral("-movflags") << QStringLiteral("+faststart")
            << out;
  return arguments;
}

void ScreenCapture::mux() {
  const QString ts_path = video_ + QStringLiteral(".ts");
  quint64 video_first = 0;
  if (!QFileInfo::exists(video_) || !readFirstFrame(ts_path, &video_first)) {
    emit message(tr("Screen recording left no picture to join"));
    return;
  }
  if (master_wav_.isEmpty() || audio_first_usecs_ == 0) {
    emit message(tr("Screen saved without sound: %1").arg(video_));
    return;
  }

  const double offset =
      (static_cast<double>(audio_first_usecs_) - static_cast<double>(video_first)) /
      1e6;
  const QString out =
      take_dir_ + QLatin1Char('/') + QFileInfo(take_dir_).fileName() +
      QStringLiteral(".mp4");
  qInfo("screen: joining, sound %+.3f s from the picture", offset);

  auto* ffmpeg = new QProcess(this);
  ffmpeg->setProcessChannelMode(QProcess::MergedChannels);
  ffmpeg->setChildProcessModifier(lower_priority);
  const QString video = video_;
  connect(ffmpeg, &QProcess::finished, this,
          [this, ffmpeg, video, ts_path, out](int code, QProcess::ExitStatus status) {
            const QString said = QString::fromLocal8Bit(ffmpeg->readAll()).trimmed();
            ffmpeg->deleteLater();
            if (status != QProcess::NormalExit || code != 0 || !QFileInfo::exists(out)) {
              qWarning("screen: ffmpeg failed (%d): %s", code, qPrintable(said.right(400)));
              emit message(tr("Could not join the screen to the take; the picture "
                              "is kept at %1").arg(video));
              return;
            }
            // The film holds the picture as it was, stream-copied: the loose
            // one would be the same bytes twice.
            QFile::remove(video);
            QFile::remove(ts_path);
            emit message(tr("Film saved: %1").arg(out));
          });
  ffmpeg->start(QStringLiteral("ffmpeg"), muxArguments(video_, master_wav_, offset, out));
  if (!ffmpeg->waitForStarted(3000)) {
    ffmpeg->disconnect(this);
    ffmpeg->deleteLater();
    emit message(tr("ffmpeg is not installed; the picture is kept at %1").arg(video_));
  }
}

}  // namespace nirbija
