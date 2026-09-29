// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
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
#include <unordered_map>
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
  Q_PROPERTY(bool masterLimiter READ masterLimiter NOTIFY masterGainChanged)
  // True while the limiter is actually holding something back.
  Q_PROPERTY(bool limiterWorking READ limiterWorking NOTIFY levelsChanged)
  // Periods the audio server reported missing since it started. Every one of
  // them is a dropout the listener heard.
  Q_PROPERTY(int xruns READ xruns NOTIFY levelsChanged)
  Q_PROPERTY(bool midiClock READ midiClock NOTIFY transportChanged)
  Q_PROPERTY(bool followMidiClock READ followMidiClock NOTIFY transportChanged)
  Q_PROPERTY(bool masterClip READ masterClip NOTIFY levelsChanged)
  // The row count as a property, so a binding in a persistent Loader (the
  // navigator's header, the matrix's width) follows strips being added and
  // removed instead of freezing on whatever rowCount() said the first time.
  Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
  // True from construction until the plugin scan has finished and the
  // autosaved session is back on the strips. The window opens before either,
  // and the top bar says so rather than sitting black until they are done.
  Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
  // Scenes (design/scenes.md). The list is [{name, hue, bars, fade, count,
  // lost}]: bars 0 is "until changed", `count` how many controls it holds and
  // `lost` how many of those point at a strip or plugin that is gone.
  Q_PROPERTY(QVariantList scenes READ scenes NOTIFY scenesChanged)
  Q_PROPERTY(int currentScene READ currentScene NOTIFY sceneStateChanged)
  Q_PROPERTY(int armedScene READ armedScene NOTIFY sceneStateChanged)
  // Which scene a touch lands in while recording, -1 when there is none.
  Q_PROPERTY(int recordScene READ recordScene NOTIFY sceneStateChanged)
  // Bars since the current scene started, from 0, plus how far into this bar
  // the transport is (0..1), so the ribbon can draw its playhead smoothly.
  Q_PROPERTY(int sceneBar READ sceneBar NOTIFY sceneStateChanged)
  Q_PROPERTY(qreal sceneBarPhase READ sceneBarPhase NOTIFY sceneStateChanged)
  Q_PROPERTY(bool sceneAuto READ sceneAuto WRITE setSceneAuto NOTIFY sceneStateChanged)
  Q_PROPERTY(bool sceneHold READ sceneHold WRITE setSceneHold NOTIFY sceneStateChanged)
  Q_PROPERTY(bool sceneRecording READ sceneRecording WRITE setSceneRecording NOTIFY
                 sceneStateChanged)

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
    // Whether anything is wired in, as a bool rather than a label compared
    // against a translated string in QML.
    InputConnectedRole,
    MidiConnectedRole,
    InsertsRole,
    // The same chain with the state a slot needs to draw itself:
    // [{name, filled, bypassed, postFader, missing, uid, ...live flags}].
    // Bypass used to be invisible until the menu was opened, which is a poor
    // place to keep "this is not being heard". `filled` is false for a hole
    // left by a removal; `missing` marks a plugin the session names but this
    // machine does not have. The list is cached per row and only announced
    // when something in it actually changed - see refreshInsertDetails().
    InsertDetailsRole,
    WidthRole,
    AccentRole,
    IsBusRole,
    DestinationRole,
    SendsRole,
    // Scenes: whether the strip follows them, whether its scene gate is on,
    // and which of its controls the scene in view holds (SceneMarksRole) or
    // the player took away from it until the next change (SceneHandsRole) -
    // bit 0 the gate, bit 1 the fader, bit 2 a sequencer pattern.
    FollowScenesRole,
    SceneOnRole,
    SceneMarksRole,
    SceneHandsRole,
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
  bool loading() const { return loading_; }
  // Blocks until the plugin scan started in the constructor (or by a rescan)
  // has finished, then restores the session if that is still pending. For
  // the headless tests, which build a model and reach for its plugins on the
  // next line; the window never needs it. Negative waits as long as it takes.
  Q_INVOKABLE bool waitForScan(int milliseconds = -1);
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

  // Clears the mixer back to nothing and saves that as the session. An undo
  // step, like everything else that throws strips away.
  Q_INVOKABLE void newSession();
  // True when there is something a "new session" would throw away.
  Q_INVOKABLE bool sessionHasContent() const {
    return !channels_.empty() || dirty_flag_;
  }
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
  // Drone: six strings and a swell. The snapshot is the editor's whole view
  // in one call - every parameter, which are mapped, and what the audio
  // thread is putting out - polled at the frame rate rather than forty
  // separate reads. Writes go by parameter id; the swell is performance and
  // does not schedule a save, everything else is the drone and does.
  Q_INVOKABLE bool insertIsDrone(int row, int slot) const;
  Q_INVOKABLE QVariantMap insertDroneSnapshot(int row, int slot) const;
  Q_INVOKABLE void setDroneParam(int row, int slot, int id, qreal value);
  Q_INVOKABLE QStringList dronePresetNames() const;
  Q_INVOKABLE void applyDronePreset(int row, int slot, int index);
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
  // LooperInstance::writing() and beats_to_boundary(): whether the head is
  // actually on the tape, and how many beats until a pending press lands.
  Q_INVOKABLE bool looperWriting(int row, int slot) const;
  Q_INVOKABLE qreal looperBeatsToBoundary(int row, int slot) const;
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
  Q_INVOKABLE void toggleMasterLimiter();
  Q_INVOKABLE void toggleMidiClock();
  Q_INVOKABLE void toggleFollowMidiClock();
  bool masterDim() const { return engine_.graph().master_dim(); }
  bool masterMute() const { return engine_.graph().master_mute(); }
  bool masterMono() const { return engine_.graph().master_mono(); }
  bool masterLimiter() const { return engine_.graph().master_limiter(); }
  bool limiterWorking() const { return limiter_working_; }
  int xruns() const { return xruns_; }
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
  // Next note-on from any controller becomes this pad's MIDI key. Not a
  // lasting binding: the pad then plays that note the ordinary way. A CC
  // is ignored so a knob sweep while wiring pads does not steal the learn.
  Q_INVOKABLE void learnSamplerPadNote(int row, int slot, int pad);
  // While the Sampler editor is open, every controller on the machine
  // lights that chip's pads — the strip's own MIDI routing does not have
  // to be right first. Off when the editor closes.
  Q_INVOKABLE void listenSamplerMidi(int row, int slot, bool on);
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
  // Test hook: an undo step at a point the UI would not take one, so a test
  // can undo a single insert removal.
  void pushUndoForTests() { pushUndo(); }

  // File player extras: only meaningful when the insert is one.
  Q_INVOKABLE bool insertIsFilePlayer(int row, int slot) const;
  Q_INVOKABLE bool insertIsStepSequencer(int row, int slot) const;
  // Rec is down on a Step Sequencer: the strip's slot shows it red, the
  // same as a looper writing or a sampler taking.
  Q_INVOKABLE bool sequencerRecording(int row, int slot) const;
  Q_INVOKABLE bool insertIsScript(int row, int slot) const;
  Q_INVOKABLE bool insertIsKeyboardInstrument(int row, int slot) const;
  Q_INVOKABLE bool insertIsSampler(int row, int slot) const;
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
  // A number that moves whenever anything in that snapshot other than the
  // heads would - a cell, a lane, a macro, the pattern in view. The grid
  // polls this and only asks for the whole snapshot when it changed; before,
  // it rebuilt five thousand values and every cell binding twenty times a
  // second to draw a playhead. The sequencer keeps no such counter of its
  // own, so this hashes the planes on this side and remembers the last hash
  // per insert. 0 when the insert is not a sequencer.
  Q_INVOKABLE int sequencerVersion(int row, int slot) const;
  // Just the heads, the one part of the snapshot that moves every tick.
  Q_INVOKABLE QVariantList insertSequencerHeads(int row, int slot) const;
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

  // Sampler: 16 pads, Rec from the strip, a file onto the focused pad.
  // Snapshot is the editor's whole view; the grid is not parameters().
  Q_INVOKABLE QVariantMap insertSamplerSnapshot(int row, int slot) const;
  Q_INVOKABLE void setSamplerRecord(int row, int slot, bool on);
  Q_INVOKABLE bool samplerRecording(int row, int slot) const;
  Q_INVOKABLE bool samplerHasAudio(int row, int slot) const;
  Q_INVOKABLE void setSamplerFocus(int row, int slot, int pad);
  Q_INVOKABLE void clearSamplerPad(int row, int slot, int pad);
  Q_INVOKABLE void setSamplerPad(int row, int slot, int pad, int note,
                                 bool oneShot, qreal volume, qreal pan,
                                 qreal pitch);
  // Give a pad a MIDI note; a pad that already had it takes this pad's old
  // one, so sixteen pads never share a key.
  Q_INVOKABLE void assignSamplerPadNote(int row, int slot, int pad, int note);
  Q_INVOKABLE void setSamplerPadName(int row, int slot, int pad,
                                     const QString& name);
  Q_INVOKABLE void setSamplerTrim(int row, int slot, int pad, qreal start,
                                  qreal end);
  Q_INVOKABLE void setSamplerFades(int row, int slot, int pad, qreal fadeIn,
                                   qreal fadeOut);
  Q_INVOKABLE bool loadSamplerPad(int row, int slot, int pad, const QUrl& file);
  Q_INVOKABLE void previewSamplerPad(int row, int slot, int pad, int velocity);
  Q_INVOKABLE void releaseSamplerPad(int row, int slot, int pad);
  Q_INVOKABLE QVariantList samplerWaveform(int row, int slot, int pad,
                                           int buckets) const;
  Q_INVOKABLE bool undoSamplerPad(int row, int slot, int pad);
  Q_INVOKABLE void setSamplerCountIn(int row, int slot, bool on);
  // The pads on their own, without the rest of the strip — a kit you can
  // save and load independent of what sits in front of the sampler.
  Q_INVOKABLE bool saveSamplerPackTo(int row, int slot, const QUrl& file);
  Q_INVOKABLE bool loadSamplerPackFrom(int row, int slot, const QUrl& file);

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

  // Every insert's state blob, in the order writeSession emits them. Read
  // with the mixer playing; the graph is parked only if a plugin says its
  // state cannot be read that way right now, and only for the asking - and
  // only when the caller allows it. With `allow_park` false such a plugin's
  // entry is marked skipped instead: the autosave and the undo snapshot
  // would rather leave one blob out than put a hole in the master.
  struct InsertState {
    QByteArray blob;
    bool skipped = false;
  };
  QVector<QVector<InsertState>> collectInsertStates(bool allow_park = true) const;
  bool anyInsertNeedsQuietSave() const;

  // False when another Nirbija already holds the session. That instance still
  // runs and still loads what is on disk, but never writes: two mixers taking
  // turns overwriting one file loses whichever was edited first.
  bool ownsSession() const { return session_fd_ >= 0; }

  // --- scenes ---------------------------------------------------------------
  QVariantList scenes() const;
  int currentScene() const { return scene_current_; }
  int armedScene() const { return scene_armed_; }
  int recordScene() const;
  int sceneBar() const { return scene_bar_; }
  qreal sceneBarPhase() const { return scene_bar_phase_; }
  bool sceneAuto() const { return engine_.scenes().auto_advance(); }
  void setSceneAuto(bool on);
  bool sceneHold() const { return engine_.scenes().hold(); }
  void setSceneHold(bool on);
  bool sceneRecording() const { return scene_recording_; }
  void setSceneRecording(bool on);
  // A new, empty scene at the end of the list. Returns its index.
  Q_INVOKABLE int addScene();
  Q_INVOKABLE void removeScene(int scene);
  Q_INVOKABLE void renameScene(int scene, const QString& name);
  // `bars` 0 plays until something else is chosen.
  Q_INVOKABLE void setSceneBars(int scene, int bars);
  Q_INVOKABLE void setSceneFade(int scene, int bars);
  Q_INVOKABLE void moveScene(int scene, int direction);
  // Forgets everything the scene holds, keeping its name and length.
  Q_INVOKABLE void clearScene(int scene);
  // Arms the scene for the next bar line - at once with the transport
  // stopped. Arming the one already armed takes it back.
  Q_INVOKABLE void armScene(int scene);
  // The strip's own on/off, the one a scene fades.
  Q_INVOKABLE void toggleSceneOn(int row);
  Q_INVOKABLE void setFollowScenes(int row, bool on);

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
  void countChanged();
  void loadingChanged();
  void scenesChanged();
  void sceneStateChanged();
  // The plugin list is complete (again). The session load waits for it.
  void scanFinished();
  // One beat of the 30 Hz poll, for editors that redraw something live - a
  // playhead, a meter, a string. Subscribing here instead of running a Timer
  // each means one wake-up per tick however many editors are open, and none
  // when the window is hidden.
  void tick();
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
    bool input_connected = false;
    bool midi_connected = false;
    QStringList inserts;
    // What InsertDetailsRole answers, built by refreshInsertDetails() and
    // handed out as is: data() runs for every delegate on every dataChanged,
    // and building a list of maps with a dozen dynamic_casts per slot there,
    // thirty times a second, was most of the mixer's idle CPU.
    QVariantList details;
    // A compact fingerprint of `details` - one word per slot - compared each
    // poll so the role is only announced when a flag actually flipped.
    QVector<quint32> details_signature;
    // Plugins the session named that are not installed here. Kept, blob and
    // all, so the next save writes them back and the strip can show the gap;
    // they sit after the live chain in the details list since the engine
    // has no slot to hold a plugin it could not make.
    struct MissingInsert {
      QString format;
      QString uid;
      QString state;  // base64, verbatim from the file
      bool bypassed = false;
      bool post_fader = false;
    };
    QVector<MissingInsert> missing;
    // [{ bus: int, name: QString, level: qreal }], in slot order.
    QVariantList sends;
    QString accent;
    // Who this strip is to a scene, across saves and reorders: 64 bits of
    // random hex, made when the strip is and kept in the session.
    QString uid;
    bool follow_scenes = true;
    bool scene_on = true;
    // SceneHandsRole's bits, cleared whenever a scene starts.
    int scene_hands = 0;
    // The same for the plugins: the chain tags of the inserts whose pattern
    // or knob the player took from the scene.
    std::vector<uint32_t> scene_hand_tags;
    // The conductor's count of fader moves, as last seen; a change means the
    // scene moved this fader and the model follows it.
    uint32_t level_writes_seen = 0;
  };

  // A scene as the UI keeps it. Targets name strips by uid and sequencers by
  // their chain tag, which is only good for this run: the session writes the
  // insert's position instead and reading turns it back into a tag.
  struct SceneUi {
    struct Target {
      QString strip;
      SceneTarget::What what = SceneTarget::What::Gate;
      uint32_t insert_tag = 0;
      float value = 0.0f;
      uint32_t param = 0;    // Param only
      bool stepped = false;  // Param only: jumps on the line
    };
    uint32_t id = 0;  // stable for the run; what the conductor follows it by
    QString name;
    qreal hue = 0.58;
    int bars = 8;
    int fade = 1;
    std::vector<Target> targets;
  };

  ChannelStrip* stripFor(int row) const;
  void swapRows(int row, int target);
  PluginInstance* insertFor(int row, int slot) const;

  // Adds an insert and reports which slot took it, or -1. addInsert and
  // addInsertAt are the boolean faces of this for QML. A state blob given
  // here is loaded before the plugin is published to the audio thread, which
  // is the one moment load_state() is safe without parking anything; a
  // sampler also resolves its sample paths against `sample_dir` then.
  int placeInsert(int row, int pluginIndex, int targetSlot,
                  const std::vector<uint8_t>* state = nullptr,
                  const QString& sample_dir = {});
  int busCount() const;
  void pollLevels();
  // Rebuilds one row's InsertDetailsRole and announces it if it changed.
  // The fingerprint is what the poll compares each tick; the list of maps
  // is only built when the fingerprint moved.
  void refreshInsertDetails(int row);
  QVector<quint32> insertDetailsSignature(int row) const;
  QVariantList buildInsertDetails(int row) const;
  // The autosave timer's slot: saves unless a plugin would need the graph
  // parked for it, in which case it waits and asks again a second later.
  void autosave();
  // The whole session as JSON, states read under the given park policy.
  QJsonObject buildSession(bool allow_park) const;
  static bool writeJson(const QString& path, const QJsonObject& root);
  // Everything a session carries besides its channels: tempo, clock, master.
  void applySessionGlobals(const QJsonObject& root);
  // Throws every strip away without writing anything to disk.
  void clearMixer();
  // Brings the mixer to what `root` describes by changing only what differs:
  // strips and inserts that are already there stay the same objects, nothing
  // is parked, and a plugin that has to be made is loaded with its state
  // before the audio thread ever sees it. How undo and redo restore.
  void applySnapshot(const QJsonObject& root);
  // Starts the plugin scan on its worker and the session load behind it.
  void beginStartup();
  void finishStartup();
  void handleControl(int cc, int channel, int value);
  void refreshRouting(int row);

  // Scenes, in mixer_scenes.cpp.
  int rowForUid(const QString& uid) const;
  static QString makeUid();
  // The row's uid, or a fresh one when it is empty or another row has it.
  QString claimUid(const QString& wanted, int row) const;
  // Rebuilds the conductor's table from scenes_ and publishes it.
  void publishScenes();
  void pollSceneParams();
  // Once per poll: what the conductor did, the faders it moved, and a table
  // rebuilt if strips came, went or stopped following since the last one.
  void pollScenes();
  // Writes a touch into the scene being recorded, or takes the control away
  // from the scene that is playing. `row` must follow scenes; `insert_tag`
  // is the sequencer's for a pattern.
  void sceneTouched(int row, SceneTarget::What what, float value,
                    uint32_t insert_tag = 0, uint32_t param = 0, bool stepped = false);
  // A plugin knob moved by the player: into the scene being recorded, or out
  // of the playing scene's hands. Only for plugins the conductor may set
  // from the audio thread (see sceneParamAllowed).
  void sceneParamTouched(int row, int slot, uint32_t id, float value);
  static bool sceneParamAllowed(const PluginInstance* insert);
  static bool sceneParamStepped(const PluginInstance* insert, uint32_t id);
  int sceneMarksFor(int row) const;
  // Whether the scene in view holds this insert's pattern or a knob of it,
  // and whether the player has taken it back: the slot's scene dot.
  bool sceneHoldsInsert(int row, int slot) const;
  bool sceneHandOnInsert(int row, int slot) const;
  void announceSceneMarks();
  void scenesEdited();
  QJsonObject scenesJson() const;
  // `keep_ids`: an undo puts back the very scenes it took, so the one
  // playing goes on playing; a session opened from a file numbers its own.
  void applyScenesJson(const QJsonObject& json, bool keep_ids = false);
  quint64 sceneTableSignature() const;

  // Coalesces the writes: a fader drag would otherwise save on every frame.
  // `schedule_save` false marks the session modified without arming the
  // autosave - for changes that have to survive the session but are not worth
  // a write of their own: a pad pressed twenty times in a bar is twenty
  // sessions built and written. The destructor saves unconditionally, so
  // nothing marked this way is lost.
  void markDirty(bool schedule_save = true);
  void claimSession();
  void post(EngineCommand::Kind kind, int row, float value);

  // Declared before the engine so it outlives it: strips hold plugin instances
  // that belong to the backends this model owns.
  std::unique_ptr<PluginListModel> plugins_;
  Engine engine_;
  std::vector<ChannelUi> channels_;

  QJsonObject writeChannel(const ChannelUi& channel, size_t row,
                           const QVector<InsertState>& row_states) const;
  QString nextAccent() const;
  int restoreChannel(const QJsonObject& entry, QStringList* missing,
                     const QString& sample_dir = {});
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
    enum class Kind { Gain, Pan, Mute, Param, SamplerPadNote } kind =
        Kind::Gain;
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
  int sampler_listen_row_ = -1;
  int sampler_listen_slot_ = -1;
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
  bool loading_ = true;
  bool session_loaded_ = false;
  // Which scan a pending "load the session when the list is ready" belongs
  // to, so a rescan from the menu does not reload the session again.
  int startup_scan_ = 0;
  // sequencerVersion()'s memory: the last hash seen per insert and the
  // counter it bumped. Keyed by the instance, which outlives the grid.
  struct SequencerVersion {
    quint64 hash = 0;
    int version = 0;
  };
  mutable std::unordered_map<const PluginInstance*, SequencerVersion>
      sequencer_versions_;
  bool master_clip_ = false;
  bool limiter_working_ = false;
  int limiter_hold_ = 0;
  int xruns_ = 0;
  std::vector<SceneUi> scenes_;
  uint32_t next_scene_id_ = 1;
  // The published table's parameters that only this thread may read (CLAP,
  // VST3): pollScenes() writes where each one is into SceneTable::now.
  struct PolledParam {
    size_t index = 0;  // into the table's targets
    QString strip;
    uint32_t insert_tag = 0;
    uint32_t param = 0;
  };
  std::shared_ptr<const SceneTable> scene_table_;
  std::vector<PolledParam> scene_polled_;
  bool scene_recording_ = false;
  int scene_current_ = -1;
  int scene_armed_ = -1;
  int scene_bar_ = -1;
  qreal scene_bar_phase_ = 0.0;
  uint32_t scene_changes_seen_ = 0;
  quint64 scene_table_signature_ = 0;
  QVector<QByteArray> undo_stack_;
  QVector<QByteArray> redo_stack_;
  void pushUndo();
  QByteArray snapshot() const;
  void restoreSnapshot(const QByteArray& blob);
};

}  // namespace nirbija
