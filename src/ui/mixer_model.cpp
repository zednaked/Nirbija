// before Qt: emit() is a method, not the Qt macro
#include "core/step_sequencer.h"

#include "mixer_model.h"

#include "core/script_plugin.h"

#include "core/file_player.h"
#include "core/looper.h"
#include "core/fx_pad.h"
#include "core/keyboard_instrument.h"

#include <unistd.h>

#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QUrl>
#include <QStandardPaths>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace nirbija {
namespace {

// Strip accents. AUM colours strips so a busy session stays readable at a
// glance, which only works while two strips do not wear the same colour.
const QStringList kAccents = {
    QStringLiteral("#4a9eda"), QStringLiteral("#5cb85c"), QStringLiteral("#d9a441"),
    QStringLiteral("#c85f8f"), QStringLiteral("#7b68c4"), QStringLiteral("#4fb3a8"),
};

constexpr qreal kMinDb = -70.0;
constexpr qreal kMaxDb = 6.0;

}  // namespace

MixerModel::MixerModel(QObject* parent) : QAbstractListModel(parent) {
  plugins_ = std::make_unique<PluginListModel>(this);

  if (engine_.start("nirbija")) {
    status_ = tr("%1 Hz · %2 frames")
                  .arg(engine_.sample_rate(), 0, 'f', 0)
                  .arg(engine_.block_frames());
  } else {
    status_ = tr("no audio server — start PipeWire or JACK and restart");
  }

  // A mixer that just opened should already be audible, the way AUM comes up
  // connected to the device. A restored session overrides this below.
  engine_.connect_master_to_default_output();

  // 30 Hz is enough for a meter to look continuous and cheap enough that the
  // poll never competes with the audio thread.
  level_timer_.setInterval(33);
  connect(&level_timer_, &QTimer::timeout, this, &MixerModel::pollLevels);
  level_timer_.start();

  // One save a second at most, however hard a fader is being dragged.
  autosave_timer_.setSingleShot(true);
  autosave_timer_.setInterval(1000);
  connect(&autosave_timer_, &QTimer::timeout, this, &MixerModel::saveSession);

  claimSession();
  const bool had_file = QFile::exists(sessionPath());
  loadSession();
  seed_empty_session_ = !had_file;
  playing_ui_ = engine_.playing();
  metronome_ui_ = engine_.metronome();

  if (!ownsSession()) {
    status_ += tr(" · session read-only (another Nirbija has it)");
    emit statusChanged();
  }

  // A live-played source, unlike everything else here, cannot afford to go
  // quiet just because a fader took the keyboard focus away from its editor -
  // installed on the application rather than any one item so it keeps
  // seeing keys with that editor closed, same as a looper keeps looping
  // with its own editor closed.
  qApp->installEventFilter(this);
}

MixerModel::~MixerModel() {
  qApp->removeEventFilter(this);

  // The debounced save may still be pending, and closing the window is exactly
  // when the session matters most.
  saveSession();

  // Released explicitly rather than left to process exit. flock is held by the
  // open file description, so a second model built in the same process — a
  // test, or a reopened window — could not take a lock this one had let go of
  // in every sense but the descriptor, and would run read-only forever.
  if (session_fd_ >= 0) {
    ::close(session_fd_);
    session_fd_ = -1;
  }
}

bool MixerModel::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
    // A text field, a rename box, the Lua editor - anything actually taking
    // dictation - keeps every key exactly as it always has. This only
    // steps in when nothing is claiming the letter for itself, which a
    // class name is a cheap enough way to tell without a private header.
    QObject* focus = qApp->focusObject();
    const bool text_input = focus != nullptr &&
        (QByteArray(focus->metaObject()->className()).contains("TextInput") ||
         QByteArray(focus->metaObject()->className()).contains("TextEdit"));
    if (!text_input) {
      auto* key_event = static_cast<QKeyEvent*>(event);
      emit globalKeyEvent(key_event->key(), event->type() == QEvent::KeyPress,
                          key_event->isAutoRepeat());
    }
  }
  return QAbstractListModel::eventFilter(watched, event);
}

int MixerModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;
  return static_cast<int>(channels_.size());
}

QVariant MixerModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= static_cast<int>(channels_.size())) return {};
  const ChannelUi& channel = channels_[index.row()];
  switch (role) {
    case NameRole: return channel.name;
    case GainRole: return channel.gain;
    case PanRole: return channel.pan;
    case MutedRole: return channel.muted;
    case SoloedRole: return channel.soloed;
    case ArmedRole: return channel.armed;
    case PeakLeftRole: return channel.peak[0];
    case PeakRightRole: return channel.peak[1];
    case PositionLeftRole: return channel.position[0];
    case PositionRightRole: return channel.position[1];
    case HoldLeftRole: return channel.hold[0];
    case HoldRightRole: return channel.hold[1];
    case InputLabelRole: return channel.input_label;
    case OutputLabelRole: return channel.output_label;
    case MidiLabelRole: return channel.midi_label;
    case InsertsRole: return channel.inserts;
    case InsertDetailsRole: {
      QVariantList details;
      details.reserve(channel.inserts.size());
      for (int slot = 0; slot < channel.inserts.size(); ++slot) {
        QVariantMap entry;
        entry.insert(QStringLiteral("name"), channel.inserts.at(slot));
        entry.insert(QStringLiteral("bypassed"),
                     insertBypassed(index.row(), slot));
        entry.insert(QStringLiteral("postFader"),
                     insertPostFader(index.row(), slot));
        // Cheap even when it is not a Looper: insertIsLooper() is one
        // dynamic_cast, and the three state reads below short-circuit to
        // false on a null cast. Read once a poll so a strip can show which
        // of its inserts are armed or playing without opening each one.
        if (insertIsLooper(index.row(), slot)) {
          entry.insert(QStringLiteral("looperRecording"),
                       looperRecording(index.row(), slot));
          entry.insert(QStringLiteral("looperPlaying"),
                       looperPlaying(index.row(), slot));
          entry.insert(QStringLiteral("looperHasAudio"),
                       looperHasAudio(index.row(), slot));
        }
        details.append(entry);
      }
      return details;
    }
    case WidthRole: return channel.width;
    case AccentRole: return channel.accent;
    case IsBusRole: return channel.is_bus;
    case DestinationRole: return channel.destination;
    case SendsRole: return channel.sends;
    default: return {};
  }
}

QHash<int, QByteArray> MixerModel::roleNames() const {
  return {
      {NameRole, "name"},           {GainRole, "gain"},
      {PanRole, "pan"},             {MutedRole, "muted"},
      {SoloedRole, "soloed"},       {ArmedRole, "armed"},
      {PeakLeftRole, "peakLeft"},   {PeakRightRole, "peakRight"},
      {PositionLeftRole, "positionLeft"},
      {PositionRightRole, "positionRight"},
      {HoldLeftRole, "holdLeft"},   {HoldRightRole, "holdRight"},
      {InputLabelRole, "inputLabel"}, {OutputLabelRole, "outputLabel"},
      {MidiLabelRole, "midiLabel"},
      {InsertsRole, "inserts"},
      {InsertDetailsRole, "insertDetails"},
      {WidthRole, "channelWidth"},
      {AccentRole, "accent"},   {IsBusRole, "isBus"},
      {DestinationRole, "destination"}, {SendsRole, "sends"},
  };
}

// The accent no strip is wearing, or the least worn once the palette runs out.
//
// It used to be kAccents[slot % size], and slots are handed out separately to
// channels and to buses - so a channel and a bus could pick the same colour
// while half the palette went unused, which is what happened the first time a
// strip was loaded into a full session. Counting what is on screen cannot do
// that.
QString MixerModel::nextAccent() const {
  std::vector<int> used(static_cast<size_t>(kAccents.size()), 0);
  for (const ChannelUi& channel : channels_) {
    const qsizetype index = kAccents.indexOf(channel.accent);
    if (index >= 0) ++used[static_cast<size_t>(index)];
  }
  const auto least = std::min_element(used.begin(), used.end());
  return kAccents[static_cast<qsizetype>(std::distance(used.begin(), least))];
}

void MixerModel::addChannel(const QString& name, int channels) {
  // Mono or stereo, whatever the caller or the session file said.
  channels = std::clamp(channels, 1, kMaxStripChannels);
  pushUndo();
  // Named after the graph slot rather than the row count. Slots are never
  // reused, so removing a channel and adding another cannot produce two
  // channels with the same name — and the name then matches the JACK ports.
  const size_t index = engine_.add_channel("channel", channels);
  if (index == kMaxChannels) return;

  const QString label =
      name.isEmpty() ? tr("Channel %1").arg(index + 1) : name;
  engine_.graph().channel(index).set_name(label.toStdString());

  beginInsertRows({}, static_cast<int>(channels_.size()),
                  static_cast<int>(channels_.size()));
  ChannelUi channel;
  channel.slot = index;
  channel.name = label;
  channel.width = channels;
  channel.input_label = tr("no input");
  channel.midi_label = tr("no MIDI");
  channel.output_label = tr("Master");
  channel.accent = nextAccent();
  channels_.push_back(std::move(channel));
  endInsertRows();
  markDirty();
}

void MixerModel::removeChannel(int row) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  if (!restoring_) pushUndo();

  // Editors belonging to this channel go with it: one left open would be
  // editing a plugin that is no longer in the signal path.
  const size_t slot = channels_[row].slot;
  const bool bus = channels_[row].is_bus;
  std::erase_if(editors_, [slot, bus](const OpenEditor& editor) {
    return editor.slot == slot && editor.is_bus == bus;
  });

  std::erase_if(midi_maps_, [slot, bus](const MidiMapping& map) {
    return map.graph_slot == static_cast<int>(slot) && map.is_bus == bus;
  });

  if (bus)
    engine_.remove_bus(slot);
  else
    engine_.remove_channel(slot);

  beginRemoveRows({}, row, row);
  channels_.erase(channels_.begin() + row);
  endRemoveRows();
  markDirty();
}

void MixerModel::renameChannel(int row, const QString& name) {
  if (row < 0 || row >= static_cast<int>(channels_.size()) || name.isEmpty()) return;
  channels_[row].name = name;
  if (ChannelStrip* strip = stripFor(row)) strip->set_name(name.toStdString());

  // A send row shows the name of the bus it feeds, so those have to follow.
  for (int other = 0; other < static_cast<int>(channels_.size()); ++other) {
    QVariantList& sends = channels_[other].sends;
    bool touched = false;
    for (int slot = 0; slot < sends.size(); ++slot) {
      QVariantMap send = sends[slot].toMap();
      if (!channels_[row].is_bus) break;
      if (send.value(QStringLiteral("bus")).toInt() !=
          static_cast<int>(channels_[row].slot))
        continue;
      send[QStringLiteral("name")] = name;
      sends[slot] = send;
      touched = true;
    }
    if (channels_[other].is_bus &&
        channels_[other].destination == static_cast<int>(channels_[row].slot))
      channels_[other].output_label = name;
    if (touched) {
      const QModelIndex other_idx = index(other);
      emit dataChanged(other_idx, other_idx, {SendsRole, OutputLabelRole});
    }
  }

  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {NameRole});
  markDirty();
}

// A row is either a channel or a bus, and they live in different lists in the
// graph. Everything that acts on a strip goes through here.
ChannelStrip* MixerModel::stripFor(int row) const {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return nullptr;
  const ChannelUi& channel = channels_[row];
  AudioGraph& graph = const_cast<Engine&>(engine_).graph();

  if (channel.is_bus) {
    if (!graph.bus_alive(channel.slot)) return nullptr;
    return &graph.bus(channel.slot);
  }
  if (!graph.channel_alive(channel.slot)) return nullptr;
  return &graph.channel(channel.slot);
}

void MixerModel::addBus(const QString& name) {
  const size_t index = engine_.add_bus("bus");
  if (index == kMaxBuses) return;

  const QString label = name.isEmpty() ? tr("Bus %1").arg(index + 1) : name;
  engine_.graph().bus(index).set_name(label.toStdString());

  beginInsertRows({}, static_cast<int>(channels_.size()),
                  static_cast<int>(channels_.size()));
  ChannelUi bus;
  bus.slot = index;
  bus.is_bus = true;
  bus.name = label;
  bus.width = 2;
  bus.input_label = tr("bus input");
  bus.midi_label = tr("no MIDI");
  bus.output_label = tr("Master");
  bus.accent = nextAccent();
  channels_.push_back(std::move(bus));
  endInsertRows();
  markDirty();
}

int MixerModel::sendRowToNewBus(int row) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return -1;

  addBus(QString());
  const int bus_row = rowCount() - 1;
  if (bus_row < 0 || !channels_[bus_row].is_bus) return -1;

  setDestination(row, static_cast<int>(channels_[bus_row].slot));
  return bus_row;
}

int MixerModel::busCount() const {
  int count = 0;
  for (const ChannelUi& channel : channels_)
    if (channel.is_bus) ++count;
  return count;
}

QVariantList MixerModel::destinationsFor(int row) const {
  QVariantList out;
  if (row < 0 || row >= static_cast<int>(channels_.size())) return out;

  QVariantMap master;
  master[QStringLiteral("label")] = tr("Master");
  master[QStringLiteral("destination")] = -1;
  out.append(master);

  const ChannelUi& source = channels_[row];
  for (const ChannelUi& candidate : channels_) {
    if (&candidate == &source) continue;

    // The render order decides what is reachable: channels render in slot
    // order and buses after all of them. Feeding anything already rendered
    // would be a loop, so it is simply not offered.
    QVariantMap entry;
    if (candidate.is_bus) {
      if (source.is_bus && candidate.slot <= source.slot) continue;
      entry[QStringLiteral("destination")] = static_cast<int>(candidate.slot);
    } else {
      if (source.is_bus) continue;  // a bus cannot feed a channel
      if (candidate.slot <= source.slot) continue;
      entry[QStringLiteral("destination")] =
          channel_destination(candidate.slot);
    }
    entry[QStringLiteral("label")] = candidate.name;
    out.append(entry);
  }
  return out;
}

void MixerModel::setSend(int row, int slot, int bus, qreal level) {
  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr || slot < 0 || slot >= static_cast<int>(kMaxSends)) return;

  strip->set_send(static_cast<size_t>(slot), bus, static_cast<float>(level));

  QString name = tr("Master");
  for (const ChannelUi& candidate : channels_)
    if (candidate.is_bus && static_cast<int>(candidate.slot) == bus)
      name = candidate.name;

  QVariantList& sends = channels_[row].sends;
  while (sends.size() <= slot) sends.append(QVariantMap{});

  QVariantMap entry;
  entry[QStringLiteral("bus")] = bus;
  entry[QStringLiteral("name")] = name;
  entry[QStringLiteral("level")] = level;
  sends[slot] = entry;

  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {SendsRole});
  markDirty();
}

void MixerModel::removeSend(int row, int slot) {
  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr || slot < 0) return;
  if (slot >= channels_[row].sends.size()) return;

  strip->set_send(static_cast<size_t>(slot), -1, 0.0f);

  // The list is compacted so the rows below move up, and every send is
  // rewritten to match: a send's slot is its index, and a hole would silently
  // shift the ones after it.
  channels_[row].sends.removeAt(slot);
  for (int i = 0; i < channels_[row].sends.size(); ++i) {
    const QVariantMap entry = channels_[row].sends[i].toMap();
    strip->set_send(static_cast<size_t>(i),
                    entry.value(QStringLiteral("bus")).toInt(),
                    static_cast<float>(entry.value(QStringLiteral("level")).toReal()));
  }
  for (size_t i = channels_[row].sends.size(); i < kMaxSends; ++i)
    strip->set_send(i, -1, 0.0f);

  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {SendsRole});
  markDirty();
}

void MixerModel::setDestination(int row, int destination) {
  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr) return;

  strip->set_destination(destination);
  channels_[row].destination = destination;

  QString label = tr("Master");
  for (const ChannelUi& candidate : channels_) {
    const int id = candidate.is_bus
                       ? static_cast<int>(candidate.slot)
                       : channel_destination(candidate.slot);
    if (id == destination) label = candidate.name;
  }
  channels_[row].output_label = label;

  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {DestinationRole, OutputLabelRole});
  markDirty();
}

void MixerModel::post(EngineCommand::Kind kind, int row, float value) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  EngineCommand command;
  command.kind = kind;
  command.channel = channels_[row].slot;
  command.bus = channels_[row].is_bus;
  command.value = value;
  if (!engine_.post(command))
    emit errorOccurred(tr("Audio thread is not keeping up"));
}

void MixerModel::setGain(int row, qreal gain) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  channels_[row].gain = gain;
  post(EngineCommand::Kind::SetGain, row, static_cast<float>(gain));
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {GainRole});
  markDirty();
}

void MixerModel::setPan(int row, qreal pan) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  channels_[row].pan = pan;
  post(EngineCommand::Kind::SetPan, row, static_cast<float>(pan));
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {PanRole});
  markDirty();
}

void MixerModel::toggleMute(int row) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  channels_[row].muted = !channels_[row].muted;
  post(EngineCommand::Kind::SetMute, row, channels_[row].muted ? 1.0f : 0.0f);
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {MutedRole});
  markDirty();
}

void MixerModel::toggleSolo(int row) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  channels_[row].soloed = !channels_[row].soloed;
  post(EngineCommand::Kind::SetSolo, row, channels_[row].soloed ? 1.0f : 0.0f);
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {SoloedRole});
  markDirty();
}

void MixerModel::toggleArm(int row) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  channels_[row].armed = !channels_[row].armed;

  // Arming mid-take does nothing until the next one: tracks are decided when
  // recording starts, and adding a file part way through would leave a take
  // whose files no longer line up.
  if (ChannelStrip* strip = stripFor(row)) strip->set_armed(channels_[row].armed);
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {ArmedRole});
}

void MixerModel::setMasterGain(qreal gain) {
  master_gain_ = gain;
  EngineCommand command;
  command.kind = EngineCommand::Kind::SetMasterGain;
  command.value = static_cast<float>(gain);
  engine_.post(command);
  emit masterGainChanged();
  markDirty();
}

void MixerModel::togglePlay() {
  playing_ui_ = !playing_ui_;
  EngineCommand command;
  command.kind = EngineCommand::Kind::SetPlaying;
  command.value = playing_ui_ ? 1.0f : 0.0f;
  if (!engine_.post(command))
    emit errorOccurred(tr("Audio thread is not keeping up"));
  emit transportChanged();
}

QString MixerModel::recordingsPath() {
  const QString music =
      QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
  return (music.isEmpty() ? QDir::homePath() : music) +
         QStringLiteral("/Nirbija");
}

QString MixerModel::toggleRecord() {
  if (engine_.recording()) {
    engine_.stop_recording();
    emit recordingChanged();
    return {};
  }

  const QString take =
      QString::fromStdString(engine_.start_recording(recordingsPath().toStdString()));
  if (take.isEmpty()) {
    qWarning("recorder: could not start a take");
    emit errorOccurred(tr("Could not start recording"));
  }
  emit recordingChanged();
  return take;
}

// Elapsed time of the take, and a warning if the disk could not keep up: a
// dropout is not something to discover after the session.
QString MixerModel::recordingLabel() const {
  if (!engine_.recording()) return {};

  const int seconds = static_cast<int>(engine_.recorded_seconds());
  const QString elapsed = QStringLiteral("%1:%2")
                              .arg(seconds / 60)
                              .arg(seconds % 60, 2, 10, QLatin1Char('0'));
  return engine_.recording_overran() ? elapsed + tr(" · dropped samples")
                                     : elapsed;
}

void MixerModel::toggleMetronome() {
  metronome_ui_ = !metronome_ui_;
  EngineCommand command;
  command.kind = EngineCommand::Kind::SetMetronome;
  command.value = metronome_ui_ ? 1.0f : 0.0f;
  engine_.post(command);
  emit transportChanged();
  markDirty();
}

void MixerModel::rewind() {
  EngineCommand command;
  command.kind = EngineCommand::Kind::Rewind;
  engine_.post(command);
}

void MixerModel::setTempo(qreal bpm) {
  // Below 20 or above 300 is not a tempo anyone meant to set, and plugins
  // divide by it.
  const qreal clamped = qBound(20.0, bpm, 300.0);
  tempo_ui_ = clamped;
  EngineCommand command;
  command.kind = EngineCommand::Kind::SetTempo;
  command.value = static_cast<float>(clamped);
  engine_.post(command);
  emit transportChanged();
  markDirty();
}

bool MixerModel::addInsert(int row, int pluginIndex) {
  return placeInsert(row, pluginIndex, -1) >= 0;
}

bool MixerModel::addInsertAt(int row, int pluginIndex, int targetSlot) {
  return placeInsert(row, pluginIndex, targetSlot) >= 0;
}

// The slot the plugin actually landed in, or -1. The engine fills the first
// hole rather than appending, so "the last insert" is not a safe way for a
// caller to find what it just added.
int MixerModel::placeInsert(int row, int pluginIndex, int targetSlot) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return -1;
  const PluginDescriptor* descriptor = plugins_->descriptor(pluginIndex);
  if (descriptor == nullptr) return -1;

  // Instantiating and activating happen here, on the UI thread. The strip only
  // publishes the insert to the audio thread once it is ready to run.
  std::unique_ptr<PluginInstance> instance = plugins_->instantiate(pluginIndex);
  if (instance == nullptr) {
    emit errorOccurred(tr("Could not load %1")
                           .arg(QString::fromStdString(descriptor->name)));
    return -1;
  }
  ChannelStrip* strip = stripFor(row);
  size_t placed_at = 0;
  if (strip == nullptr || !strip->add_insert(std::move(instance), &placed_at)) {
    emit errorOccurred(tr("Could not activate %1")
                           .arg(QString::fromStdString(descriptor->name)));
    return -1;
  }
  size_t label_at = placed_at;
  if (targetSlot >= 0 && static_cast<int>(placed_at) != targetSlot &&
      targetSlot < static_cast<int>(strip->insert_count())) {
    strip->swap_inserts(placed_at, static_cast<size_t>(targetSlot));
    label_at = static_cast<size_t>(targetSlot);
  }

  // The label list mirrors the engine's slots, holes included, so the new
  // plugin's label lands exactly where the engine put the plugin.
  QStringList& labels = channels_[row].inserts;
  while (labels.size() <= static_cast<int>(label_at)) labels.append(QString());
  labels[static_cast<int>(label_at)] = QString::fromStdString(descriptor->name);
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {InsertsRole, InsertDetailsRole});
  markDirty();
  return static_cast<int>(label_at);
}

void MixerModel::removeInsert(int row, int slot) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  if (slot < 0 || slot >= channels_[row].inserts.size()) return;

  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr) return;
  PluginInstance* dying = strip->insert_at(static_cast<size_t>(slot));
  std::erase_if(editors_, [dying](const OpenEditor& editor) {
    return editor.insert == dying;
  });
  std::erase_if(midi_maps_, [row, slot](const MidiMapping& map) {
    return map.kind == MidiMapping::Kind::Param && map.row == row &&
           map.slot == slot;
  });
  strip->remove_insert(static_cast<size_t>(slot));
  // The engine leaves a hole so the surviving indices stay put; the label list
  // has to keep the same shape or the two would drift apart.
  channels_[row].inserts[slot].clear();
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {InsertsRole, InsertDetailsRole});
  markDirty();
}

void MixerModel::moveInsert(int row, int slot, int direction) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  const int target = slot + direction;

  QStringList& labels = channels_[row].inserts;
  if (slot < 0 || slot >= labels.size()) return;
  if (target < 0 || target >= labels.size()) return;

  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr) return;
  strip->swap_inserts(static_cast<size_t>(slot), static_cast<size_t>(target));
  labels.swapItemsAt(slot, target);

  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {InsertsRole, InsertDetailsRole});
  markDirty();
}

bool MixerModel::openInsertEditor(int row, int slot) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return false;

  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr) return false;
  PluginInstance* insert = strip->insert_at(static_cast<size_t>(slot));
  if (insert == nullptr) return false;

  // A second click on the same insert closes its window rather than opening a
  // twin: open once, close once.
  const auto existing = std::find_if(
      editors_.begin(), editors_.end(),
      [insert](const OpenEditor& editor) { return editor.insert == insert; });
  if (existing != editors_.end()) {
    // A double-click's second press lands right after the open; treating it as
    // the closing click made every editor "open and shut by itself" for anyone
    // who double-clicks by habit.
    if (QDateTime::currentMSecsSinceEpoch() - existing->opened_ms < 600)
      return true;
    qWarning("editor: fechada pelo toggle (segundo clique)");
    editors_.erase(existing);
    // Whatever was done in there is worth a save. LV2 gives the host no way to
    // hear about a kit or a patch loaded inside the plugin's own window, so the
    // window closing is the signal.
    markDirty();
    return true;
  }

  std::unique_ptr<PluginGui> gui = insert->create_gui();
  if (gui == nullptr) return false;

  auto window = std::make_unique<PluginWindow>(
      std::move(gui), QString::fromStdString(insert->descriptor().name));
  if (!window->open()) return false;

  // Closing through the window manager has to leave the list too, or the next
  // click would "close" a window that is already gone. Queued on purpose:
  // closed() is emitted from inside the window's own timer callback, and a
  // direct connection would destroy the window while that callback is still
  // on the stack.
  PluginWindow* raw = window.get();
  connect(
      raw, &PluginWindow::closed, this,
      [this, raw] {
        std::erase_if(editors_, [raw](const OpenEditor& editor) {
          return editor.window.get() == raw;
        });
        markDirty();
      },
      Qt::QueuedConnection);

  editors_.push_back({channels_[row].slot, channels_[row].is_bus, insert,
                      QDateTime::currentMSecsSinceEpoch(), std::move(window)});
  return true;
}

QStringList MixerModel::sources(bool midi) const {
  QStringList out;
  for (const std::string& port : engine_.available_sources(midi))
    out.append(QString::fromStdString(port));
  return out;
}

QStringList MixerModel::sinks() const {
  QStringList out;
  for (const std::string& port : engine_.available_sinks())
    out.append(QString::fromStdString(port));
  return out;
}

void MixerModel::connectSource(int row, const QString& port, bool midi) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  engine_.connect_source(channels_[row].slot, port.toStdString(), midi);
  refreshRouting(row);
}

bool MixerModel::midiLinked(int row, const QString& port) const {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return false;
  if (channels_[row].is_bus) return false;
  for (const std::string& source :
       engine_.current_sources(channels_[row].slot, true))
    if (source == port.toStdString()) return true;
  return false;
}

void MixerModel::setMidiLink(int row, const QString& port, bool on) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  if (channels_[row].is_bus) return;
  engine_.set_midi_link(channels_[row].slot, port.toStdString(), on);
  refreshRouting(row);
}

void MixerModel::connectMaster(const QString& port) {
  // A stereo sink's right side is the next port of the same client, which is
  // how JACK names a pair.
  const QStringList all = sinks();
  const int index = all.indexOf(port);
  QString right;
  if (index >= 0 && index + 1 < all.size()) {
    const QString client = port.section(':', 0, 0);
    if (all[index + 1].startsWith(client + ':')) right = all[index + 1];
  }
  engine_.connect_master(port.toStdString(), right.toStdString());
  emit routingChanged();
  markDirty();
}

QString MixerModel::masterSink() const {
  return shortPortName(QString::fromStdString(engine_.current_master_sink()));
}

// "Midi-Bridge:FM-1: MIDI 1 (capture)" is useless in a 108-pixel strip; the
// client name alone is what identifies it at a glance.
QString MixerModel::shortPortName(const QString& port) {
  if (port.isEmpty()) return {};
  const QString client = port.section(':', 0, 0);
  const QString rest = port.section(':', 1);
  if (client == QLatin1String("Midi-Bridge") && !rest.isEmpty())
    return rest.section(':', 0, 0).trimmed();
  return client;
}

void MixerModel::refreshRouting(int row) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  ChannelUi& channel = channels_[row];
  if (channel.is_bus) return;  // fed by strips, not by ports

  const QString audio = shortPortName(
      QString::fromStdString(engine_.current_source(channel.slot, false)));
  const std::vector<std::string> midi_sources =
      engine_.current_sources(channel.slot, true);

  channel.input_label = audio.isEmpty() ? tr("no input") : audio;
  if (midi_sources.empty()) {
    channel.midi_label = tr("no MIDI");
  } else if (midi_sources.size() == 1) {
    channel.midi_label = shortPortName(QString::fromStdString(midi_sources[0]));
  } else {
    channel.midi_label = tr("%1 sources").arg(midi_sources.size());
  }

  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {InputLabelRole, MidiLabelRole});
  emit routingChanged();
  markDirty();
}

// The insert behind a row and slot, or null. Shared by the generic editor
// calls below.
PluginInstance* MixerModel::insertFor(int row, int slot) const {
  ChannelStrip* strip = const_cast<MixerModel*>(this)->stripFor(row);
  if (strip == nullptr || slot < 0) return nullptr;
  return strip->insert_at(static_cast<size_t>(slot));
}

QVariantList MixerModel::insertParameters(int row, int slot) const {
  QVariantList out;
  PluginInstance* insert = insertFor(row, slot);
  if (insert == nullptr) return out;

  for (const ParameterInfo& parameter : insert->parameters()) {
    QVariantMap entry;
    entry[QStringLiteral("id")] = parameter.id;
    entry[QStringLiteral("name")] = QString::fromStdString(parameter.name);
    entry[QStringLiteral("min")] = parameter.min_value;
    entry[QStringLiteral("max")] = parameter.max_value;
    entry[QStringLiteral("value")] = insert->parameter_value(parameter.id);
    out.append(entry);
  }
  return out;
}

void MixerModel::setInsertParameter(int row, int slot, int id, qreal value) {
  PluginInstance* insert = insertFor(row, slot);
  if (insert == nullptr) return;
  insert->set_parameter(static_cast<uint32_t>(id), value);
  markDirty();
}

QString MixerModel::insertName(int row, int slot) const {
  PluginInstance* insert = insertFor(row, slot);
  if (insert == nullptr) return {};
  return QString::fromStdString(insert->descriptor().name);
}

bool MixerModel::insertIsFilePlayer(int row, int slot) const {
  PluginInstance* insert = insertFor(row, slot);
  return insert != nullptr && insert->descriptor().uid == "nirbija.fileplayer";
}

bool MixerModel::insertIsStepSequencer(int row, int slot) const {
  PluginInstance* insert = insertFor(row, slot);
  return insert != nullptr && insert->descriptor().uid == "nirbija.stepseq";
}

bool MixerModel::insertIsScript(int row, int slot) const {
  PluginInstance* insert = insertFor(row, slot);
  return insert != nullptr && insert->descriptor().uid == "nirbija.script";
}

bool MixerModel::insertIsKeyboardInstrument(int row, int slot) const {
  PluginInstance* insert = insertFor(row, slot);
  return insert != nullptr && insert->descriptor().uid == "nirbija.keyboard";
}

void MixerModel::pressComputerKey(int row, int slot, int note, int velocity) {
  auto* keyboard = dynamic_cast<KeyboardInstrumentInstance*>(insertFor(row, slot));
  if (keyboard == nullptr) return;
  keyboard->key_down(note, velocity);
}

void MixerModel::releaseComputerKey(int row, int slot, int note) {
  auto* keyboard = dynamic_cast<KeyboardInstrumentInstance*>(insertFor(row, slot));
  if (keyboard == nullptr) return;
  keyboard->key_up(note);
}

QString MixerModel::insertScript(int row, int slot) const {
  auto* script = dynamic_cast<ScriptInstance*>(insertFor(row, slot));
  return script == nullptr ? QString()
                           : QString::fromStdString(script->script());
}

QString MixerModel::insertScriptError(int row, int slot) const {
  auto* script = dynamic_cast<ScriptInstance*>(insertFor(row, slot));
  return script == nullptr ? QString()
                           : QString::fromStdString(script->error());
}

bool MixerModel::setInsertScript(int row, int slot, const QString& source) {
  auto* script = dynamic_cast<ScriptInstance*>(insertFor(row, slot));
  if (script == nullptr) return false;
  const bool ok = script->set_script(source.toStdString());
  markDirty();
  return ok;
}

int MixerModel::insertPlayhead(int row, int slot) const {
  PluginInstance* insert = insertFor(row, slot);
  return insert == nullptr ? -1 : insert->playhead();
}

QVariantMap MixerModel::insertSequencerSnapshot(int row, int slot) const {
  QVariantMap out;
  auto* seq = dynamic_cast<StepSequencerInstance*>(insertFor(row, slot));
  if (seq == nullptr) return out;

  out[QStringLiteral("pattern")] = seq->pattern();
  out[QStringLiteral("nextPattern")] = seq->next_pattern();
  out[QStringLiteral("fill")] = seq->fill();
  out[QStringLiteral("recording")] = seq->recording();
  out[QStringLiteral("focusedLane")] = seq->focus();
  out[QStringLiteral("view")] = seq->view();
  out[QStringLiteral("transpose")] = seq->transpose();
  out[QStringLiteral("swing")] = static_cast<qreal>(seq->swing());
  out[QStringLiteral("scale")] = seq->scale();
  out[QStringLiteral("root")] = seq->root();

  QVariantList macros;
  macros.reserve(4);
  for (int i = 0; i < 4; ++i) macros.append(static_cast<qreal>(seq->macro(i)));
  out[QStringLiteral("macros")] = macros;

  QVariantList heads;
  heads.reserve(StepSequencerInstance::kLanes +
                StepSequencerInstance::kExtraHeads);
  for (int lane = 0; lane < StepSequencerInstance::kLanes; ++lane) {
    const int step = seq->native_head_step(lane);
    heads.append(step < 0 ? -1 : ((lane << 8) | (step & 0xff)));
  }
  for (int extra = 0; extra < StepSequencerInstance::kExtraHeads; ++extra) {
    const int step = seq->extra_head_step(extra);
    if (seq->extra_head_muted(extra) || step < 0) {
      heads.append(-1);
      continue;
    }
    const int lane = seq->extra_head_lane(extra);
    heads.append(0x10000 | (lane << 8) | (step & 0xff));
  }
  out[QStringLiteral("heads")] = heads;

  QVariantList lanes;
  lanes.reserve(StepSequencerInstance::kLanes * 7);
  QVariantList gates;
  gates.reserve(StepSequencerInstance::kLanes);
  for (int lane = 0; lane < StepSequencerInstance::kLanes; ++lane) {
    lanes.append(seq->lane_note(lane));
    lanes.append(seq->lane_length(lane));
    lanes.append(seq->lane_muted(lane) ? 1 : 0);
    lanes.append(seq->lane_channel(lane));
    lanes.append(seq->lane_division(lane));
    lanes.append(seq->lane_direction(lane));
    lanes.append(seq->lane_euclid(lane));
    gates.append(seq->lane_gate(lane));
  }
  out[QStringLiteral("lanes")] = lanes;
  out[QStringLiteral("gates")] = gates;

  const int pattern = seq->pattern();
  const int plane = StepSequencerInstance::kLanes *
                    StepSequencerInstance::kMaxSteps;
  QVariantList on, accent, tie, note, vel, ratchet, cond, condArg, chance, micro;
  on.reserve(plane);
  accent.reserve(plane);
  tie.reserve(plane);
  note.reserve(plane);
  vel.reserve(plane);
  ratchet.reserve(plane);
  cond.reserve(plane);
  condArg.reserve(plane);
  chance.reserve(plane);
  micro.reserve(plane);
  for (int lane = 0; lane < StepSequencerInstance::kLanes; ++lane) {
    for (int step = 0; step < StepSequencerInstance::kMaxSteps; ++step) {
      on.append(seq->cell_active(pattern, lane, step) ? 1 : 0);
      accent.append(seq->cell_accent(pattern, lane, step) ? 1 : 0);
      tie.append(seq->cell_tie(pattern, lane, step) ? 1 : 0);
      note.append(seq->cell_note(pattern, lane, step));
      vel.append(seq->cell_velocity(pattern, lane, step));
      ratchet.append(seq->cell_ratchet(pattern, lane, step));
      cond.append(seq->cell_condition(pattern, lane, step));
      condArg.append(seq->cell_cond_arg(pattern, lane, step));
      chance.append(static_cast<qreal>(seq->cell_probability(pattern, lane, step)));
      micro.append(static_cast<qreal>(seq->cell_microtiming(pattern, lane, step)));
    }
  }
  out[QStringLiteral("on")] = on;
  out[QStringLiteral("accent")] = accent;
  out[QStringLiteral("tie")] = tie;
  out[QStringLiteral("note")] = note;
  out[QStringLiteral("vel")] = vel;
  out[QStringLiteral("ratchet")] = ratchet;
  out[QStringLiteral("cond")] = cond;
  out[QStringLiteral("condArg")] = condArg;
  out[QStringLiteral("chance")] = chance;
  out[QStringLiteral("micro")] = micro;
  return out;
}

QVariantMap MixerModel::insertSequencerTarget(int row, int slot) const {
  // MIDI walks the chain in slot order. The sequencer only ever addresses
  // the next live insert — merging every chip on the strip would name an
  // Odin key "kick" the moment a kit sat below it.
  QVariantMap out;
  ChannelStrip* strip = const_cast<MixerModel*>(this)->stripFor(row);
  if (strip == nullptr || slot < 0) return out;
  if (dynamic_cast<StepSequencerInstance*>(insertFor(row, slot)) == nullptr)
    return out;

  PluginInstance* target = nullptr;
  const size_t count = strip->insert_count();
  for (size_t i = static_cast<size_t>(slot) + 1; i < count; ++i) {
    PluginInstance* candidate = strip->insert_at(i);
    if (candidate == nullptr) continue;
    target = candidate;
    break;
  }
  if (target == nullptr) return out;

  out[QStringLiteral("name")] =
      QString::fromStdString(target->descriptor().name);

  std::vector<NoteName> named = target->note_names();
  std::stable_sort(named.begin(), named.end(),
                    [](const NoteName& a, const NoteName& b) { return a.key < b.key; });

  QVariantList pads;
  pads.reserve(static_cast<int>(named.size()));
  bool seen[128] = {};
  for (const NoteName& entry : named) {
    if (entry.key < 0 || entry.key > 127 || entry.name.empty()) continue;
    if (seen[entry.key]) continue;
    seen[entry.key] = true;
    QVariantMap pad;
    pad[QStringLiteral("note")] = entry.key;
    pad[QStringLiteral("name")] = QString::fromStdString(entry.name);
    pads.append(pad);
  }
  out[QStringLiteral("pads")] = pads;
  return out;
}

void MixerModel::setSequencerCell(int row, int slot, int pattern, int lane,
                                 int step, int note, int velocity, bool on,
                                 qreal chance, bool accent, bool tie) {
  auto* seq = dynamic_cast<StepSequencerInstance*>(insertFor(row, slot));
  if (seq == nullptr) return;
  seq->set_cell(pattern, lane, step, note, velocity, on,
                static_cast<float>(chance), accent, tie);
  markDirty();
}

void MixerModel::setSequencerTrig(int row, int slot, int pattern, int lane,
                                 int step, qreal micro, int ratchet, int cond,
                                 int condArg) {
  auto* seq = dynamic_cast<StepSequencerInstance*>(insertFor(row, slot));
  if (seq == nullptr) return;
  seq->set_trig(pattern, lane, step, static_cast<float>(micro), ratchet, cond,
                condArg);
  markDirty();
}

void MixerModel::setSequencerLane(int row, int slot, int lane, int note,
                                 int length, int division, int direction,
                                 int channel, bool mute, qreal gate) {
  auto* seq = dynamic_cast<StepSequencerInstance*>(insertFor(row, slot));
  if (seq == nullptr) return;
  seq->set_lane(lane, note, length, division, direction, channel, mute, gate);
  markDirty();
}

void MixerModel::setSequencerLaneEuclid(int row, int slot, int lane, int pulses) {
  auto* seq = dynamic_cast<StepSequencerInstance*>(insertFor(row, slot));
  if (seq == nullptr) return;
  seq->set_lane_euclid(lane, pulses);
  markDirty();
}

void MixerModel::setSequencerHead(int row, int slot, int extra, int lane,
                                 int rate, int direction, int start, int length,
                                 int transpose, bool mute) {
  auto* seq = dynamic_cast<StepSequencerInstance*>(insertFor(row, slot));
  if (seq == nullptr) return;
  seq->set_extra_head(extra, lane, rate, direction, start, length, transpose,
                      mute);
  markDirty();
}

bool MixerModel::setInsertFile(int row, int slot, const QUrl& file) {
  auto* player = dynamic_cast<FilePlayerInstance*>(insertFor(row, slot));
  if (player == nullptr) return false;

  const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
  const bool loaded = player->load(path.toStdString());
  if (!loaded) qWarning("file player: could not read %s", qUtf8Printable(path));
  markDirty();
  return loaded;
}

QString MixerModel::insertFilePath(int row, int slot) const {
  auto* player = dynamic_cast<FilePlayerInstance*>(insertFor(row, slot));
  if (player == nullptr) return {};
  return QString::fromStdString(player->path());
}

void MixerModel::closeAllEditors() { editors_.clear(); }

void MixerModel::newSession() {
  closeAllEditors();
  midi_maps_.clear();
  while (rowCount() > 0) removeChannel(0);
  setMasterGain(1.0);
  setTempo(120.0);
  if (engine_.metronome()) toggleMetronome();
  if (masterDim()) toggleMasterDim();
  if (masterMute()) toggleMasterMute();
  if (masterMono()) toggleMasterMono();
  if (engine_.midi_clock()) toggleMidiClock();
  if (engine_.follow_midi_clock()) toggleFollowMidiClock();
  setTimeSignature(4, 4);
  if (playing_ui_) togglePlay();
  saveSession();
}

QString MixerModel::recordingsUrl() {
  return QUrl::fromLocalFile(recordingsPath()).toString();
}

void MixerModel::learnGain(int row) {
  pending_learn_ = {true,
                    {.kind = MidiMapping::Kind::Gain,
                     .row = row,
                     .graph_slot = static_cast<int>(channels_[row].slot),
                     .is_bus = channels_[row].is_bus}};
  engine_.connect_all_midi_to_control();
  emit learnChanged();
}

void MixerModel::learnPan(int row) {
  pending_learn_ = {true,
                    {.kind = MidiMapping::Kind::Pan,
                     .row = row,
                     .graph_slot = static_cast<int>(channels_[row].slot),
                     .is_bus = channels_[row].is_bus}};
  engine_.connect_all_midi_to_control();
  emit learnChanged();
}

void MixerModel::learnMute(int row) {
  pending_learn_ = {true,
                    {.kind = MidiMapping::Kind::Mute,
                     .row = row,
                     .graph_slot = static_cast<int>(channels_[row].slot),
                     .is_bus = channels_[row].is_bus}};
  engine_.connect_all_midi_to_control();
  emit learnChanged();
}

void MixerModel::learnInsertParam(int row, int slot, int param, qreal min,
                                  qreal max) {
  pending_learn_ = {true,
                    {.kind = MidiMapping::Kind::Param,
                     .row = row,
                     .graph_slot = static_cast<int>(channels_[row].slot),
                     .is_bus = channels_[row].is_bus,
                     .slot = slot,
                     .param = static_cast<uint32_t>(param),
                     .min = min,
                     .max = max}};
  engine_.connect_all_midi_to_control();
  emit learnChanged();
}

void MixerModel::cancelLearn() {
  pending_learn_.armed = false;
  emit learnChanged();
}

bool MixerModel::insertParamMapped(int row, int slot, int param) const {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return false;
  const int graph = static_cast<int>(channels_[row].slot);
  const bool bus = channels_[row].is_bus;
  for (const MidiMapping& map : midi_maps_) {
    if (map.kind == MidiMapping::Kind::Param && map.graph_slot == graph &&
        map.is_bus == bus && map.slot == slot &&
        map.param == static_cast<uint32_t>(param) && map.cc >= 0)
      return true;
  }
  return false;
}

void MixerModel::clearMidiMaps(int row) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  const int slot = static_cast<int>(channels_[row].slot);
  const bool bus = channels_[row].is_bus;
  std::erase_if(midi_maps_, [slot, bus](const MidiMapping& map) {
    return map.graph_slot == slot && map.is_bus == bus;
  });
  markDirty();
}

void MixerModel::handleControl(int cc, int channel, int value) {
  // Learning takes the message rather than acting on it, so arming a fader and
  // sweeping the knob does not also drag whatever it was bound to before.
  if (pending_learn_.armed) {
    // Notes and hard 0/127 are buttons. A knob's first value almost never
    // sits on the rail, so Rec/Play learned from a toggle pad flip on
    // press instead of tracking the 0 it sends when it latches off.
    pending_learn_.target.cc = cc;
    pending_learn_.target.midi_channel = channel;
    pending_learn_.target.toggle = (cc >= 128 || value <= 1 || value >= 126);
    // One binding per control per target: relearning replaces.
    std::erase_if(midi_maps_, [this](const MidiMapping& map) {
      return map.kind == pending_learn_.target.kind &&
             map.row == pending_learn_.target.row &&
             map.slot == pending_learn_.target.slot &&
             map.param == pending_learn_.target.param;
    });
    midi_maps_.push_back(pending_learn_.target);
    pending_learn_.armed = false;
    emit learnChanged();
    markDirty();
    return;
  }

  const qreal normal = value / 127.0;
  for (const MidiMapping& map : midi_maps_) {
    if (map.cc != cc || map.midi_channel != channel) continue;
    int row = map.row;
    if (map.graph_slot >= 0) {
      row = -1;
      for (int i = 0; i < static_cast<int>(channels_.size()); ++i) {
        if (static_cast<int>(channels_[i].slot) == map.graph_slot &&
            channels_[i].is_bus == map.is_bus) {
          row = i;
          break;
        }
      }
      // A map from before the session reload still names the old slot.
      // The row it was learned on is the next best thing.
      if (row < 0 && map.row >= 0 &&
          map.row < static_cast<int>(channels_.size()))
        row = map.row;
    }
    if (row < 0 || row >= static_cast<int>(channels_.size())) continue;

    switch (map.kind) {
      case MidiMapping::Kind::Gain:
        // Through the fader curve, so the knob feels like the fader it drives.
        setGain(row, faderToGain(normal));
        break;
      case MidiMapping::Kind::Pan:
        setPan(row, normal * 2.0 - 1.0);
        break;
      case MidiMapping::Kind::Mute:
        // Absolute, not a toggle: a pedal sending 127/0 means down/up, and a
        // toggle would fall out of step with it.
        if (channels_[row].muted != (value >= 64)) toggleMute(row);
        break;
      case MidiMapping::Kind::Param:
        if (map.toggle) {
          // Releases and the off half of a toggle pad are noise. The press
          // flips whatever Rec or Play is doing now.
          if (value < 64) break;
          PluginInstance* insert = insertFor(row, map.slot);
          if (insert == nullptr) break;
          const double current =
              insert->parameter_value(static_cast<uint32_t>(map.param));
          const double mid = (map.min + map.max) * 0.5;
          setInsertParameter(row, map.slot, static_cast<int>(map.param),
                             current >= mid ? map.min : map.max);
        } else {
          setInsertParameter(row, map.slot, static_cast<int>(map.param),
                             map.min + (map.max - map.min) * normal);
        }
        break;
    }
  }
}

void MixerModel::injectControl(int cc, int channel, int value) {
  handleControl(cc, channel, value);
}

void MixerModel::setMetersActive(bool on) {
  if (meters_active_ == on) return;
  meters_active_ = on;
  emit metersActiveChanged();
}

// One poll of ballistics. Rise is instantaneous — a transient that only shows
// on one frame still has to be visible — and the fall is a fixed slope, about
// 26 dB per second, which is close to what a hardware meter does. The held mark
// waits half a second before it starts down, or it never lasts long enough to
// be read.
void MixerModel::advanceMeter(qreal peak, qreal& position, qreal& hold,
                              int& age) {
  constexpr qreal kFallPerPoll = 0.0115;      // of full travel, per 33 ms tick
  constexpr qreal kHoldFallPerPoll = 0.0060;  // the mark falls slower
  constexpr int kHoldTicks = 15;              // ~500 ms before it lets go

  const qreal target = gainToFader(peak);
  position = target >= position ? target
                                : std::max(target, position - kFallPerPoll);

  if (target >= hold) {
    hold = target;
    age = 0;
  } else if (++age > kHoldTicks) {
    hold = std::max(position, hold - kHoldFallPerPoll);
  }
}

void MixerModel::pollLevels() {
  if (!engine_.running()) return;

  // Controller messages ride the same poll as the meters: 30 Hz is fine for a
  // knob, and the audio thread stays out of the mapping table entirely.
  MidiEvent control[64];
  const size_t count = engine_.poll_control(control, 64);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t status = control[i].data[0] & 0xf0;
    const uint8_t channel = control[i].data[0] & 0x0f;
    if (control[i].size < 3) continue;
    if (status == 0xb0) {
      handleControl(control[i].data[1], channel, control[i].data[2]);
    } else if (status == 0x90) {
      // Pads often send notes, not CCs. Note-on with velocity 0 is off.
      const int number = 128 + static_cast<int>(control[i].data[1]);
      handleControl(number, channel, control[i].data[2]);
    } else if (status == 0x80) {
      handleControl(128 + static_cast<int>(control[i].data[1]), channel, 0);
    }
  }

  // Reading the peaks is what clears them on the audio side, so it happens
  // whether or not anyone is looking; only the redraw is skipped.
  for (size_t row = 0; row < channels_.size(); ++row) {
    ChannelUi& channel = channels_[row];
    ChannelStrip* strip = stripFor(static_cast<int>(row));
    if (strip == nullptr) continue;

    for (int ch = 0; ch < channel.width; ++ch)
      channel.peak[ch] = strip->read_peak(ch);
    if (channel.width == 1) channel.peak[1] = channel.peak[0];
    for (int ch = 0; ch < 2; ++ch) {
      advanceMeter(channel.peak[ch], channel.position[ch], channel.hold[ch],
                   channel.hold_age[ch]);
    }
  }

  master_peak_[0] = engine_.graph().read_master_peak(0);
  master_peak_[1] = engine_.graph().read_master_peak(1);
  for (int ch = 0; ch < 2; ++ch) {
    advanceMeter(master_peak_[ch], master_position_[ch], master_hold_[ch],
                 master_hold_age_[ch]);
  }
  master_clip_ = engine_.graph().read_master_clip() > 0.0f;

  if (meters_active_) {
    if (!channels_.empty()) {
      emit dataChanged(index(0), index(static_cast<int>(channels_.size()) - 1),
                       {PeakLeftRole, PeakRightRole, PositionLeftRole,
                        PositionRightRole, HoldLeftRole, HoldRightRole,
                        InsertDetailsRole});
    }
    emit levelsChanged();
  }

  engine_.graph().reclaim();
  bool plugin_state_moved = false;
  for (size_t row = 0; row < channels_.size(); ++row) {
    ChannelStrip* strip = stripFor(static_cast<int>(row));
    if (strip == nullptr) continue;
    strip->reclaim();
    for (size_t slot = 0; slot < strip->insert_count(); ++slot) {
      PluginInstance* insert = strip->insert_at(slot);
      if (insert == nullptr) continue;
      insert->host_idle();
      // A patch or a preset loaded from the plugin's own window goes through
      // none of this model's setters, so without asking, the session would
      // never learn it had anything new to write.
      if (insert->take_state_dirty()) plugin_state_moved = true;
      // Length reading Sync: this instance never looks at the graph around
      // it, so the host measures the target here once a poll and hands the
      // number in. A target with no closed loop of its own reads as 0, the
      // same as free-running, rather than leaving the last thing it had.
      if (auto* looper = dynamic_cast<LooperInstance*>(insert)) {
        if (looper->quantize() == LooperInstance::kQuantizeSync) {
          auto* source = dynamic_cast<LooperInstance*>(
              insertFor(looper->sync_target_row(), looper->sync_target_slot()));
          looper->set_sync_beats(
              source != nullptr && source->loop_closed()
                  ? source->loop_beats()
                  : 0.0);
        }
      }
    }
  }
  if (plugin_state_moved) markDirty();

  if (engine_.follow_midi_clock()) {
    const bool now = engine_.playing();
    if (now != playing_ui_) {
      playing_ui_ = now;
      emit transportChanged();
    }
  }
}

// A fader that is linear in decibels spends most of its travel where the ear
// cares, and reaches silence at the bottom instead of merely getting quiet.
qreal MixerModel::faderToGain(qreal position) {
  if (position <= 0.0) return 0.0;
  const qreal db = kMinDb + (kMaxDb - kMinDb) * position;
  return std::pow(10.0, db / 20.0);
}

qreal MixerModel::gainToFader(qreal gain) {
  if (gain <= 0.0) return 0.0;
  const qreal db = 20.0 * std::log10(gain);
  return qBound(0.0, (db - kMinDb) / (kMaxDb - kMinDb), 1.0);
}

QString MixerModel::positionLabel() const {
  if (engine_.sample_rate() <= 0.0) return QStringLiteral("1.1");
  const double beats =
      static_cast<double>(engine_.transport_frame()) / engine_.sample_rate() *
      engine_.tempo() / 60.0;
  const int num = std::max(1, engine_.time_numerator());
  const int bar = static_cast<int>(beats / num) + 1;
  const int beat = static_cast<int>(std::fmod(beats, num)) + 1;
  return QStringLiteral("%1.%2").arg(bar).arg(beat);
}

void MixerModel::pushUndo() {
  if (restoring_) return;
  redo_stack_.clear();
  undo_stack_.push_back(snapshot());
  while (undo_stack_.size() > 16) undo_stack_.removeFirst();
  emit dirtyChanged();
}

QByteArray MixerModel::snapshot() const {
  const QString path = sessionPath() + QStringLiteral(".snap");
  writeSession(path);
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return file.readAll();
}

void MixerModel::restoreSnapshot(const QByteArray& blob) {
  if (blob.isEmpty()) return;
  const QString path = sessionPath() + QStringLiteral(".snap");
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
  file.write(blob);
  file.close();
  closeAllEditors();
  midi_maps_.clear();
  restoring_ = true;
  while (rowCount() > 0) removeChannel(0);
  restoring_ = false;
  readSession(path);
}

void MixerModel::undo() {
  if (undo_stack_.isEmpty()) return;
  redo_stack_.push_back(snapshot());
  const QByteArray blob = undo_stack_.takeLast();
  restoreSnapshot(blob);
  emit dirtyChanged();
}

void MixerModel::redo() {
  if (redo_stack_.isEmpty()) return;
  undo_stack_.push_back(snapshot());
  restoreSnapshot(redo_stack_.takeLast());
  emit dirtyChanged();
}

void MixerModel::duplicateChannel(int row) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  pushUndo();
  const ChannelUi src = channels_[row];

  // Every plugin's state, taken with the graph parked - the LV2 spec is clear
  // that save_state is not something to ask for while the instance is running,
  // and the same rule is what makes a saved session reload correctly.
  std::vector<std::vector<uint8_t>> blobs;
  std::vector<int> plugin_rows;
  std::vector<bool> bypassed;
  std::vector<bool> post_fader;
  if (ChannelStrip* strip = stripFor(row)) {
    engine_.park_graph();
    for (size_t slot = 0; slot < strip->insert_count(); ++slot) {
      PluginInstance* insert = strip->insert_at(slot);
      if (insert == nullptr) continue;  // a hole left by a removal
      const PluginDescriptor& descriptor = insert->descriptor();
      plugin_rows.push_back(plugins_->rowFor(descriptor.format, descriptor.uid));
      blobs.push_back(insert->save_state());
      bypassed.push_back(strip->insert_bypassed(slot));
      post_fader.push_back(strip->insert_post_fader(slot));
    }
    engine_.unpark_graph();
  }

  if (src.is_bus) {
    addBus(src.name + QStringLiteral(" copy"));
  } else {
    addChannel(src.name + QStringLiteral(" copy"), src.width);
  }
  const int dest = rowCount() - 1;
  if (dest == row) return;  // the graph was full

  setGain(dest, src.gain);
  setPan(dest, src.pan);
  if (src.muted) toggleMute(dest);
  if (src.soloed) toggleSolo(dest);
  setMidiMask(dest, midiMask(row));
  setDestination(dest, src.destination);

  // The chain, in order, each plugin handed back the state its twin was in.
  // A copy of a strip that arrives empty is not a copy of anything.
  engine_.park_graph();
  for (size_t i = 0; i < plugin_rows.size(); ++i) {
    if (plugin_rows[i] < 0) continue;  // no longer installed
    const int slot = placeInsert(dest, plugin_rows[i], -1);
    if (slot < 0) break;  // the chain is full
    setInsertBypassed(dest, slot, bypassed[i]);
    setInsertPostFader(dest, slot, post_fader[i]);
    if (blobs[i].empty()) continue;

    ChannelStrip* strip = stripFor(dest);
    if (strip == nullptr) break;
    PluginInstance* insert = strip->insert_at(static_cast<size_t>(slot));
    if (insert != nullptr) insert->load_state(blobs[i]);
  }
  engine_.unpark_graph();

  // Sends last, the way a session restores them, since they name a bus.
  const QVariantList sends = src.sends;
  for (int slot = 0; slot < sends.size(); ++slot) {
    const QVariantMap send = sends[slot].toMap();
    setSend(dest, slot, send.value(QStringLiteral("bus")).toInt(),
            send.value(QStringLiteral("level")).toDouble());
  }
}

void MixerModel::moveChannel(int row, int direction) {
  const int target = row + direction;
  if (row < 0 || target < 0 || row >= static_cast<int>(channels_.size()) ||
      target >= static_cast<int>(channels_.size()))
    return;
  pushUndo();
  beginMoveRows({}, row, row, {}, target > row ? target + 1 : target);
  std::swap(channels_[row], channels_[target]);
  endMoveRows();
  markDirty();
}

void MixerModel::setInsertBypassed(int row, int slot, bool on) {
  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr) return;
  strip->set_insert_bypassed(static_cast<size_t>(slot), on);
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {InsertDetailsRole});
  markDirty();
}

bool MixerModel::insertBypassed(int row, int slot) const {
  ChannelStrip* strip = stripFor(row);
  return strip != nullptr && strip->insert_bypassed(static_cast<size_t>(slot));
}

void MixerModel::setInsertPostFader(int row, int slot, bool on) {
  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr) return;
  strip->set_insert_post_fader(static_cast<size_t>(slot), on);
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {InsertDetailsRole});
  markDirty();
}

bool MixerModel::insertPostFader(int row, int slot) const {
  ChannelStrip* strip = stripFor(row);
  return strip != nullptr && strip->insert_post_fader(static_cast<size_t>(slot));
}

int MixerModel::extraOutputPairs(int row, int slot) const {
  PluginInstance* insert = insertFor(row, slot);
  return insert != nullptr ? insert->extra_output_pairs() : 0;
}

void MixerModel::addTapChannels(int row, int slot) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  const int pairs = extraOutputPairs(row, slot);
  if (pairs <= 0) return;
  pushUndo();
  const size_t source = channels_[row].slot;
  for (int p = 0; p < pairs; ++p) {
    const size_t index = engine_.add_tap_channel(source, p);
    if (index == kMaxChannels) break;
    beginInsertRows({}, rowCount(), rowCount());
    ChannelUi tap;
    tap.slot = index;
    tap.name = channels_[row].name + QStringLiteral(" out %1").arg(p + 2);
    tap.width = 2;
    tap.input_label = tr("plugin tap");
    tap.midi_label = tr("no MIDI");
    tap.output_label = tr("Master");
    tap.accent = channels_[row].accent;
    channels_.push_back(std::move(tap));
    endInsertRows();
  }
  markDirty();
}

void MixerModel::setMidiMask(int row, int mask) {
  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr) return;
  strip->set_midi_mask(static_cast<uint16_t>(mask));
  markDirty();
}

int MixerModel::midiMask(int row) const {
  ChannelStrip* strip = stripFor(row);
  return strip != nullptr ? strip->midi_mask() : 0xFFFF;
}

void MixerModel::sendNote(int row, int note, int velocity) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  if (channels_[row].is_bus) return;
  MidiEvent event;
  event.size = 3;
  event.data[0] = static_cast<uint8_t>(velocity > 0 ? 0x90 : 0x80);
  event.data[1] = static_cast<uint8_t>(std::clamp(note, 0, 127));
  event.data[2] = static_cast<uint8_t>(std::clamp(velocity, 0, 127));
  engine_.inject_midi(channels_[row].slot, event);
}

void MixerModel::connectChannelSink(int row, const QString& port) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  engine_.connect_channel_sink(channels_[row].slot, port.toStdString());
  markDirty();
}

QString MixerModel::channelSink(int row) const {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return {};
  return shortPortName(
      QString::fromStdString(engine_.current_channel_sink(channels_[row].slot)));
}

void MixerModel::setSidechain(int row, int sourceRow) {
  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr) return;
  int slot = -1;
  if (sourceRow >= 0 && sourceRow < static_cast<int>(channels_.size()))
    slot = static_cast<int>(channels_[sourceRow].slot);
  strip->set_sidechain_slot(slot);
  markDirty();
}

int MixerModel::sidechainRow(int row) const {
  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr) return -1;
  const int slot = strip->sidechain_slot();
  for (int i = 0; i < static_cast<int>(channels_.size()); ++i)
    if (static_cast<int>(channels_[i].slot) == slot) return i;
  return -1;
}

bool MixerModel::insertIsLooper(int row, int slot) const {
  PluginInstance* insert = insertFor(row, slot);
  return insert != nullptr && insert->descriptor().uid == "nirbija.looper";
}

bool MixerModel::insertIsFxPad(int row, int slot) const {
  PluginInstance* insert = insertFor(row, slot);
  return insert != nullptr && insert->descriptor().uid == "nirbija.fxpad";
}

void MixerModel::setFxPad(int row, int slot, int pad, bool on) {
  auto* fx = dynamic_cast<FxPadInstance*>(insertFor(row, slot));
  if (fx == nullptr) return;
  fx->set_pad(pad, on);
  // Which pads are down still belongs to the session, but a pad is pressed
  // mid-take: arming the autosave here would park the graph a second later
  // and drop a hole in the very performance the pad was hit for.
  markDirty(false);
}

bool MixerModel::fxPadOn(int row, int slot, int pad) const {
  auto* fx = dynamic_cast<FxPadInstance*>(insertFor(row, slot));
  return fx != nullptr && fx->pad_on(pad);
}

void MixerModel::setFxPadAmount(int row, int slot, int pad, qreal amount) {
  auto* fx = dynamic_cast<FxPadInstance*>(insertFor(row, slot));
  if (fx == nullptr) return;
  fx->set_pad_amount(pad, static_cast<float>(amount));
  markDirty(false);
}

qreal MixerModel::fxPadAmount(int row, int slot, int pad) const {
  auto* fx = dynamic_cast<FxPadInstance*>(insertFor(row, slot));
  return fx != nullptr ? static_cast<qreal>(fx->pad_amount(pad)) : 0.0;
}

bool MixerModel::fxPadBipolar(int pad) const {
  return FxPadInstance::pad_bipolar(pad);
}

void MixerModel::setFxPadHold(int row, int slot, bool on) {
  auto* fx = dynamic_cast<FxPadInstance*>(insertFor(row, slot));
  if (fx == nullptr) return;
  fx->set_hold(on);
  markDirty(false);
}

bool MixerModel::fxPadHold(int row, int slot) const {
  auto* fx = dynamic_cast<FxPadInstance*>(insertFor(row, slot));
  return fx != nullptr && fx->hold();
}

void MixerModel::setLooperRecord(int row, int slot, bool on) {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  if (looper == nullptr) return;
  if (on) {
    // Snapshot before the audio thread starts writing. Parked so the copy
    // cannot tear a sample the process callback is mid-overdub.
    engine_.park_graph();
    if (looper->loop_closed())
      looper->capture_undo();
    else
      looper->capture_undo_empty();
    engine_.unpark_graph();
  }
  looper->set_parameter(0, on ? 1.0 : 0.0);
  markDirty();
}

void MixerModel::setLooperPlay(int row, int slot, bool on) {
  PluginInstance* insert = insertFor(row, slot);
  if (insert != nullptr) insert->set_parameter(1, on ? 1.0 : 0.0);
}

void MixerModel::clearLooper(int row, int slot) {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  if (looper == nullptr) return;
  engine_.park_graph();
  looper->capture_undo();
  looper->set_parameter(2, 1.0);
  engine_.unpark_graph();
  markDirty();
}

bool MixerModel::looperCanUndo(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper != nullptr && looper->can_undo();
}

bool MixerModel::looperCanRedo(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper != nullptr && looper->can_redo();
}

void MixerModel::undoLooper(int row, int slot) {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  if (looper == nullptr || !looper->can_undo()) return;
  engine_.park_graph();
  looper->undo();
  engine_.unpark_graph();
  markDirty();
}

void MixerModel::redoLooper(int row, int slot) {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  if (looper == nullptr || !looper->can_redo()) return;
  engine_.park_graph();
  looper->redo();
  engine_.unpark_graph();
  markDirty();
}

void MixerModel::multiplyLooper(int row, int slot) {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  if (looper == nullptr || !looper->can_multiply()) return;
  engine_.park_graph();
  looper->capture_undo();
  looper->multiply();
  engine_.unpark_graph();
  markDirty();
}

bool MixerModel::looperCanMultiply(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper != nullptr && looper->can_multiply();
}

QVariantList MixerModel::looperWaveform(int row, int slot, int buckets) const {
  QVariantList out;
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  if (looper == nullptr) return out;
  for (float peak : looper->waveform(buckets)) out.append(peak);
  return out;
}

qreal MixerModel::looperTrimStart(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper == nullptr ? 0.0 : looper->trim_start();
}

qreal MixerModel::looperTrimEnd(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper == nullptr ? 1.0 : looper->trim_end();
}

qreal MixerModel::looperFadeIn(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper == nullptr ? 0.0 : looper->fade_in();
}

qreal MixerModel::looperFadeOut(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper == nullptr ? 0.0 : looper->fade_out();
}

void MixerModel::setLooperTrim(int row, int slot, qreal start, qreal end) {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  if (looper == nullptr) return;
  looper->set_trim(start, end);
  markDirty();
}

void MixerModel::setLooperFades(int row, int slot, qreal fadeIn, qreal fadeOut) {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  if (looper == nullptr) return;
  looper->set_fades(fadeIn, fadeOut);
  markDirty();
}

qreal MixerModel::looperPosition(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper == nullptr ? -1.0 : looper->position_fraction();
}

bool MixerModel::looperRecording(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper != nullptr && looper->recording();
}

bool MixerModel::looperCountIn(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper != nullptr && looper->count_in();
}

void MixerModel::setLooperCountIn(int row, int slot, bool on) {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  if (looper == nullptr) return;
  looper->set_count_in(on);
  markDirty(false);
}

int MixerModel::looperCountBeats(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper != nullptr ? looper->count_in_beats() : 0;
}

bool MixerModel::looperPlaying(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper != nullptr && looper->playing();
}

bool MixerModel::looperHasAudio(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper != nullptr && looper->has_audio();
}

bool MixerModel::looperLoopClosed(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper != nullptr && looper->loop_closed();
}

qreal MixerModel::looperBeats(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper == nullptr ? 0.0 : looper->loop_beats();
}

qreal MixerModel::looperLevel(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper == nullptr ? 0.0 : static_cast<qreal>(looper->loop_peak());
}

QVariantList MixerModel::looperLayers(int row, int slot, int buckets) const {
  QVariantList out;
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  if (looper == nullptr) return out;
  for (int layer : looper->layer_map(buckets)) out.append(layer);
  return out;
}

void MixerModel::setLooperSyncTarget(int row, int slot, int targetRow,
                                     int targetSlot) {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  if (looper == nullptr) return;
  looper->set_sync_target(targetRow, targetSlot);
  // Nothing pushed yet at the new target: a stale beat count from whatever
  // was picked before would otherwise sit there until the next poll.
  looper->set_sync_beats(0.0);
  markDirty(false);
}

int MixerModel::looperSyncTargetRow(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper == nullptr ? -1 : looper->sync_target_row();
}

int MixerModel::looperSyncTargetSlot(int row, int slot) const {
  auto* looper = dynamic_cast<LooperInstance*>(insertFor(row, slot));
  return looper == nullptr ? -1 : looper->sync_target_slot();
}

QVariantList MixerModel::looperSyncCandidates(int row, int slot) const {
  QVariantList out;
  for (int r = 0; r < static_cast<int>(channels_.size()); ++r) {
    ChannelStrip* strip = const_cast<MixerModel*>(this)->stripFor(r);
    if (strip == nullptr) continue;
    // Two passes: a channel with two Loopers needs its slot said out loud,
    // one with a single Looper reads better without the extra number.
    int looper_count = 0;
    for (size_t s = 0; s < strip->insert_count(); ++s)
      if (dynamic_cast<LooperInstance*>(strip->insert_at(s)) != nullptr)
        ++looper_count;
    for (size_t s = 0; s < strip->insert_count(); ++s) {
      if (r == row && static_cast<int>(s) == slot) continue;
      auto* looper = dynamic_cast<LooperInstance*>(strip->insert_at(s));
      if (looper == nullptr) continue;
      QVariantMap entry;
      entry.insert(QStringLiteral("row"), r);
      entry.insert(QStringLiteral("slot"), static_cast<int>(s));
      const QString& name = channels_[static_cast<size_t>(r)].name;
      entry.insert(QStringLiteral("label"),
                   looper_count > 1
                       ? QStringLiteral("%1 (slot %2)").arg(name).arg(s + 1)
                       : name);
      out.append(entry);
    }
  }
  return out;
}

void MixerModel::toggleMasterDim() {
  engine_.graph().set_master_dim(!engine_.graph().master_dim());
  emit masterGainChanged();
}

void MixerModel::toggleMasterMute() {
  engine_.graph().set_master_mute(!engine_.graph().master_mute());
  emit masterGainChanged();
}

void MixerModel::toggleMasterMono() {
  engine_.graph().set_master_mono(!engine_.graph().master_mono());
  emit masterGainChanged();
}

void MixerModel::toggleMidiClock() {
  engine_.set_midi_clock(!engine_.midi_clock());
  emit transportChanged();
}

void MixerModel::toggleFollowMidiClock() {
  engine_.set_follow_midi_clock(!engine_.follow_midi_clock());
  emit transportChanged();
}

void MixerModel::setTimeSignature(int num, int den) {
  engine_.set_time_signature(num, den);
  emit transportChanged();
}

QString MixerModel::gainLabel(qreal gain) {
  if (gain <= 0.0) return QStringLiteral("-inf");
  const qreal db = 20.0 * std::log10(gain);
  return QString::number(db, 'f', 1);
}

}  // namespace nirbija
