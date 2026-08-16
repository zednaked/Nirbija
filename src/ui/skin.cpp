#include "skin.h"

#include <QSettings>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>

namespace nirbija {
namespace {

// Remembered per machine, not per session: how big the interface should be is
// a fact about the screen in front of you, and carrying it inside a session
// would make a jam opened on a laptop resize the desktop it came from.
constexpr auto kScaleKey = "ui/scale";

qreal clamped(qreal value) {
  return std::clamp(value, Skin::kMinScale, Skin::kMaxScale);
}

}  // namespace

qreal Skin::startingScale() {
  bool ok = false;
  const qreal env = qEnvironmentVariable("NIRBIJA_UI_SCALE").toDouble(&ok);
  return ok ? clamped(env) : 1.0;
}

// The environment wins when it is set - someone who typed NIRBIJA_UI_SCALE
// this time meant it - and otherwise the size last chosen here comes back.
qreal Skin::scale_ = [] {
  if (!qEnvironmentVariableIsEmpty("NIRBIJA_UI_SCALE")) return startingScale();
  const QSettings settings;
  return clamped(settings.value(QString::fromLatin1(kScaleKey), 1.0).toReal());
}();

void Skin::setScale(qreal value) {
  const qreal wanted = clamped(value);
  // Sizes are whole pixels, so two scales a hair apart draw identically and
  // the second one would be a repaint that changes nothing.
  if (qFuzzyCompare(wanted, scale_)) return;
  scale_ = wanted;

  QSettings settings;
  settings.setValue(QString::fromLatin1(kScaleKey), scale_);
  emit scaleChanged();
}

// A fader with no marks on it is a slider: you can move it, but you cannot say
// where it is without reading the number somewhere else. These are the decibel
// values worth a line, sparse at the quiet end where the travel compresses.
QVariantList Skin::faderTicks() {
  static const QVariantList ticks = [] {
    constexpr double kMarks[] = {6.0, 0.0, -6.0, -12.0, -24.0, -48.0};
    QVariantList out;
    for (const double db : kMarks) {
      QVariantMap tick;
      tick.insert(QStringLiteral("db"), db);
      tick.insert(QStringLiteral("position"), (db - kMinDb) / (kMaxDb - kMinDb));
      tick.insert(QStringLiteral("label"),
                  db > 0.0 ? QStringLiteral("+%1").arg(db, 0, 'f', 0)
                           : QString::number(db, 'f', 0));
      // Unity is the one a hand looks for, so it is drawn heavier.
      tick.insert(QStringLiteral("major"), db == 0.0);
      out.append(tick);
    }
    return out;
  }();
  return ticks;
}

}  // namespace nirbija
