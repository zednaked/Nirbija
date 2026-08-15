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

  QJsonArray channels;
  for (size_t row = 0; row < channels_.size(); ++row) {
    const ChannelUi& channel = channels_[row];

    QJsonObject entry;
    entry[QStringLiteral("name")] = channel.name;
    entry[QStringLiteral("isBus")] = channel.is_bus;
    entry[QStringLiteral("destination")] = channel.destination;
    entry[QStringLiteral("width")] = channel.width;
    entry[QStringLiteral("gain")] = channel.gain;
    entry[QStringLiteral("pan")] = channel.pan;
    entry[QStringLiteral("muted")] = channel.muted;
    entry[QStringLiteral("soloed")] = channel.soloed;

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
      channels.append(entry);
      continue;
    }
    ChannelStrip& strip = *strip_ptr;
    for (size_t slot = 0; slot < strip.insert_count(); ++slot) {
      PluginInstance* insert = strip.insert_at(slot);
      if (insert == nullptr) continue;  // a hole left by a removal

      const PluginDescriptor& descriptor = insert->descriptor();
      QJsonObject saved;
      saved[QStringLiteral("format")] = format_name(descriptor.format);
      saved[QStringLiteral("uid")] = QString::fromStdString(descriptor.uid);

      // The blob is whatever the plugin says its state is, stored verbatim.
      const std::vector<uint8_t> blob = insert->save_state();
      if (!blob.empty()) {
        const QByteArray bytes(reinterpret_cast<const char*>(blob.data()),
                               static_cast<qsizetype>(blob.size()));
        saved[QStringLiteral("state")] = QString::fromLatin1(bytes.toBase64());
      }
      inserts.append(saved);
    }
    entry[QStringLiteral("inserts")] = inserts;

    QJsonArray sends;
    for (const QVariant& value : channel.sends) {
      const QVariantMap send = value.toMap();
      QJsonObject saved;
      saved[QStringLiteral("bus")] = send.value(QStringLiteral("bus")).toInt();
      saved[QStringLiteral("level")] = send.value(QStringLiteral("level")).toDouble();
      sends.append(saved);
    }
    entry[QStringLiteral("sends")] = sends;
    channels.append(entry);
  }

  QJsonObject master;
  master[QStringLiteral("gain")] = master_gain_;
  master[QStringLiteral("sink")] =
      QString::fromStdString(engine_.current_master_sink());

  QJsonObject root;
  root[QStringLiteral("version")] = kSessionVersion;
  root[QStringLiteral("tempo")] = engine_.tempo();
  root[QStringLiteral("metronome")] = engine_.metronome();
  root[QStringLiteral("master")] = master;
  root[QStringLiteral("channels")] = channels;

  const QString path = sessionPath();
  QDir().mkpath(QFileInfo(path).absolutePath());

  // Written to a temporary first: a crash halfway through a save would
  // otherwise leave a truncated session that cannot be opened.
  QFile file(path + QStringLiteral(".tmp"));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
  file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  file.close();

  QFile::remove(path);
  file.rename(path);
}

void MixerModel::loadSession() {
  if (!engine_.running()) return;

  QFile file(sessionPath());
  if (!file.open(QIODevice::ReadOnly)) return;

  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  file.close();
  if (!document.isObject()) return;

  const QJsonObject root = document.object();
  if (root[QStringLiteral("version")].toInt() != kSessionVersion) return;

  // Restoring drives the same setters the UI does, and each of those would
  // otherwise queue a save of what is only half restored.
  restoring_ = true;

  const QJsonArray channels = root[QStringLiteral("channels")].toArray();
  for (const QJsonValue& value : channels) {
    const QJsonObject entry = value.toObject();

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
    if (row < 0) break;  // the graph is full

    setGain(row, entry[QStringLiteral("gain")].toDouble(1.0));
    setPan(row, entry[QStringLiteral("pan")].toDouble(0.0));
    if (entry[QStringLiteral("muted")].toBool()) toggleMute(row);
    if (entry[QStringLiteral("soloed")].toBool()) toggleSolo(row);

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
        continue;
      }
      if (!addInsert(row, plugin_row)) continue;

      const QString state = saved[QStringLiteral("state")].toString();
      if (state.isEmpty()) continue;

      const QByteArray bytes = QByteArray::fromBase64(state.toLatin1());
      ChannelStrip& strip = engine_.graph().channel(channels_[row].slot);
      PluginInstance* insert = strip.insert_at(strip.insert_count() - 1);
      if (insert == nullptr) continue;

      const std::vector<uint8_t> blob(bytes.begin(), bytes.end());
      if (!insert->load_state(blob))
        qWarning("session: %s refused its own saved state", uid.c_str());
    }
  }

  // Destinations and sends last: both can name a bus that appears later in the
  // list, and only now is every row in place.
  int row = 0;
  for (const QJsonValue& value : channels) {
    const QJsonObject entry = value.toObject();

    const int destination = entry[QStringLiteral("destination")].toInt(-1);
    if (destination >= 0) setDestination(row, destination);

    int slot = 0;
    for (const QJsonValue& send : entry[QStringLiteral("sends")].toArray()) {
      const QJsonObject saved = send.toObject();
      setSend(row, slot++, saved[QStringLiteral("bus")].toInt(-1),
              saved[QStringLiteral("level")].toDouble(0.0));
    }
    ++row;
  }

  const double tempo = root[QStringLiteral("tempo")].toDouble(120.0);
  if (tempo > 0.0) setTempo(tempo);
  if (root[QStringLiteral("metronome")].toBool() != engine_.metronome())
    toggleMetronome();

  const QJsonObject master = root[QStringLiteral("master")].toObject();
  setMasterGain(master[QStringLiteral("gain")].toDouble(1.0));
  const QString sink = master[QStringLiteral("sink")].toString();
  if (!sink.isEmpty()) connectMaster(sink);

  restoring_ = false;
}

void MixerModel::markDirty() {
  if (restoring_) return;
  autosave_timer_.start();
}

}  // namespace nirbija
