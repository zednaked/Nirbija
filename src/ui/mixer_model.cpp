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

  // 30 Hz is enough for a meter to look continuous and cheap enough that the
  // poll never competes with the audio thread.
  level_timer_.setInterval(33);
  connect(&level_timer_, &QTimer::timeout, this, &MixerModel::pollLevels);
  level_timer_.start();
}

MixerModel::~MixerModel() = default;

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
  channel.name = label;
  channel.width = channels;
  channel.input_label = channels == 1 ? tr("%1 in").arg(index + 1)
                                      : tr("%1 in L/R").arg(index + 1);
  channel.output_label = tr("Master");
  channel.accent = kAccents[static_cast<int>(index) % kAccents.size()];
  channels_.push_back(std::move(channel));
  endInsertRows();
}

void MixerModel::removeChannel(int row) {
  // TODO(phase-8): the engine keeps its channel slots for the lifetime of the
  // session, so a strip can be hidden here but its ports and DSP stay live.
  // Real removal needs the graph swap that session loading will require anyway.
  Q_UNUSED(row);
}

void MixerModel::renameChannel(int row, const QString& name) {
  if (row < 0 || row >= static_cast<int>(channels_.size()) || name.isEmpty()) return;
  channels_[row].name = name;
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {NameRole});
}

void MixerModel::post(EngineCommand::Kind kind, int row, float value) {
  EngineCommand command;
  command.kind = kind;
  command.channel = static_cast<size_t>(row);
  command.value = value;
  engine_.post(command);
}

void MixerModel::setGain(int row, qreal gain) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  channels_[row].gain = gain;
  post(EngineCommand::Kind::SetGain, row, static_cast<float>(gain));
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {GainRole});
}

void MixerModel::setPan(int row, qreal pan) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  channels_[row].pan = pan;
  post(EngineCommand::Kind::SetPan, row, static_cast<float>(pan));
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {PanRole});
}

void MixerModel::toggleMute(int row) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  channels_[row].muted = !channels_[row].muted;
  post(EngineCommand::Kind::SetMute, row, channels_[row].muted ? 1.0f : 0.0f);
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {MutedRole});
}

void MixerModel::toggleSolo(int row) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  channels_[row].soloed = !channels_[row].soloed;
  post(EngineCommand::Kind::SetSolo, row, channels_[row].soloed ? 1.0f : 0.0f);
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {SoloedRole});
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
}

bool MixerModel::addInsert(int row, int pluginIndex) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return false;
  const PluginDescriptor* descriptor = plugins_->descriptor(pluginIndex);
  if (descriptor == nullptr) return false;

  // Instantiating and activating happen here, on the UI thread. The strip only
  // publishes the insert to the audio thread once it is ready to run.
  std::unique_ptr<PluginInstance> instance = plugins_->instantiate(pluginIndex);
  if (instance == nullptr) return false;
  if (!engine_.graph().channel(row).add_insert(std::move(instance))) return false;

  channels_[row].inserts.append(QString::fromStdString(descriptor->name));
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {InsertsRole});
  return true;
}

void MixerModel::removeInsert(int row, int slot) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  if (slot < 0 || slot >= channels_[row].inserts.size()) return;

  engine_.graph().channel(row).remove_insert(static_cast<size_t>(slot));
  // The engine leaves a hole so the surviving indices stay put; the label list
  // has to keep the same shape or the two would drift apart.
  channels_[row].inserts[slot].clear();
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {InsertsRole});
}

bool MixerModel::openInsertEditor(int row, int slot) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return false;

  PluginInstance* insert = engine_.graph().channel(row).insert_at(
      static_cast<size_t>(slot));
  if (insert == nullptr) return false;

  std::unique_ptr<PluginGui> gui = insert->create_gui();
  if (gui == nullptr) return false;

  auto window = std::make_unique<PluginWindow>(
      std::move(gui), QString::fromStdString(insert->descriptor().name));
  if (!window->open()) return false;

  editors_.push_back(std::move(window));
  return true;
}

void MixerModel::pollLevels() {
  if (!engine_.running()) return;

  AudioGraph& graph = engine_.graph();
  const size_t count = std::min(graph.channel_count(), channels_.size());
  for (size_t i = 0; i < count; ++i) {
    ChannelStrip& strip = graph.channel(i);
    for (int ch = 0; ch < channels_[i].width; ++ch)
      channels_[i].peak[ch] = strip.read_peak(ch);
    if (channels_[i].width == 1) channels_[i].peak[1] = channels_[i].peak[0];
  }
  if (count > 0) {
    emit dataChanged(index(0), index(static_cast<int>(count) - 1),
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
