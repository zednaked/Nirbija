#pragma once

#include <QAbstractListModel>
#include <QStringList>
#include <QByteArray>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QJsonArray>
#include <QTimer>
#include <QVector>
#include <qqmlintegration.h>

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
  // Registered as the `Mixer` singleton of the Nirbija module rather than
  // pushed in as a context property: a context property has no type until the
  // binding runs, so qmllint could not check a single `mixer.foo` in the tree
  // and the QML compiler had to fall back to a lookup by name for every one.
  QML_NAMED_ELEMENT(Mixer)
  QML_SINGLETON
  Q_PROPERTY(bool running READ running NOTIFY runningChanged)
  Q_PROPERTY(QString status READ status NOTIFY statusChanged)
  Q_PROPERTY(double sampleRate READ sampleRate NOTIFY runningChanged)
  Q_PROPERTY(int blockFrames READ blockFrames NOTIFY runningChanged)
  Q_PROPERTY(qreal masterPeakLeft READ masterPeakLeft NOTIFY levelsChanged)
  Q_PROPERTY(qreal masterPeakRight READ masterPeakRight NOTIFY levelsChanged)
  // The same levels already mapped onto the fader's decibel travel, and the
  // held peak beside them. Done here because a meter binding that called
  // gainToFader() crossed into C++ several times per strip per frame, thirty
  // times a second, for a number this side already has.
  Q_PROPERTY(qreal masterPositionLeft READ masterPositionLeft NOTIFY levelsChanged)
  Q_PROPERTY(qreal masterPositionRight READ masterPositionRight NOTIFY levelsChanged)
  Q_PROPERTY(qreal masterHoldLeft READ masterHoldLeft NOTIFY levelsChanged)
  Q_PROPERTY(qreal masterHoldRight READ masterHoldRight NOTIFY levelsChanged)
  // Whether meters are worth computing at all: false while the window is
  // hidden. The engine is still serviced on the same tick — only the redraw
  // stops.
  Q_PROPERTY(bool metersActive READ metersActive WRITE setMetersActive NOTIFY
                 metersActiveChanged)
  Q_PROPERTY(qreal masterGain READ masterGain WRITE setMasterGain NOTIFY masterGainChanged)
  Q_PROPERTY(QString masterSink READ masterSink NOTIFY routingChanged)
  Q_PROPERTY(bool playing READ playing NOTIFY transportChanged)
  Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
  Q_PROPERTY(QString recordingLabel READ recordingLabel NOTIFY levelsChanged)
  Q_PROPERTY(qreal tempo READ tempo WRITE setTempo NOTIFY transportChanged)
  Q_PROPERTY(bool metronome READ metronome NOTIFY transportChanged)
  Q_PROPERTY(bool learning READ learning NOTIFY learnChanged)
  Q_PROPERTY(nirbija::PluginListModel* plugins READ plugins CONSTANT)
  Q_PROPERTY(QString positionLabel READ positionLabel NOTIFY levelsChanged)
  Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
  Q_PROPERTY(bool canUndo READ canUndo NOTIFY dirtyChanged)
  Q_PROPERTY(bool canRedo READ canRedo NOTIFY dirtyChanged)
  Q_PROPERTY(bool masterDim READ masterDim NOTIFY masterGainChanged)
  Q_PROPERTY(bool masterMute READ masterMute NOTIFY masterGainChanged)
  Q_PROPERTY(bool masterMono READ masterMono NOTIFY masterGainChanged)
  Q_PROPERTY(bool midiClock READ midiClock NOTIFY transportChanged)
  Q_PROPERTY(bool followMidiClock READ followMidiClock NOTIFY transportChanged)
  Q_PROPERTY(bool masterClip READ masterClip NOTIFY levelsChanged)

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
    PositionLeftRole,
    PositionRightRole,
    HoldLeftRole,
    HoldRightRole,
    InputLabelRole,
    OutputLabelRole,
    MidiLabelRole,
    InsertsRole,
    // The same chain with the state a slot needs to draw itself:
    // [{name, bypassed, postFader}]. Bypass used to be invisible until the
    // menu was opened, which is a poor place to keep "this is not being heard".
    InsertDetailsRole,
    WidthRole,
    AccentRole,
    IsBusRole,
    DestinationRole,
    SendsRole,
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
  qreal masterPositionLeft() const { return master_position_[0]; }
  qreal masterPositionRight() const { return master_position_[1]; }
  qreal masterHoldLeft() const { return master_hold_[0]; }
  qreal masterHoldRight() const { return master_hold_[1]; }
  bool metersActive() const { return meters_active_; }
  void setMetersActive(bool on);
  qreal masterGain() const { return master_gain_; }
  QString masterSink() const;
  bool playing() const {
    return engine_.follow_midi_clock() ? engine_.playing() : playing_ui_;
  }
  bool recording() const { return engine_.recording(); }
  QString recordingLabel() const;
  // The UI's own idea of tempo, except while an external MIDI clock is
  // driving it - then it comes from the audio thread instead, since that is
  // the only place it is known. Reading through to the engine unconditionally
  // is what forced setTempo() to delay its notification: the command a drag
  // just posted was not applied yet, so an immediate read echoed the old
  // value right back. Every other setter this UI writes (masterGain,
  // playing, metronome) keeps its own cache for the same reason.
  qreal tempo() const {
    return engine_.follow_midi_clock() ? engine_.tempo() : tempo_ui_;
  }
  void setTempo(qreal bpm);
  bool metronome() const { return metronome_ui_; }
  Q_INVOKABLE void toggleMetronome();
  PluginListModel* plugins() const { return plugins_.get(); }
  void setMasterGain(qreal gain);

  Q_INVOKABLE void togglePlay();

  // Starts recording every armed channel plus the master, or stops the take in
  // progress. Returns the folder the take went to, empty on failure.
  Q_INVOKABLE QString toggleRecord();

  Q_INVOKABLE static QString recordingsPath();
  Q_INVOKABLE void rewind();

  // Returns the new row, or -1 if the graph is already at its channel/bus cap.
  Q_INVOKABLE int addChannel(const QString& name, int channels);
  Q_INVOKABLE int addBus(const QString& name);

  // Makes a bus and points `row` at it, which is how one strip comes to feed
  // another. Returns the new bus's row.
  Q_INVOKABLE int sendRowToNewBus(int row);

  // Where a row sends its output: -1 is the master, otherwise a bus index.
  Q_INVOKABLE void setDestination(int row, int destination);

  // Rows a given row is allowed to send to, as [{label, destination}]. A bus
  // may only feed a bus that renders after it, or the master.
  Q_INVOKABLE QVariantList destinationsFor(int row) const;

  // A send is a scaled copy of the strip's output going to a bus, on top of
  // whatever its destination is. `level` is linear, 0 removes the send.
  Q_INVOKABLE void setSend(int row, int slot, int bus, qreal level);
  Q_INVOKABLE void removeSend(int row, int slot);
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

  // Moves an insert one place up or down the chain. `direction` is -1 or +1.
  Q_INVOKABLE void moveInsert(int row, int slot, int direction);

  // Opens the plugin's own editor in its own window. False when the plugin
  // ships no editor this host can embed, which is common.
  Q_INVOKABLE bool openInsertEditor(int row, int slot);

  // For plugins that ship no editor this host can embed: the parameter list,
  // ready to draw as sliders. [{id, name, min, max, value}]
  Q_INVOKABLE QVariantList insertParameters(int row, int slot) const;
  Q_INVOKABLE void setInsertParameter(int row, int slot, int id, qreal value);
  Q_INVOKABLE QString insertName(int row, int slot) const;
  Q_INVOKABLE void closeAllEditors();

  // Clears the mixer back to nothing and saves that as the session.
  Q_INVOKABLE void newSession();
  Q_INVOKABLE bool shouldSeedSession() const { return seed_empty_session_; }

  Q_INVOKABLE bool addInsertAt(int row, int pluginIndex, int targetSlot);

  Q_INVOKABLE void undo();
  Q_INVOKABLE void redo();
  bool canUndo() const { return !undo_stack_.isEmpty(); }
  bool canRedo() const { return !redo_stack_.isEmpty(); }
  bool dirty() const { return dirty_flag_; }
  QString positionLabel() const;

  Q_INVOKABLE void duplicateChannel(int row);
  Q_INVOKABLE void moveChannel(int row, int direction);

  // The drag-to-reorder trio, called once each per gesture rather than once
  // per strip crossed - see moveChannelLiveBy() in mixer_model.cpp for why.
  Q_INVOKABLE void beginChannelReorder();
  Q_INVOKABLE void moveChannelLiveBy(int row, int steps);
  Q_INVOKABLE void endChannelReorder();

  Q_INVOKABLE void setInsertBypassed(int row, int slot, bool on);
  Q_INVOKABLE bool insertBypassed(int row, int slot) const;
  Q_INVOKABLE void setInsertPostFader(int row, int slot, bool on);
  Q_INVOKABLE bool insertPostFader(int row, int slot) const;
  Q_INVOKABLE int extraOutputPairs(int row, int slot) const;
  Q_INVOKABLE void addTapChannels(int row, int slot);
  Q_INVOKABLE void setMidiMask(int row, int mask);
  Q_INVOKABLE int midiMask(int row) const;
  Q_INVOKABLE void sendNote(int row, int note, int velocity);
  Q_INVOKABLE void connectChannelSink(int row, const QString& port);
  Q_INVOKABLE QString channelSink(int row) const;
  Q_INVOKABLE void setSidechain(int row, int sourceRow);
  Q_INVOKABLE int sidechainRow(int row) const;

  Q_INVOKABLE bool insertIsLooper(int row, int slot) const;
  Q_INVOKABLE bool insertIsFxPad(int row, int slot) const;
  Q_INVOKABLE void setFxPad(int row, int slot, int pad, bool on);
  Q_INVOKABLE bool fxPadOn(int row, int slot, int pad) const;
  Q_INVOKABLE void setFxPadAmount(int row, int slot, int pad, qreal amount);
  Q_INVOKABLE qreal fxPadAmount(int row, int slot, int pad) const;
  Q_INVOKABLE bool fxPadBipolar(int pad) const;
  Q_INVOKABLE void setFxPadHold(int row, int slot, bool on);
  Q_INVOKABLE bool fxPadHold(int row, int slot) const;
  Q_INVOKABLE void setLooperRecord(int row, int slot, bool on);
  Q_INVOKABLE void setLooperPlay(int row, int slot, bool on);
  Q_INVOKABLE void clearLooper(int row, int slot);
  Q_INVOKABLE bool looperCanUndo(int row, int slot) const;
  Q_INVOKABLE bool looperCanRedo(int row, int slot) const;
  Q_INVOKABLE void undoLooper(int row, int slot);
  Q_INVOKABLE void redoLooper(int row, int slot);
  Q_INVOKABLE void multiplyLooper(int row, int slot);
  Q_INVOKABLE bool looperCanMultiply(int row, int slot) const;

  // One peak per bucket across the closed loop, for drawing a waveform - see
  // LooperInstance::waveform() for what "closed" and "peak" mean here.
  Q_INVOKABLE QVariantList looperWaveform(int row, int slot, int buckets) const;
  Q_INVOKABLE qreal looperTrimStart(int row, int slot) const;
  Q_INVOKABLE qreal looperTrimEnd(int row, int slot) const;
  Q_INVOKABLE qreal looperFadeIn(int row, int slot) const;
  Q_INVOKABLE qreal looperFadeOut(int row, int slot) const;
  Q_INVOKABLE void setLooperTrim(int row, int slot, qreal start, qreal end);
  Q_INVOKABLE void setLooperFades(int row, int slot, qreal fadeIn, qreal fadeOut);
  // 0..1 through the closed loop, or -1 while there is nothing playing -
  // read on a timer to move a playhead over the waveform.
  Q_INVOKABLE qreal looperPosition(int row, int slot) const;
  Q_INVOKABLE bool looperRecording(int row, int slot) const;
  Q_INVOKABLE bool looperPlaying(int row, int slot) const;
  Q_INVOKABLE bool looperCountIn(int row, int slot) const;
  Q_INVOKABLE void setLooperCountIn(int row, int slot, bool on);
  Q_INVOKABLE int looperCountBeats(int row, int slot) const;
  Q_INVOKABLE bool looperHasAudio(int row, int slot) const;
  Q_INVOKABLE bool looperLoopClosed(int row, int slot) const;
  Q_INVOKABLE qreal looperBeats(int row, int slot) const;
  // Peak of the loop's own wet signal in the last block, 0..1ish - see
  // LooperInstance::loop_peak(). For a meter separate from the channel's own,
  // which is the loop plus whatever is passing through live.
  Q_INVOKABLE qreal looperLevel(int row, int slot) const;
  // Which overdub pass most recently touched each bucket - see
  // LooperInstance::layer_map().
  Q_INVOKABLE QVariantList looperLayers(int row, int slot, int buckets) const;
  // Length reading Sync: which other Looper this one follows, and the list
  // to pick one from. targetRow/Slot -1 means none chosen yet.
  Q_INVOKABLE void setLooperSyncTarget(int row, int slot, int targetRow,
                                       int targetSlot);
  Q_INVOKABLE int looperSyncTargetRow(int row, int slot) const;
  Q_INVOKABLE int looperSyncTargetSlot(int row, int slot) const;
  // Every other Looper in the session this one could sync to, as
  // [{row, slot, label}] - never itself.
  Q_INVOKABLE QVariantList looperSyncCandidates(int row, int slot) const;
  Q_INVOKABLE int timeNumerator() const {
    const int n = engine_.time_numerator();
    return n > 0 ? n : 1;
  }

  Q_INVOKABLE void toggleMasterDim();
  Q_INVOKABLE void toggleMasterMute();
  Q_INVOKABLE void toggleMasterMono();
  Q_INVOKABLE void toggleMidiClock();
  Q_INVOKABLE void toggleFollowMidiClock();
  bool masterDim() const { return engine_.graph().master_dim(); }
  bool masterMute() const { return engine_.graph().master_mute(); }
  bool masterMono() const { return engine_.graph().master_mono(); }
  bool midiClock() const { return engine_.midi_clock(); }
  bool followMidiClock() const { return engine_.follow_midi_clock(); }
  bool masterClip() const { return master_clip_; }
  Q_INVOKABLE void setTimeSignature(int num, int den);

  // Named sessions, apart from the automatic one: save a copy anywhere, or
  // replace the current mixer with a file's contents. Loading also becomes the
  // autosaved state, so a restart comes back to what was loaded.
  Q_INVOKABLE bool saveSessionAs(const QUrl& file);
  Q_INVOKABLE bool loadSessionFrom(const QUrl& file);
  // One strip, saved on its own and loadable into any session. Plugins that
  // are not installed here are named rather than passed over in silence.
  Q_INVOKABLE bool saveChannelTo(int row, const QUrl& file);
  Q_INVOKABLE bool loadChannelFrom(const QUrl& file);
  Q_INVOKABLE static QString recordingsUrl();

  // --- MIDI learn -----------------------------------------------------------
  // Arms a target; the next controller message that arrives binds to it.
  Q_INVOKABLE void learnGain(int row);
  Q_INVOKABLE void learnPan(int row);
  Q_INVOKABLE void learnMute(int row);
  Q_INVOKABLE void learnInsertParam(int row, int slot, int param,
                                    qreal min, qreal max);
  Q_INVOKABLE void cancelLearn();
  Q_INVOKABLE void clearMidiMaps(int row);
  Q_INVOKABLE bool insertParamMapped(int row, int slot, int param) const;
  bool learning() const { return pending_learn_.armed; }

  // Test hook: feeds one controller message through the same path a real one
  // takes after the engine queue.
  Q_INVOKABLE void injectControl(int cc, int channel, int value);

  // Test hook: the engine behind the model, so a test can reach a strip and
  // stand a plugin of its own in place of a scanned one.
  Engine& engineForTests() { return engine_; }

  // File player extras: only meaningful when the insert is one.
  Q_INVOKABLE bool insertIsFilePlayer(int row, int slot) const;
  Q_INVOKABLE bool insertIsStepSequencer(int row, int slot) const;
  Q_INVOKABLE bool insertIsScript(int row, int slot) const;
  Q_INVOKABLE bool insertIsKeyboardInstrument(int row, int slot) const;
  // A physical key going down or up, aimed at one insert rather than a whole
  // channel - see sendNote() above for the channel-wide equivalent. `note`
  // is already absolute; octave and velocity are the caller's own business.
  Q_INVOKABLE void pressComputerKey(int row, int slot, int note, int velocity);
  Q_INVOKABLE void releaseComputerKey(int row, int slot, int note);
  // The Lua a Script insert is running, what it said when it last failed, and
  // the way to hand it a new one. Compiling happens here, on the UI thread,
  // which is the whole point of how that plugin is built.
  Q_INVOKABLE QString insertScript(int row, int slot) const;
  Q_INVOKABLE QString insertScriptError(int row, int slot) const;
  Q_INVOKABLE bool setInsertScript(int row, int slot, const QString& source);
  // Which step the sequencer is on, or -1. Polled while its grid is open.
  Q_INVOKABLE int insertPlayhead(int row, int slot) const;
  // Current-pattern planes plus scalars. Empty if the insert is not a sequencer.
  Q_INVOKABLE QVariantMap insertSequencerSnapshot(int row, int slot) const;
  // Who this sequencer actually feeds: the next non-empty insert, nobody
  // else. Two instruments on one strip is two sequencers, not a merged
  // name list. `{name, pads:[{note,name},...]}`; empty when nothing sits
  // below, or the chip published no named keys.
  Q_INVOKABLE QVariantMap insertSequencerTarget(int row, int slot) const;
  Q_INVOKABLE void setSequencerCell(int row, int slot, int pattern, int lane,
                                    int step, int note, int velocity, bool on,
                                    qreal chance, bool accent, bool tie);
  Q_INVOKABLE void setSequencerTrig(int row, int slot, int pattern, int lane,
                                    int step, qreal micro, int ratchet,
                                    int cond, int condArg);
  // Euclid is not a field here. Mute/channel must not repaint Toussaint.
  Q_INVOKABLE void setSequencerLane(int row, int slot, int lane, int note,
                                    int length, int division, int direction,
                                    int channel, bool mute, qreal gate);
  Q_INVOKABLE void setSequencerLaneEuclid(int row, int slot, int lane,
                                          int pulses);
  Q_INVOKABLE void setSequencerHead(int row, int slot, int extra, int lane,
                                    int rate, int direction, int start,
                                    int length, int transpose, bool mute);
  Q_INVOKABLE bool setInsertFile(int row, int slot, const QUrl& file);
  Q_INVOKABLE QString insertFilePath(int row, int slot) const;

  // --- routing ------------------------------------------------------------
  // Ports a channel can be fed from, ready to show in a picker. `midi` picks
  // between MIDI sources and audio ones.
  Q_INVOKABLE QStringList sources(bool midi) const;
  Q_INVOKABLE QStringList sinks() const;

  Q_INVOKABLE void connectSource(int row, const QString& port, bool midi);

  // The MIDI matrix: any source into any channel, several at once.
  Q_INVOKABLE bool midiLinked(int row, const QString& port) const;
  Q_INVOKABLE void setMidiLink(int row, const QString& port, bool on);
  Q_INVOKABLE void connectMaster(const QString& port);

  // The short form of a port name, for a label that has to fit in a strip.
  Q_INVOKABLE static QString shortPortName(const QString& port);

  // --- session ------------------------------------------------------------
  // There is no save dialog: the session is written continuously and restored
  // on the next start, so closing the app and reopening it lands you where you
  // left off.
  void saveSession() const;
  void loadSession();
  void writeSession(const QString& path) const;
  bool readSession(const QString& path);
  static QString sessionPath();

  // Every insert's state blob, in the order writeSession emits them. Parks the
  // graph for the asking and no longer, so the rest of a save runs with the
  // mixer audible.
  QVector<QVector<QByteArray>> collectInsertStates() const;

  // False when another Nirbija already holds the session. That instance still
  // runs and still loads what is on disk, but never writes: two mixers taking
  // turns overwriting one file loses whichever was edited first.
  bool ownsSession() const { return session_fd_ >= 0; }

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
  void learnChanged();
  void recordingChanged();
  void errorOccurred(const QString& message);
  void dirtyChanged();
  void metersActiveChanged();
  // A key going down or up anywhere in the window, seen ahead of whichever
  // QML item happens to have focus - see eventFilter() below. A control
  // that wants the letters for itself (a text field, the Lua editor) still
  // gets every one of them untouched; this is only ever a second listener,
  // never a thief.
  void globalKeyEvent(int key, bool pressed, bool autoRepeat);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  struct ChannelUi {
    // Which graph slot this row drives. Removing a row leaves the slots around
    // it where they were, so the two indices drift apart and only this mapping
    // is safe to hand the engine.
    size_t slot = 0;
    // A bus is a strip fed by other strips rather than by a port, and lives in
    // the graph's own bus list, so the slot means a different thing.
    bool is_bus = false;
    int destination = -1;
    QString name;
    int width = 2;
    qreal gain = 1.0;
    qreal pan = 0.0;
    bool muted = false;
    bool soloed = false;
    bool armed = false;
    qreal peak[2] = {0.0, 0.0};
    // Meter ballistics, all in fader travel rather than in amplitude: the bar
    // jumps to a new peak and falls back at a fixed rate, and the held mark
    // sits at the loudest thing seen recently. A bar that simply followed the
    // sampled peak flickered, and left nothing on screen to read a transient
    // off.
    qreal position[2] = {0.0, 0.0};
    qreal hold[2] = {0.0, 0.0};
    int hold_age[2] = {0, 0};
    QString input_label;
    QString output_label;
    QString midi_label;
    QStringList inserts;
    // [{ bus: int, name: QString, level: qreal }], in slot order.
    QVariantList sends;
    QString accent;
  };

  ChannelStrip* stripFor(int row) const;
  void swapRows(int row, int target);
  PluginInstance* insertFor(int row, int slot) const;

  // Adds an insert and reports which slot took it, or -1. addInsert and
  // addInsertAt are the boolean faces of this for QML.
  int placeInsert(int row, int pluginIndex, int targetSlot);
  int busCount() const;
  void pollLevels();
  void handleControl(int cc, int channel, int value);
  void refreshRouting(int row);

  // Coalesces the writes: a fader drag would otherwise save on every frame.
  // `schedule_save` false marks the session modified without arming the
  // autosave - for changes that have to survive the session but are not worth
  // a write of their own, because collecting the plugin states parks the graph
  // and the master goes quiet for the two blocks that takes. The destructor
  // saves unconditionally, so nothing marked this way is lost.
  void markDirty(bool schedule_save = true);
  void claimSession();
  void post(EngineCommand::Kind kind, int row, float value);

  // Declared before the engine so it outlives it: strips hold plugin instances
  // that belong to the backends this model owns.
  std::unique_ptr<PluginListModel> plugins_;
  Engine engine_;
  std::vector<ChannelUi> channels_;

  QJsonObject writeChannel(const ChannelUi& channel, size_t row,
                           const QVector<QByteArray>& row_states) const;
  QString nextAccent() const;
  int restoreChannel(const QJsonObject& entry, QStringList* missing);
  void restoreChannelLinks(int row, const QJsonObject& entry);
  // Bindings are stored on the channel, not by graph slot: those numbers
  // are issued fresh every time the mixer starts, and a map that kept one
  // would land on the wrong strip — or on none.
  QJsonArray mapsJsonForRow(int row) const;
  void applyMapsJson(int row, const QJsonArray& maps);
  QTimer level_timer_;

  // What a controller message can drive. Bindings survive in the session.
  struct MidiMapping {
    int cc = -1;
    int midi_channel = -1;
    enum class Kind { Gain, Pan, Mute, Param } kind = Kind::Gain;
    int row = -1;
    int graph_slot = -1;
    bool is_bus = false;
    int slot = -1;
    uint32_t param = 0;
    double min = 0.0;
    double max = 1.0;
    // A button that only knows on/off. Learned from a 0 or 127 (or a note):
    // each press flips Rec/Play instead of following the 0 that a toggle
    // pad sends when it latches off — which used to punch Rec in and
    // immediately out.
    bool toggle = false;
  };
  std::vector<MidiMapping> midi_maps_;

  struct {
    bool armed = false;
    MidiMapping target;
  } pending_learn_;
  QTimer autosave_timer_;
  // Restoring fires the same setters the UI does; without this every one of
  // them would queue another save of what was just loaded.
  bool restoring_ = false;
  // Held for the life of the process; the kernel drops it if we die badly.
  int session_fd_ = -1;
  // Editor windows stay owned here so closing the mixer closes them too. The
  // slot tags each one, so removing a channel closes only its own editors.
  struct OpenEditor {
    size_t slot;
    // Channels and buses number their slots separately, so the slot alone does
    // not say which strip this belongs to. Without it, removing channel 2 shut
    // bus 2's editors and removing a bus shut none of its own — leaving a
    // window driving a plugin the graph had already reclaimed.
    bool is_bus = false;
    // Which plugin this window edits, so a second click finds the first window
    // instead of opening a twin.
    PluginInstance* insert;
    // When it opened: the second half of a double-click must not count as the
    // closing click.
    qint64 opened_ms = 0;
    std::unique_ptr<PluginWindow> window;
  };
  std::vector<OpenEditor> editors_;
  qreal master_peak_[2] = {0.0, 0.0};
  qreal master_position_[2] = {0.0, 0.0};
  qreal master_hold_[2] = {0.0, 0.0};
  int master_hold_age_[2] = {0, 0};
  bool meters_active_ = true;
  // Advances one meter by one poll: instant rise, steady fall, and a held mark
  // that waits before it starts to drop.
  static void advanceMeter(qreal peak, qreal& position, qreal& hold, int& age);
  qreal master_gain_ = 1.0;
  QString status_;
  bool playing_ui_ = false;
  bool metronome_ui_ = false;
  qreal tempo_ui_ = 120.0;
  bool seed_empty_session_ = true;
  bool dirty_flag_ = false;
  bool master_clip_ = false;
  QVector<QByteArray> undo_stack_;
  QVector<QByteArray> redo_stack_;
  void pushUndo();
  QByteArray snapshot() const;
  void restoreSnapshot(const QByteArray& blob);
};

}  // namespace nirbija
