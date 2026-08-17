#pragma once

#include <QColor>
#include <QFontDatabase>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <qqmlintegration.h>

namespace nirbija {

// The mixer's design tokens — palette, spacing, type scale and timings —
// exposed to QML as the `Skin` singleton of the Nirbija module.
//
// A registered singleton rather than a context property: a context property is
// invisible to qmllint and to the QML compiler, so every `Skin.foo` in the tree
// stayed an unqualified runtime lookup. Registered, the same expressions have a
// type at build time.
//
// Every colour is parsed once into a static, because these are read from
// bindings that re-evaluate while meters run — the old form built a QColor from
// a string on each read.
class Skin : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON

#define NIRBIJA_SKIN_COLOR(name, value)                                   \
  Q_PROPERTY(QColor name READ name CONSTANT)                              \
 public:                                                                  \
  static QColor name() {                                                  \
    static const QColor cached(QStringLiteral(value));                    \
    return cached;                                                        \
  }                                                                       \
                                                                          \
 private:

  // --- surfaces ------------------------------------------------------------
  // Dark and near-neutral, with only a few points of lightness between the
  // layers, so the coloured strips and the meters are what the eye lands on.
  NIRBIJA_SKIN_COLOR(background, "#0f1113")
  NIRBIJA_SKIN_COLOR(bar, "#16191d")
  NIRBIJA_SKIN_COLOR(strip, "#1d2126")
  NIRBIJA_SKIN_COLOR(stripAlt, "#20242a")  // hovered strip / raised row
  NIRBIJA_SKIN_COLOR(slot, "#272c33")
  NIRBIJA_SKIN_COLOR(slotHover, "#313740")
  NIRBIJA_SKIN_COLOR(slotEmpty, "#15181c")
  NIRBIJA_SKIN_COLOR(popup, "#22262c")

  // `line` is the hairline between surfaces; `border` is the one that has to be
  // seen from a metre away. They were the same colour, which is why every
  // outline in the mixer read as noise instead of as an edge.
  NIRBIJA_SKIN_COLOR(line, "#0b0d0f")
  NIRBIJA_SKIN_COLOR(border, "#343a42")

  // --- text ----------------------------------------------------------------
  // textDim was #8a8a8a on #242424: 3.4:1, under the 4.5:1 that small text
  // needs. `disabled` is separate now — disabled entries used to be painted in
  // `line`, which on a menu is 1.3:1 and effectively invisible.
  NIRBIJA_SKIN_COLOR(text, "#e9ecef")
  NIRBIJA_SKIN_COLOR(textDim, "#a2a9b2")
  NIRBIJA_SKIN_COLOR(disabled, "#5c636c")
  NIRBIJA_SKIN_COLOR(onAccent, "#0a1219")  // text drawn on top of an accent fill

  // --- state ---------------------------------------------------------------
  NIRBIJA_SKIN_COLOR(mute, "#e05a5a")
  NIRBIJA_SKIN_COLOR(solo, "#e8c04a")
  NIRBIJA_SKIN_COLOR(arm, "#e0483f")
  NIRBIJA_SKIN_COLOR(accent, "#4fa3e3")
  NIRBIJA_SKIN_COLOR(focus, "#7cc4ff")  // keyboard focus ring

  // --- meters --------------------------------------------------------------
  NIRBIJA_SKIN_COLOR(meterLow, "#45c17a")
  NIRBIJA_SKIN_COLOR(meterMid, "#e5c452")
  NIRBIJA_SKIN_COLOR(meterHigh, "#e05252")
  NIRBIJA_SKIN_COLOR(meterClip, "#ff4d3d")
  NIRBIJA_SKIN_COLOR(meterTrack, "#0c0e10")

#undef NIRBIJA_SKIN_COLOR

 public:
  using QObject::QObject;

  // --- metrics -------------------------------------------------------------
  // Everything is derived from `scale`, so a HiDPI screen or a touchscreen can
  // be served by one number instead of by editing every literal in the tree.
  // NIRBIJA_UI_SCALE=1.25 is a comfortable size on a 4K panel.
  Q_PROPERTY(qreal scale READ scale NOTIFY scaleChanged)

  Q_PROPERTY(int spacingXS READ spacingXS NOTIFY scaleChanged)
  Q_PROPERTY(int spacingS READ spacingS NOTIFY scaleChanged)
  Q_PROPERTY(int spacing READ spacing NOTIFY scaleChanged)
  Q_PROPERTY(int spacingL READ spacingL NOTIFY scaleChanged)
  Q_PROPERTY(int gap READ gap NOTIFY scaleChanged)

  Q_PROPERTY(int radiusS READ radiusS NOTIFY scaleChanged)
  Q_PROPERTY(int radius READ radius NOTIFY scaleChanged)
  Q_PROPERTY(int radiusL READ radiusL NOTIFY scaleChanged)

  Q_PROPERTY(int stripWidth READ stripWidth NOTIFY scaleChanged)
  Q_PROPERTY(int slotHeight READ slotHeight NOTIFY scaleChanged)
  Q_PROPERTY(int barHeight READ barHeight NOTIFY scaleChanged)
  Q_PROPERTY(int buttonHeight READ buttonHeight NOTIFY scaleChanged)
  Q_PROPERTY(int rowHeight READ rowHeight NOTIFY scaleChanged)
  // The smallest thing a finger can be asked to hit. Everything interactive
  // either meets this or grows an invisible margin until it does.
  Q_PROPERTY(int touchTarget READ touchTarget NOTIFY scaleChanged)

  Q_PROPERTY(int fontXS READ fontXS NOTIFY scaleChanged)
  Q_PROPERTY(int fontS READ fontS NOTIFY scaleChanged)
  Q_PROPERTY(int font READ font NOTIFY scaleChanged)
  Q_PROPERTY(int fontL READ fontL NOTIFY scaleChanged)
  Q_PROPERTY(int fontXL READ fontXL NOTIFY scaleChanged)
  // Numbers that change while you watch them — dB, BPM, bar.beat — in a face
  // whose digits are all one width, so the label stops twitching.
  Q_PROPERTY(QString monoFamily READ monoFamily CONSTANT)

  // Motion: short enough to read as feedback rather than as animation. One
  // place to turn it off, since a mixer is watched for hours.
  Q_PROPERTY(int fast READ fast CONSTANT)
  Q_PROPERTY(int medium READ medium CONSTANT)
  Q_PROPERTY(int tipDelay READ tipDelay CONSTANT)

  // The one number every size here is derived from. NIRBIJA_UI_SCALE still
  // sets it at startup; so does whatever was last chosen from the keyboard,
  // which is remembered per machine rather than per session - how big the
  // interface should be is a fact about the screen in front of you.
  static qreal scale() { return scaleRef(); }

  static constexpr qreal kMinScale = 0.6;
  static constexpr qreal kMaxScale = 3.0;

  // Steps rather than a free number: a size worth having is one you can get
  // back to, and twelve percent a press is coarse enough to feel and fine
  // enough to land on comfortable.
  Q_INVOKABLE void zoomIn() { setScale(scaleRef() * 1.12); }
  Q_INVOKABLE void zoomOut() { setScale(scaleRef() / 1.12); }
  Q_INVOKABLE void zoomReset() { setScale(startingScale()); }
  Q_INVOKABLE void setScale(qreal value);

  // What the environment asked for, or 1.0 - the size a reset goes back to.
  static qreal startingScale();

 signals:
  void scaleChanged();

 public:

  // For the handful of sizes that are local to one component and not worth a
  // token of their own — they still have to follow the scale.
  Q_INVOKABLE static int px(qreal value) {
    return static_cast<int>(value * scale() + 0.5);
  }

  static int spacingXS() { return px(2); }
  static int spacingS() { return px(4); }
  static int spacing() { return px(8); }
  static int spacingL() { return px(12); }
  // Kept under the old name: it is the gutter between strips and inside them.
  static int gap() { return px(4); }

  static int radiusS() { return px(2); }
  static int radius() { return px(4); }
  static int radiusL() { return px(8); }

  static int stripWidth() { return px(116); }
  static int slotHeight() { return px(34); }
  static int barHeight() { return px(48); }
  static int buttonHeight() { return px(28); }
  static int rowHeight() { return px(32); }
  static int touchTarget() { return px(32); }

  static int fontXS() { return px(9); }
  static int fontS() { return px(10); }
  static int font() { return px(11); }
  static int fontL() { return px(13); }
  static int fontXL() { return px(16); }

  static QString monoFamily() {
    static const QString family =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    return family;
  }

  static int fast() { return 90; }
  static int medium() { return 160; }
  static int tipDelay() { return 450; }

  // Meters go green, then yellow, then red. The thresholds are in decibels —
  // yellow from -6 dBFS, red from -1 — converted to the same 0..1 the fader
  // uses. Comparing raw fractions here made an ordinary signal look like it was
  // about to clip.
  Q_INVOKABLE static QColor meterColor(qreal level) {
    if (level > kRedPosition) return meterHigh();
    if (level > kYellowPosition) return meterMid();
    return meterLow();
  }

  // Where the two meter colours change, as a fraction of the meter's height, so
  // a gradient can be built once in QML instead of a colour being recomputed
  // from a binding on every frame.
  Q_PROPERTY(qreal meterYellowAt READ meterYellowAt CONSTANT)
  Q_PROPERTY(qreal meterRedAt READ meterRedAt CONSTANT)
  static qreal meterYellowAt() { return kYellowPosition; }
  static qreal meterRedAt() { return kRedPosition; }

  // Where unity sits on the fader's travel, for the tick that marks it.
  Q_PROPERTY(qreal unityAt READ unityAt CONSTANT)
  static qreal unityAt() { return (0.0 - kMinDb) / (kMaxDb - kMinDb); }

  // The dB marks worth drawing beside a fader, top to bottom.
  Q_INVOKABLE static QVariantList faderTicks();

 private:
  // A function-local static rather than a namespace-scope one: the latter
  // would run during static initialization, before main() has had a chance
  // to call QApplication::setOrganizationName(). QSettings reads under that
  // window found no organization set, so every restart came back to 1.0
  // instead of whatever the last session left the scale at. This one builds
  // on first use instead, which for a QML-driven app is well after main().
  static qreal& scaleRef();

 public:

 private:
  // Must match MixerModel's fader range.
  static constexpr qreal kMinDb = -70.0;
  static constexpr qreal kMaxDb = 6.0;
  static constexpr qreal kYellowPosition = (-6.0 - kMinDb) / (kMaxDb - kMinDb);
  static constexpr qreal kRedPosition = (-1.0 - kMinDb) / (kMaxDb - kMinDb);
};

}  // namespace nirbija
