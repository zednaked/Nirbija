#pragma once

#include <QAbstractListModel>
#include <QStringList>
#include <QTimer>

#include <memory>
#include <vector>

#include "core/engine.h"
#include "core/plugin.h"
#include "plugin_list_model.h"
#include "plugin_window.h"

namespace nirbija {

// Bridges the audio engine to QML. Every write goes through the engine's
// lock-free queue; every read of a level is a plain atomic load, polled on a
// timer rather than pushed, so a stalled UI cannot back up the audio thread.
class MixerModel : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(bool running READ running NOTIFY runningChanged)
  Q_PROPERTY(QString status READ status NOTIFY statusChanged)
  Q_PROPERTY(double sampleRate READ sampleRate NOTIFY runningChanged)
  Q_PROPERTY(int blockFrames READ blockFrames NOTIFY runningChanged)
  Q_PROPERTY(qreal masterPeakLeft READ masterPeakLeft NOTIFY levelsChanged)
  Q_PROPERTY(qreal masterPeakRight READ masterPeakRight NOTIFY levelsChanged)
  Q_PROPERTY(qreal masterGain READ masterGain WRITE setMasterGain NOTIFY masterGainChanged)
  Q_PROPERTY(QString masterSink READ masterSink NOTIFY routingChanged)
  Q_PROPERTY(bool playing READ playing NOTIFY transportChanged)
  Q_PROPERTY(qreal tempo READ tempo WRITE setTempo NOTIFY transportChanged)
  Q_PROPERTY(nirbija::PluginListModel* plugins READ plugins CONSTANT)

 public:
  enum Roles {
    NameRole = Qt::UserRole + 1,
    GainRole,
    PanRole,
    MutedRole,
    SoloedRole,
    ArmedRole,
    PeakLeftRole,
    PeakRightRole,
    InputLabelRole,
    OutputLabelRole,
    MidiLabelRole,
    InsertsRole,
    WidthRole,
    AccentRole,
  };

  explicit MixerModel(QObject* parent = nullptr);
  ~MixerModel() override;

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  bool running() const { return engine_.running(); }
  QString status() const { return status_; }
  double sampleRate() const { return engine_.sample_rate(); }
  int blockFrames() const { return static_cast<int>(engine_.block_frames()); }
  qreal masterPeakLeft() const { return master_peak_[0]; }
  qreal masterPeakRight() const { return master_peak_[1]; }
  qreal masterGain() const { return master_gain_; }
  QString masterSink() const;
  bool playing() const { return engine_.playing(); }
  qreal tempo() const { return engine_.tempo(); }
  void setTempo(qreal bpm);
  PluginListModel* plugins() const { return plugins_.get(); }
  void setMasterGain(qreal gain);

  Q_INVOKABLE void togglePlay();
  Q_INVOKABLE void rewind();

  Q_INVOKABLE void addChannel(const QString& name, int channels);
  Q_INVOKABLE void removeChannel(int row);
  Q_INVOKABLE void renameChannel(int row, const QString& name);
  Q_INVOKABLE void setGain(int row, qreal gain);
  Q_INVOKABLE void setPan(int row, qreal pan);
  Q_INVOKABLE void toggleMute(int row);
  Q_INVOKABLE void toggleSolo(int row);
  Q_INVOKABLE void toggleArm(int row);

  // `pluginIndex` refers to the row of the shared PluginListModel.
  Q_INVOKABLE bool addInsert(int row, int pluginIndex);
  Q_INVOKABLE void removeInsert(int row, int slot);

  // Opens the plugin's own editor in its own window. False when the plugin
  // ships no editor this host can embed, which is common.
  Q_INVOKABLE bool openInsertEditor(int row, int slot);

  // --- routing ------------------------------------------------------------
  // Ports a channel can be fed from, ready to show in a picker. `midi` picks
  // between MIDI sources and audio ones.
  Q_INVOKABLE QStringList sources(bool midi) const;
  Q_INVOKABLE QStringList sinks() const;

  Q_INVOKABLE void connectSource(int row, const QString& port, bool midi);
  Q_INVOKABLE void connectMaster(const QString& port);

  // The short form of a port name, for a label that has to fit in a strip.
  Q_INVOKABLE static QString shortPortName(const QString& port);

  // --- session ------------------------------------------------------------
  // There is no save dialog: the session is written continuously and restored
  // on the next start, so closing the app and reopening it lands you where you
  // left off.
  void saveSession() const;
  void loadSession();
  static QString sessionPath();

  // Turns a fader position in 0..1 into a linear gain, and back. AUM's fader is
  // not linear in amplitude: most of the travel covers the top of the range.
  Q_INVOKABLE static qreal faderToGain(qreal position);
  Q_INVOKABLE static qreal gainToFader(qreal gain);
  Q_INVOKABLE static QString gainLabel(qreal gain);

 signals:
  void runningChanged();
  void statusChanged();
  void levelsChanged();
  void masterGainChanged();
  void routingChanged();
  void transportChanged();

 private:
  struct ChannelUi {
    // Which graph slot this row drives. Removing a row leaves the slots around
    // it where they were, so the two indices drift apart and only this mapping
    // is safe to hand the engine.
    size_t slot = 0;
    QString name;
    int width = 2;
    qreal gain = 1.0;
    qreal pan = 0.0;
    bool muted = false;
    bool soloed = false;
    bool armed = false;
    qreal peak[2] = {0.0, 0.0};
    QString input_label;
    QString output_label;
    QString midi_label;
    QStringList inserts;
    QString accent;
  };

  void pollLevels();
  void refreshRouting(int row);

  // Coalesces the writes: a fader drag would otherwise save on every frame.
  void markDirty();
  void post(EngineCommand::Kind kind, int row, float value);

  // Declared before the engine so it outlives it: strips hold plugin instances
  // that belong to the backends this model owns.
  std::unique_ptr<PluginListModel> plugins_;
  Engine engine_;
  std::vector<ChannelUi> channels_;
  QTimer level_timer_;
  QTimer autosave_timer_;
  // Restoring fires the same setters the UI does; without this every one of
  // them would queue another save of what was just loaded.
  bool restoring_ = false;
  // Editor windows stay owned here so closing the mixer closes them too. The
  // slot tags each one, so removing a channel closes only its own editors.
  struct OpenEditor {
    size_t slot;
    std::unique_ptr<PluginWindow> window;
  };
  std::vector<OpenEditor> editors_;
  qreal master_peak_[2] = {0.0, 0.0};
  qreal master_gain_ = 1.0;
  QString status_;
};

}  // namespace nirbija
