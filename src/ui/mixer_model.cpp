#include "mixer_model.h"

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

  loadSession();
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
      {AccentRole, "accent"},
  };
}

void MixerModel::addChannel(const QString& name, int channels) {
  const QString label = name.isEmpty()
                            ? tr("Channel %1").arg(channels_.size() + 1)
                            : name;
  const size_t index = engine_.add_channel(label.toStdString(), channels);
  if (index == kMaxChannels) return;

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
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {NameRole});
  markDirty();
}

void MixerModel::post(EngineCommand::Kind kind, int row, float value) {
  EngineCommand command;
  command.kind = kind;
  command.channel = channels_[row].slot;
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
  // Arming has no effect until the recorder lands; the button is here so the
  // strip layout is right, and it already tracks per-channel state.
  channels_[row].armed = !channels_[row].armed;
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
  if (!engine_.graph().channel(channels_[row].slot).add_insert(std::move(instance)))
    return false;

  channels_[row].inserts.append(QString::fromStdString(descriptor->name));
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {InsertsRole});
  markDirty();
  return true;
}

void MixerModel::removeInsert(int row, int slot) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  if (slot < 0 || slot >= channels_[row].inserts.size()) return;

  engine_.graph().channel(channels_[row].slot).remove_insert(static_cast<size_t>(slot));
  // The engine leaves a hole so the surviving indices stay put; the label list
  // has to keep the same shape or the two would drift apart.
  channels_[row].inserts[slot].clear();
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {InsertsRole});
  markDirty();
}

bool MixerModel::openInsertEditor(int row, int slot) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return false;

  PluginInstance* insert = engine_.graph().channel(channels_[row].slot).insert_at(
      static_cast<size_t>(slot));
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

void MixerModel::pollLevels() {
  if (!engine_.running()) return;

  AudioGraph& graph = engine_.graph();
  for (size_t row = 0; row < channels_.size(); ++row) {
    ChannelUi& channel = channels_[row];
    if (!graph.channel_alive(channel.slot)) continue;

    ChannelStrip& strip = graph.channel(channel.slot);
    for (int ch = 0; ch < channel.width; ++ch)
      channel.peak[ch] = strip.read_peak(ch);
    if (channel.width == 1) channel.peak[1] = channel.peak[0];
  }
  if (!channels_.empty()) {
    emit dataChanged(index(0), index(static_cast<int>(channels_.size()) - 1),
                     {PeakLeftRole, PeakRightRole});
  }

  master_peak_[0] = graph.read_master_peak(0);
  master_peak_[1] = graph.read_master_peak(1);
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
