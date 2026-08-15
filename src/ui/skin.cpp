#include "skin.h"

#include <QVariantList>
#include <QVariantMap>

namespace nirbija {

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
