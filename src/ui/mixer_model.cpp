#include "mixer_model.h"

#include "core/file_player.h"

#include <QDir>
#include <QStandardPaths>
#include <QtMath>

#include <cmath>

namespace nirbija {
namespace {

// Channel accents, cycled as channels are added. AUM colours strips so a busy
// session stays readable at a glance.
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
  loadSession();

  if (!ownsSession()) {
    status_ += tr(" · session read-only (another Nirbija has it)");
    emit statusChanged();
  }
}

MixerModel::~MixerModel() {
  // The debounced save may still be pending, and closing the window is exactly
  // when the session matters most.
  saveSession();
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
    case InputLabelRole: return channel.input_label;
    case OutputLabelRole: return channel.output_label;
    case MidiLabelRole: return channel.midi_label;
    case InsertsRole: return channel.inserts;
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
      {InputLabelRole, "inputLabel"}, {OutputLabelRole, "outputLabel"},
      {MidiLabelRole, "midiLabel"},
      {InsertsRole, "inserts"},     {WidthRole, "channelWidth"},
      {AccentRole, "accent"},   {IsBusRole, "isBus"},
      {DestinationRole, "destination"}, {SendsRole, "sends"},
  };
}

void MixerModel::addChannel(const QString& name, int channels) {
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
  channel.accent = kAccents[static_cast<int>(index) % kAccents.size()];
  channels_.push_back(std::move(channel));
  endInsertRows();
  markDirty();
}

void MixerModel::removeChannel(int row) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;

  // Editors belonging to this channel go with it: one left open would be
  // editing a plugin that is no longer in the signal path.
  const size_t slot = channels_[row].slot;
  std::erase_if(editors_, [slot](const OpenEditor& editor) {
    return editor.slot == slot;
  });

  engine_.remove_channel(channels_[row].slot);

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
  bus.accent = kAccents[(static_cast<int>(index) + 3) % kAccents.size()];
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
  EngineCommand command;
  command.kind = kind;
  command.channel = channels_[row].slot;
  command.bus = channels_[row].is_bus;
  command.value = value;
  engine_.post(command);
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
  EngineCommand command;
  command.kind = EngineCommand::Kind::SetPlaying;
  command.value = engine_.playing() ? 0.0f : 1.0f;
  engine_.post(command);

  // The engine only applies this on its next block, so the property is read
  // back a moment later rather than assumed.
  QTimer::singleShot(50, this, [this] { emit transportChanged(); });
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
  if (take.isEmpty()) qWarning("recorder: could not start a take");
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
  EngineCommand command;
  command.kind = EngineCommand::Kind::SetMetronome;
  command.value = engine_.metronome() ? 0.0f : 1.0f;
  engine_.post(command);
  QTimer::singleShot(50, this, [this] { emit transportChanged(); });
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
  EngineCommand command;
  command.kind = EngineCommand::Kind::SetTempo;
  command.value = static_cast<float>(clamped);
  engine_.post(command);
  QTimer::singleShot(50, this, [this] { emit transportChanged(); });
  markDirty();
}

bool MixerModel::addInsert(int row, int pluginIndex) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return false;
  const PluginDescriptor* descriptor = plugins_->descriptor(pluginIndex);
  if (descriptor == nullptr) return false;

  // Instantiating and activating happen here, on the UI thread. The strip only
  // publishes the insert to the audio thread once it is ready to run.
  std::unique_ptr<PluginInstance> instance = plugins_->instantiate(pluginIndex);
  if (instance == nullptr) return false;
  ChannelStrip* strip = stripFor(row);
  size_t placed_at = 0;
  if (strip == nullptr || !strip->add_insert(std::move(instance), &placed_at))
    return false;

  // The label list mirrors the engine's slots, holes included, so the new
  // plugin's label lands exactly where the engine put the plugin.
  QStringList& labels = channels_[row].inserts;
  while (labels.size() <= static_cast<int>(placed_at)) labels.append(QString());
  labels[static_cast<int>(placed_at)] = QString::fromStdString(descriptor->name);
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {InsertsRole});
  markDirty();
  return true;
}

void MixerModel::removeInsert(int row, int slot) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  if (slot < 0 || slot >= channels_[row].inserts.size()) return;

  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr) return;
  strip->remove_insert(static_cast<size_t>(slot));
  // The engine leaves a hole so the surviving indices stay put; the label list
  // has to keep the same shape or the two would drift apart.
  channels_[row].inserts[slot].clear();
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {InsertsRole});
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
  emit dataChanged(idx, idx, {InsertsRole});
  markDirty();
}

bool MixerModel::openInsertEditor(int row, int slot) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return false;

  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr) return false;
  PluginInstance* insert = strip->insert_at(static_cast<size_t>(slot));
  if (insert == nullptr) return false;

  std::unique_ptr<PluginGui> gui = insert->create_gui();
  if (gui == nullptr) return false;

  auto window = std::make_unique<PluginWindow>(
      std::move(gui), QString::fromStdString(insert->descriptor().name));
  if (!window->open()) return false;

  editors_.push_back({channels_[row].slot, std::move(window)});
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
  const QString midi = shortPortName(
      QString::fromStdString(engine_.current_source(channel.slot, true)));

  channel.input_label = audio.isEmpty() ? tr("no input") : audio;
  channel.midi_label = midi.isEmpty() ? tr("no MIDI") : midi;

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

void MixerModel::pollLevels() {
  if (!engine_.running()) return;

  for (size_t row = 0; row < channels_.size(); ++row) {
    ChannelUi& channel = channels_[row];
    ChannelStrip* strip = stripFor(static_cast<int>(row));
    if (strip == nullptr) continue;

    for (int ch = 0; ch < channel.width; ++ch)
      channel.peak[ch] = strip->read_peak(ch);
    if (channel.width == 1) channel.peak[1] = channel.peak[0];
  }
  if (!channels_.empty()) {
    emit dataChanged(index(0), index(static_cast<int>(channels_.size()) - 1),
                     {PeakLeftRole, PeakRightRole});
  }

  master_peak_[0] = engine_.graph().read_master_peak(0);
  master_peak_[1] = engine_.graph().read_master_peak(1);
  emit levelsChanged();
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

QString MixerModel::gainLabel(qreal gain) {
  if (gain <= 0.0) return QStringLiteral("-inf");
  const qreal db = 20.0 * std::log10(gain);
  return QString::number(db, 'f', 1);
}

}  // namespace nirbija
