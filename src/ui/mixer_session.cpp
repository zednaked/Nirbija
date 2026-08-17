// Session persistence for MixerModel. There is no save dialog and no file
// picker: the session is written continuously and restored on the next start,
// so closing the app and opening it again lands you where you left off.

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdio>

#include "mixer_model.h"

namespace nirbija {
namespace {

constexpr int kSessionVersion = 1;

QString format_name(PluginFormat format) {
  switch (format) {
    case PluginFormat::Lv2: return QStringLiteral("LV2");
    case PluginFormat::Clap: return QStringLiteral("CLAP");
    case PluginFormat::Vst3: return QStringLiteral("VST3");
    case PluginFormat::Internal: return QStringLiteral("Internal");
  }
  return {};
}

bool format_from_name(const QString& name, PluginFormat* format) {
  if (name == QLatin1String("LV2")) {
    *format = PluginFormat::Lv2;
  } else if (name == QLatin1String("CLAP")) {
    *format = PluginFormat::Clap;
  } else if (name == QLatin1String("VST3")) {
    *format = PluginFormat::Vst3;
  } else if (name == QLatin1String("Internal")) {
    *format = PluginFormat::Internal;
  } else {
    return false;
  }
  return true;
}

}  // namespace

QString MixerModel::sessionPath() {
  // Overridable so a test never writes over the session someone is using.
  const QByteArray override = qgetenv("NIRBIJA_SESSION");
  if (!override.isEmpty()) return QString::fromLocal8Bit(override);

  const QString dir =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  return dir + QStringLiteral("/session.json");
}

// Takes the session lock, or reports that someone else has it. Called once,
// before anything is loaded.
void MixerModel::claimSession() {
  const QString path = sessionPath() + QStringLiteral(".lock");
  QDir().mkpath(QFileInfo(path).absolutePath());

  const int fd = ::open(path.toLocal8Bit().constData(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) return;

  // Non-blocking: if another instance holds it, this one carries on read-only
  // rather than waiting for a mixer that may never close.
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return;
  }
  session_fd_ = fd;
}

void MixerModel::saveSession() const {
  if (!engine_.running()) return;
  // Another instance owns the file. Loading it was useful; writing it would
  // throw away whatever that instance is doing.
  if (session_fd_ < 0) return;
  auto* self = const_cast<MixerModel*>(this);
  writeSession(sessionPath());
  self->dirty_flag_ = false;
  emit self->dirtyChanged();
}

// Asking a plugin for its state is the one part of a save that cannot happen
// under a running process(), so it is the only part the graph is parked for.
// Parking for the whole save meant the master went silent for as long as a
// JSON build and a disk write took — once a second, while a fader was moving.
QVector<QVector<QByteArray>> MixerModel::collectInsertStates() const {
  QVector<QVector<QByteArray>> states;
  states.resize(static_cast<qsizetype>(channels_.size()));

  auto* self = const_cast<MixerModel*>(this);
  self->engine_.park_graph();
  for (size_t row = 0; row < channels_.size(); ++row) {
    ChannelStrip* strip = stripFor(static_cast<int>(row));
    if (strip == nullptr) continue;
    for (size_t slot = 0; slot < strip->insert_count(); ++slot) {
      PluginInstance* insert = strip->insert_at(slot);
      if (insert == nullptr) continue;  // a hole left by a removal
      const std::vector<uint8_t> blob = insert->save_state();
      states[static_cast<qsizetype>(row)].append(
          QByteArray(reinterpret_cast<const char*>(blob.data()),
                     static_cast<qsizetype>(blob.size())));
    }
  }
  self->engine_.unpark_graph();
  return states;
}

// One channel, exactly as the session stores it. Pulled out of the save loop
// so a single strip can be written to a file of its own in the same shape: a
// strip preset is a session holding one channel, which is what keeps the two
// from drifting apart as either grows.
QJsonObject MixerModel::writeChannel(const ChannelUi& channel, size_t row,
                                     const QVector<QByteArray>& row_states) const {

  QJsonObject entry;
  entry[QStringLiteral("name")] = channel.name;
  entry[QStringLiteral("isBus")] = channel.is_bus;
  entry[QStringLiteral("destination")] = channel.destination;
  if (channel.destination < 0) {
    entry[QStringLiteral("destinationKind")] = QStringLiteral("master");
  } else {
    for (const ChannelUi& candidate : channels_) {
      const int id = candidate.is_bus
                         ? static_cast<int>(candidate.slot)
                         : channel_destination(candidate.slot);
      if (id != channel.destination) continue;
      entry[QStringLiteral("destinationKind")] =
          candidate.is_bus ? QStringLiteral("bus") : QStringLiteral("channel");
      entry[QStringLiteral("destinationName")] = candidate.name;
      break;
    }
  }
  entry[QStringLiteral("width")] = channel.width;
  entry[QStringLiteral("gain")] = channel.gain;
  entry[QStringLiteral("pan")] = channel.pan;
  entry[QStringLiteral("muted")] = channel.muted;
  entry[QStringLiteral("soloed")] = channel.soloed;
  entry[QStringLiteral("armed")] = channel.armed;
  entry[QStringLiteral("midiMask")] = midiMask(static_cast<int>(row));
  entry[QStringLiteral("channelSink")] =
      QString::fromStdString(engine_.current_channel_sink(channel.slot));
  // By name like the destinations, since slots move between sessions.
  const int sidechain = sidechainRow(static_cast<int>(row));
  if (sidechain >= 0)
    entry[QStringLiteral("sidechainName")] = channels_[sidechain].name;

  // Ports are stored by name. A source that is gone when the session reopens
  // simply stays unconnected rather than blocking the load. A bus has none.
  if (!channel.is_bus) {
    entry[QStringLiteral("audioSource")] =
        QString::fromStdString(engine_.current_source(channel.slot, false));
    QJsonArray midi_sources;
    for (const std::string& source :
         engine_.current_sources(channel.slot, true))
      midi_sources.append(QString::fromStdString(source));
    entry[QStringLiteral("midiSources")] = midi_sources;
  }

  QJsonArray inserts;
  ChannelStrip* strip_ptr = stripFor(static_cast<int>(row));
  if (strip_ptr == nullptr) {
    return entry;  // no strip: the row is all there is to say
  }
  ChannelStrip& strip = *strip_ptr;
  qsizetype state_index = 0;
  for (size_t slot = 0; slot < strip.insert_count(); ++slot) {
    PluginInstance* insert = strip.insert_at(slot);
    if (insert == nullptr) continue;  // a hole left by a removal

    const PluginDescriptor& descriptor = insert->descriptor();
    QJsonObject saved;
    saved[QStringLiteral("format")] = format_name(descriptor.format);
    saved[QStringLiteral("uid")] = QString::fromStdString(descriptor.uid);
    saved[QStringLiteral("bypassed")] = strip.insert_bypassed(slot);
    saved[QStringLiteral("postFader")] = strip.insert_post_fader(slot);

    // The blob is whatever the plugin said its state was while the graph was
    // parked, stored verbatim.
    if (state_index < row_states.size()) {
      const QByteArray& bytes = row_states[state_index];
      if (!bytes.isEmpty())
        saved[QStringLiteral("state")] = QString::fromLatin1(bytes.toBase64());
    }
    ++state_index;
    inserts.append(saved);
  }
  entry[QStringLiteral("inserts")] = inserts;

  QJsonArray sends;
  for (const QVariant& value : channel.sends) {
    const QVariantMap send = value.toMap();
    QJsonObject saved;
    saved[QStringLiteral("bus")] = send.value(QStringLiteral("bus")).toInt();
    saved[QStringLiteral("busName")] = send.value(QStringLiteral("name")).toString();
    saved[QStringLiteral("level")] = send.value(QStringLiteral("level")).toDouble();
    sends.append(saved);
  }
  entry[QStringLiteral("sends")] = sends;
  entry[QStringLiteral("midiMaps")] = mapsJsonForRow(static_cast<int>(row));
  return entry;
}

QJsonArray MixerModel::mapsJsonForRow(int row) const {
  QJsonArray out;
  if (row < 0 || row >= static_cast<int>(channels_.size())) return out;
  const int graph = static_cast<int>(channels_[row].slot);
  const bool bus = channels_[row].is_bus;
  for (const MidiMapping& map : midi_maps_) {
    if (map.graph_slot != graph || map.is_bus != bus) continue;
    QJsonObject saved;
    saved[QStringLiteral("cc")] = map.cc;
    saved[QStringLiteral("ch")] = map.midi_channel;
    saved[QStringLiteral("kind")] = static_cast<int>(map.kind);
    saved[QStringLiteral("slot")] = map.slot;
    saved[QStringLiteral("param")] = static_cast<int>(map.param);
    saved[QStringLiteral("min")] = map.min;
    saved[QStringLiteral("max")] = map.max;
    saved[QStringLiteral("toggle")] = map.toggle;
    out.append(saved);
  }
  return out;
}

void MixerModel::applyMapsJson(int row, const QJsonArray& maps) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  const int graph = static_cast<int>(channels_[row].slot);
  const bool bus = channels_[row].is_bus;
  for (const QJsonValue& value : maps) {
    const QJsonObject saved = value.toObject();
    MidiMapping map;
    map.cc = saved[QStringLiteral("cc")].toInt(-1);
    map.midi_channel = saved[QStringLiteral("ch")].toInt(-1);
    map.kind = static_cast<MidiMapping::Kind>(saved[QStringLiteral("kind")].toInt(0));
    map.row = row;
    map.graph_slot = graph;
    map.is_bus = bus;
    map.slot = saved[QStringLiteral("slot")].toInt(-1);
    map.param = static_cast<uint32_t>(saved[QStringLiteral("param")].toInt(0));
    map.min = saved[QStringLiteral("min")].toDouble(0.0);
    map.max = saved[QStringLiteral("max")].toDouble(1.0);
    map.toggle = saved[QStringLiteral("toggle")].toBool(false);
    if (map.cc >= 0) midi_maps_.push_back(map);
  }
}

void MixerModel::writeSession(const QString& target) const {
  if (!engine_.running()) return;

  // Taken before anything else, and the graph is running again by the time the
  // JSON below is built.
  const QVector<QVector<QByteArray>> states = collectInsertStates();

  QJsonArray channels;
  for (size_t row = 0; row < channels_.size(); ++row) {
    const ChannelUi& channel = channels_[row];
    channels.append(
        writeChannel(channel, row, states[static_cast<qsizetype>(row)]));
  }

  QJsonObject master;
  master[QStringLiteral("gain")] = master_gain_;
  master[QStringLiteral("sink")] =
      QString::fromStdString(engine_.current_master_sink());
  master[QStringLiteral("dim")] = masterDim();
  master[QStringLiteral("mute")] = masterMute();
  master[QStringLiteral("mono")] = masterMono();

  QJsonObject root;
  root[QStringLiteral("version")] = kSessionVersion;
  root[QStringLiteral("tempo")] = engine_.tempo();
  root[QStringLiteral("metronome")] = engine_.metronome();
  root[QStringLiteral("midiClock")] = engine_.midi_clock();
  root[QStringLiteral("followMidiClock")] = engine_.follow_midi_clock();
  root[QStringLiteral("timeNumerator")] = engine_.time_numerator();
  root[QStringLiteral("timeDenominator")] = engine_.time_denominator();

  QJsonArray maps;
  for (const MidiMapping& map : midi_maps_) {
    QJsonObject saved;
    saved[QStringLiteral("cc")] = map.cc;
    saved[QStringLiteral("ch")] = map.midi_channel;
    saved[QStringLiteral("kind")] = static_cast<int>(map.kind);
    saved[QStringLiteral("row")] = map.row;
    saved[QStringLiteral("graphSlot")] = map.graph_slot;
    saved[QStringLiteral("bus")] = map.is_bus;
    saved[QStringLiteral("slot")] = map.slot;
    saved[QStringLiteral("param")] = static_cast<int>(map.param);
    saved[QStringLiteral("min")] = map.min;
    saved[QStringLiteral("max")] = map.max;
    saved[QStringLiteral("toggle")] = map.toggle;
    maps.append(saved);
  }
  root[QStringLiteral("midiMaps")] = maps;
  root[QStringLiteral("master")] = master;
  root[QStringLiteral("channels")] = channels;

  const QString path = target;
  QDir().mkpath(QFileInfo(path).absolutePath());

  // Written to a temporary first, then renamed over the old file. Rename on
  // the same filesystem is atomic; removing the old file first is not.
  QFile file(path + QStringLiteral(".tmp"));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
  const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
  if (file.write(payload) != payload.size()) {
    qWarning("session: short write to %s (%lld bytes)",
             qPrintable(file.fileName()),
             static_cast<long long>(payload.size()));
    file.close();
    QFile::remove(file.fileName());
    return;
  }
  file.flush();
  file.close();

  // ::rename, not QFile::rename: the Qt one refuses when the destination
  // exists, so every save after the first quietly left the new session sitting
  // in the .tmp and the old one in place. The POSIX call replaces atomically,
  // which is the whole point of writing to a temporary first.
  const QByteArray from = (path + QStringLiteral(".tmp")).toLocal8Bit();
  const QByteArray to = path.toLocal8Bit();
  if (::rename(from.constData(), to.constData()) != 0) {
    qWarning("session: could not replace %s", to.constData());
    QFile::remove(file.fileName());
  }
}

void MixerModel::loadSession() { readSession(sessionPath()); }

bool MixerModel::saveSessionAs(const QUrl& file) {
  const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
  if (path.isEmpty()) return false;
  // writeSession parks for the plugin state and nothing else.
  writeSession(path);
  return QFile::exists(path);
}

bool MixerModel::loadSessionFrom(const QUrl& file) {
  const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
  if (path.isEmpty()) return false;

  // Validate before wiping the live mixer. A bad file must not become the
  // autosave.
  QFile probe(path);
  if (!probe.open(QIODevice::ReadOnly)) {
    emit errorOccurred(tr("Could not open session"));
    return false;
  }
  const QJsonDocument document = QJsonDocument::fromJson(probe.readAll());
  probe.close();
  if (!document.isObject() ||
      document.object()[QStringLiteral("version")].toInt() != kSessionVersion) {
    emit errorOccurred(tr("Session file is not a Nirbija session"));
    return false;
  }

  newSession();
  if (!readSession(path)) {
    emit errorOccurred(tr("Session could not be restored"));
    return false;
  }
  markDirty();
  return true;
}

// Rebuilds one channel from the session's own JSON and returns its row, or -1
// if it could not be made. A plugin that is no longer installed is named in
// `missing` and skipped rather than taking the strip down with it: the rest
// is still worth having, and whoever opened the file deserves to be told
// which one is gone by name.
//
// Shared with strip presets, which are single-channel files in this format.
int MixerModel::restoreChannel(const QJsonObject& entry, QStringList* missing) {

  const int width = entry[QStringLiteral("width")].toInt(2);
  const bool is_bus = entry[QStringLiteral("isBus")].toBool();
  const QString name = entry[QStringLiteral("name")].toString();

  // Buses are added in the same pass, and they keep their order because a bus
  // may only feed one that comes after it.
  if (is_bus) {
    addBus(name);
  } else {
    addChannel(name, width);
  }
  const int row = rowCount() - 1;
  if (row < 0) return -1;  // the graph is full

  setGain(row, entry[QStringLiteral("gain")].toDouble(1.0));
  setPan(row, entry[QStringLiteral("pan")].toDouble(0.0));
  if (entry[QStringLiteral("muted")].toBool()) toggleMute(row);
  if (entry[QStringLiteral("soloed")].toBool()) toggleSolo(row);
  if (entry[QStringLiteral("armed")].toBool()) toggleArm(row);
  setMidiMask(row, entry[QStringLiteral("midiMask")].toInt(0xFFFF));
  const QString channel_sink = entry[QStringLiteral("channelSink")].toString();
  if (!channel_sink.isEmpty()) connectChannelSink(row, channel_sink);

  // Applied after every row exists, further down, since a destination can
  // name a bus that has not been created yet.
  const QString audio = entry[QStringLiteral("audioSource")].toString();
  if (!audio.isEmpty()) connectSource(row, audio, false);
  // Newer sessions carry every source; older ones a single string.
  const QJsonArray midi_sources = entry[QStringLiteral("midiSources")].toArray();
  for (const QJsonValue& source : midi_sources)
    setMidiLink(row, source.toString(), true);
  const QString midi = entry[QStringLiteral("midiSource")].toString();
  if (!midi.isEmpty()) connectSource(row, midi, true);

  int insert_slot = -1;
  for (const QJsonValue& slot : entry[QStringLiteral("inserts")].toArray()) {
    const QJsonObject saved = slot.toObject();

    PluginFormat format = PluginFormat::Lv2;
    if (!format_from_name(saved[QStringLiteral("format")].toString(), &format))
      continue;

    const std::string uid =
        saved[QStringLiteral("uid")].toString().toStdString();
    const int plugin_row = plugins_->rowFor(format, uid);
    if (plugin_row < 0) {
      // The plugin was uninstalled since the session was written. Skipping it
      // keeps the rest of the channel rather than losing the whole session.
      qWarning("session: plugin no longer installed: %s", uid.c_str());
      if (missing != nullptr) missing->append(QString::fromStdString(uid));
      continue;
    }
    // The slot the engine chose, not "the last one": add_insert fills the
    // first hole in the chain, so the two are not the same thing.
    insert_slot = placeInsert(row, plugin_row, -1);
    if (insert_slot < 0) continue;

    setInsertBypassed(row, insert_slot,
                      saved[QStringLiteral("bypassed")].toBool());
    setInsertPostFader(row, insert_slot,
                       saved[QStringLiteral("postFader")].toBool());

    const QString state = saved[QStringLiteral("state")].toString();
    if (state.isEmpty()) continue;

    const QByteArray bytes = QByteArray::fromBase64(state.toLatin1());
    // Through stripFor, which knows a bus from a channel: reaching into the
    // channel list with a bus slot lands on whatever channel shares the
    // number — or, after a load has cleared the old session, on a null
    // pointer, which is exactly the crash loading a session used to be.
    ChannelStrip* strip = stripFor(row);
    if (strip == nullptr) continue;
    PluginInstance* insert = strip->insert_at(static_cast<size_t>(insert_slot));
    if (insert == nullptr) continue;

    const std::vector<uint8_t> blob(bytes.begin(), bytes.end());
    if (!insert->load_state(blob))
      qWarning("session: %s refused its own saved state", uid.c_str());
  }
  return row;
}

// --- one strip, on its own -----------------------------------------------
//
// A strip you liked - a goth drum kit, a bass and its arpeggio - written to a
// file you can keep and pass on. The format is a session with a single channel
// in it, so the same writer and the same reader serve both, and a preset opens
// in a version of the mixer that has grown since it was written.

// The half of a channel that can only be wired once every row exists: the
// output destination, the sends and the sidechain all name another strip, and
// a name means nothing until the strip wearing it is there.
void MixerModel::restoreChannelLinks(int row, const QJsonObject& entry) {

  const QString dest_kind =
      entry[QStringLiteral("destinationKind")].toString();
  const QString dest_name =
      entry[QStringLiteral("destinationName")].toString();
  int destination = entry[QStringLiteral("destination")].toInt(-1);
  if (dest_kind == QLatin1String("master") ||
      (dest_kind.isEmpty() && destination < 0)) {
    destination = -1;
  } else if (!dest_name.isEmpty()) {
    for (const ChannelUi& candidate : channels_) {
      if (candidate.name != dest_name) continue;
      if (dest_kind == QLatin1String("bus") && !candidate.is_bus) continue;
      if (dest_kind == QLatin1String("channel") && candidate.is_bus) continue;
      destination = candidate.is_bus
                        ? static_cast<int>(candidate.slot)
                        : channel_destination(candidate.slot);
      break;
    }
  }
  if (destination >= 0 || dest_kind == QLatin1String("master"))
    setDestination(row, destination);

  int slot = 0;
  for (const QJsonValue& send : entry[QStringLiteral("sends")].toArray()) {
    const QJsonObject saved = send.toObject();
    int bus = saved[QStringLiteral("bus")].toInt(-1);
    const QString bus_name = saved[QStringLiteral("busName")].toString();
    if (!bus_name.isEmpty()) {
      for (const ChannelUi& candidate : channels_) {
        if (candidate.is_bus && candidate.name == bus_name) {
          bus = static_cast<int>(candidate.slot);
          break;
        }
      }
    }
    setSend(row, slot++, bus, saved[QStringLiteral("level")].toDouble(0.0));
  }

  // Like the destinations, the key input is stored by name and resolved once
  // every row exists.
  const QString sidechain = entry[QStringLiteral("sidechainName")].toString();
  if (!sidechain.isEmpty()) {
    for (int i = 0; i < static_cast<int>(channels_.size()); ++i) {
      if (channels_[i].name != sidechain) continue;
      setSidechain(row, i);
      break;
    }
  }
}

bool MixerModel::saveChannelTo(int row, const QUrl& file) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return false;
  const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
  if (path.isEmpty()) return false;

  // Every plugin's state, read with the graph parked, exactly as a session save
  // does and for the reason the LV2 spec gives.
  const QVector<QVector<QByteArray>> states = collectInsertStates();

  QJsonArray channels;
  channels.append(writeChannel(channels_[row], static_cast<size_t>(row),
                               states[static_cast<qsizetype>(row)]));

  QJsonObject root;
  root[QStringLiteral("version")] = kSessionVersion;
  root[QStringLiteral("kind")] = QStringLiteral("strip");
  root[QStringLiteral("channels")] = channels;

  QFile out(path);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    emit errorOccurred(tr("Could not write %1").arg(QFileInfo(path).fileName()));
    return false;
  }
  out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  out.close();
  return true;
}

bool MixerModel::loadChannelFrom(const QUrl& file) {
  const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
  if (path.isEmpty()) return false;

  QFile in(path);
  if (!in.open(QIODevice::ReadOnly)) {
    emit errorOccurred(tr("Could not open %1").arg(QFileInfo(path).fileName()));
    return false;
  }
  const QJsonDocument document = QJsonDocument::fromJson(in.readAll());
  in.close();

  if (!document.isObject() ||
      document.object()[QStringLiteral("version")].toInt() != kSessionVersion) {
    emit errorOccurred(tr("%1 is not a Nirbija strip").arg(QFileInfo(path).fileName()));
    return false;
  }
  const QJsonArray channels =
      document.object()[QStringLiteral("channels")].toArray();
  if (channels.isEmpty()) {
    emit errorOccurred(tr("%1 holds no strip").arg(QFileInfo(path).fileName()));
    return false;
  }

  pushUndo();
  const QJsonObject entry = channels.first().toObject();

  QStringList missing;
  engine_.park_graph();
  const int row = restoreChannel(entry, &missing);
  engine_.unpark_graph();
  if (row < 0) {
    emit errorOccurred(tr("There is no room for another strip"));
    return false;
  }

  // Sends and the destination name buses, which a preset carries no copies of.
  // Whatever matches by name in this session is wired up; the rest is left
  // alone rather than pointing somewhere arbitrary.
  restoreChannelLinks(row, entry);
  applyMapsJson(row, entry[QStringLiteral("midiMaps")].toArray());
  if (!midi_maps_.empty()) engine_.connect_all_midi_to_control();
  markDirty();

  // A plugin the sender had and the receiver does not is the ordinary case for
  // a file that travelled, not a broken file: the strip arrives with a hole in
  // it and the hole is named.
  if (!missing.isEmpty()) {
    emit errorOccurred(
        tr("%1 loaded without %2, which is not installed here")
            .arg(entry[QStringLiteral("name")].toString(), missing.join(", ")));
  }
  return true;
}

bool MixerModel::readSession(const QString& target) {
  if (!engine_.running()) return false;
  engine_.park_graph();

  QFile file(target);
  if (!file.open(QIODevice::ReadOnly)) {
    engine_.unpark_graph();
    return false;
  }

  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  file.close();
  if (!document.isObject()) {
    engine_.unpark_graph();
    return false;
  }

  const QJsonObject root = document.object();
  if (root[QStringLiteral("version")].toInt() != kSessionVersion) {
    engine_.unpark_graph();
    return false;
  }

  // Restoring drives the same setters the UI does, and each of those would
  // otherwise queue a save of what is only half restored.
  restoring_ = true;

  const QJsonArray channels = root[QStringLiteral("channels")].toArray();
  for (const QJsonValue& value : channels) {
      restoreChannel(value.toObject(), nullptr);
  }

  // Destinations and sends last: both can name a bus that appears later in the
  // list, and only now is every row in place.
  int row = 0;
  for (const QJsonValue& value : channels) {
    restoreChannelLinks(row, value.toObject());
    ++row;
  }

  const double tempo = root[QStringLiteral("tempo")].toDouble(120.0);
  if (tempo > 0.0) setTempo(tempo);
  if (root[QStringLiteral("metronome")].toBool() != engine_.metronome())
    toggleMetronome();
  if (root[QStringLiteral("midiClock")].toBool() != engine_.midi_clock())
    toggleMidiClock();
  if (root[QStringLiteral("followMidiClock")].toBool() != engine_.follow_midi_clock())
    toggleFollowMidiClock();
  setTimeSignature(root[QStringLiteral("timeNumerator")].toInt(4),
                   root[QStringLiteral("timeDenominator")].toInt(4));

  midi_maps_.clear();
  bool from_channels = false;
  int map_row = 0;
  for (const QJsonValue& value : channels) {
    const QJsonObject entry = value.toObject();
    if (entry.contains(QStringLiteral("midiMaps"))) {
      from_channels = true;
      applyMapsJson(map_row, entry[QStringLiteral("midiMaps")].toArray());
    }
    ++map_row;
  }
  // Older sessions kept maps at the root, keyed by a graph slot that is
  // issued fresh on every launch. Rebind them to the row they named.
  if (!from_channels) {
    for (const QJsonValue& value : root[QStringLiteral("midiMaps")].toArray()) {
      const QJsonObject saved = value.toObject();
      MidiMapping map;
      map.cc = saved[QStringLiteral("cc")].toInt(-1);
      map.midi_channel = saved[QStringLiteral("ch")].toInt(-1);
      map.kind =
          static_cast<MidiMapping::Kind>(saved[QStringLiteral("kind")].toInt(0));
      map.row = saved[QStringLiteral("row")].toInt(-1);
      map.slot = saved[QStringLiteral("slot")].toInt(-1);
      map.param = static_cast<uint32_t>(saved[QStringLiteral("param")].toInt(0));
      map.min = saved[QStringLiteral("min")].toDouble(0.0);
      map.max = saved[QStringLiteral("max")].toDouble(1.0);
      map.toggle = saved[QStringLiteral("toggle")].toBool(false);
      if (map.row >= 0 && map.row < static_cast<int>(channels_.size())) {
        map.graph_slot = static_cast<int>(channels_[map.row].slot);
        map.is_bus = channels_[map.row].is_bus;
      } else {
        map.graph_slot = saved[QStringLiteral("graphSlot")].toInt(-1);
        map.is_bus = saved[QStringLiteral("bus")].toBool();
      }
      if (map.cc >= 0) midi_maps_.push_back(map);
    }
  }
  if (!midi_maps_.empty()) engine_.connect_all_midi_to_control();

  const QJsonObject master = root[QStringLiteral("master")].toObject();
  setMasterGain(master[QStringLiteral("gain")].toDouble(1.0));
  const QString sink = master[QStringLiteral("sink")].toString();
  if (!sink.isEmpty()) connectMaster(sink);
  if (master[QStringLiteral("dim")].toBool() != masterDim()) toggleMasterDim();
  if (master[QStringLiteral("mute")].toBool() != masterMute()) toggleMasterMute();
  if (master[QStringLiteral("mono")].toBool() != masterMono()) toggleMasterMono();

  restoring_ = false;
  engine_.unpark_graph();
  return true;
}

void MixerModel::markDirty(bool schedule_save) {
  if (restoring_) return;
  if (!dirty_flag_) {
    dirty_flag_ = true;
    emit dirtyChanged();
  }
  if (schedule_save) autosave_timer_.start();
}

}  // namespace nirbija
