#pragma once

#include <QColor>
#include <QObject>

namespace nirbija {

// The mixer's palette and metrics, exposed to QML as the `Skin` context
// property. This lives in C++ rather than a QML singleton so every component
// reads the same object without depending on module registration order.
class Skin : public QObject {
  Q_OBJECT

#define NIRBIJA_SKIN_COLOR(name, value)                                   \
  Q_PROPERTY(QColor name READ name CONSTANT)                              \
 public:                                                                  \
  QColor name() const { return QColor(QStringLiteral(value)); }           \
                                                                          \
 private:

  // Dark, flat, low contrast between panels, so the coloured strips and the
  // meters are what the eye lands on.
  NIRBIJA_SKIN_COLOR(background, "#141414")
  NIRBIJA_SKIN_COLOR(bar, "#1e1e1e")
  NIRBIJA_SKIN_COLOR(strip, "#242424")
  NIRBIJA_SKIN_COLOR(slot, "#2e2e2e")
  NIRBIJA_SKIN_COLOR(slotEmpty, "#1a1a1a")
  NIRBIJA_SKIN_COLOR(line, "#0d0d0d")

  NIRBIJA_SKIN_COLOR(text, "#e6e6e6")
  NIRBIJA_SKIN_COLOR(textDim, "#8a8a8a")

  NIRBIJA_SKIN_COLOR(mute, "#d24b4b")
  NIRBIJA_SKIN_COLOR(solo, "#e0b23c")
  NIRBIJA_SKIN_COLOR(arm, "#d24b4b")
  NIRBIJA_SKIN_COLOR(accent, "#4a9eda")

  NIRBIJA_SKIN_COLOR(meterLow, "#4cae4c")
  NIRBIJA_SKIN_COLOR(meterMid, "#d9c341")
  NIRBIJA_SKIN_COLOR(meterHigh, "#d24b4b")

#undef NIRBIJA_SKIN_COLOR

  Q_PROPERTY(int stripWidth READ stripWidth CONSTANT)
  Q_PROPERTY(int slotHeight READ slotHeight CONSTANT)
  Q_PROPERTY(int gap READ gap CONSTANT)
  Q_PROPERTY(int radius READ radius CONSTANT)
  Q_PROPERTY(int barHeight READ barHeight CONSTANT)

 public:
  using QObject::QObject;

  int stripWidth() const { return 108; }
  int slotHeight() const { return 34; }
  int gap() const { return 3; }
  int radius() const { return 3; }
  int barHeight() const { return 44; }

  // Meters go green, then yellow, then red near the top of the scale.
  Q_INVOKABLE QColor meterColor(qreal level) const {
    if (level > 0.89) return meterHigh();
    if (level > 0.6) return meterMid();
    return meterLow();
  }
};

}  // namespace nirbija
