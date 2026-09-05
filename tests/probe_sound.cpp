// Scratch probe: loads the session named by NIRBIJA_SESSION, presses play,
// and reports the master and per-strip peaks every half second for a few
// seconds. Not a ctest target.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>

#include <cstdio>

#include "mixer_model.h"

int main(int argc, char* argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  nirbija::MixerModel mixer;
  if (!mixer.running()) {
    std::printf("no audio server\n");
    return 1;
  }
  std::printf("rows %d playing %d limiter %d\n", mixer.rowCount(),
              mixer.playing() ? 1 : 0, mixer.masterLimiter() ? 1 : 0);
  if (!mixer.playing()) mixer.togglePlay();
  QElapsedTimer clock;
  clock.start();
  double master_max = 0.0;
  while (clock.elapsed() < 5000) {
    QCoreApplication::processEvents();
    const double m = mixer.masterPeakLeft();
    master_max = std::max(master_max, m);
    static qint64 next = 500;
    if (clock.elapsed() >= next) {
      next += 500;
      std::printf("t=%lld master %.4f", clock.elapsed(), m);
      for (int r = 0; r < mixer.rowCount(); ++r)
        std::printf("  row%d %.4f", r,
                    mixer.data(mixer.index(r), nirbija::MixerModel::PeakLeftRole)
                        .toDouble());
      std::printf("  pos %s\n", qUtf8Printable(mixer.positionLabel()));
    }
  }
  std::printf("master max %.4f\n", master_max);
  return 0;
}
