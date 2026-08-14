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

#include "mixer_model.h"

namespace nirbija {
namespace {

constexpr int kSessionVersion = 1;

QString format_name(PluginFormat format) {
  switch (format) {
    case PluginFormat::Lv2: return QStringLiteral("LV2");
    case PluginFormat::Clap: return QStringLiteral("CLAP");
    case PluginFormat::Vst3: return QStringLiteral("VST3");
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

void MixerModel::saveSession() const {
  if (!engine_.running()) return;

  QJsonArray channels;
  for (size_t row = 0; row < channels_.size(); ++row) {
    const ChannelUi& channel = channels_[row];

    QJsonObject entry;
    entry[QStringLiteral("name")] = channel.name;
    entry[QStringLiteral("width")] = channel.width;
    entry[QStringLiteral("gain")] = channel.gain;
    entry[QStringLiteral("pan")] = channel.pan;
    entry[QStringLiteral("muted")] = channel.muted;
    entry[QStringLiteral("soloed")] = channel.soloed;

    // Ports are stored by name. A source that is gone when the session reopens
    // simply stays unconnected rather than blocking the load.
    entry[QStringLiteral("audioSource")] =
        QString::fromStdString(engine_.current_source(channel.slot, false));
    entry[QStringLiteral("midiSource")] =
        QString::fromStdString(engine_.current_source(channel.slot, true));

    QJsonArray inserts;
    ChannelStrip& strip =
        const_cast<Engine&>(engine_).graph().channel(channel.slot);
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
    channels.append(entry);
  }

  QJsonObject master;
  master[QStringLiteral("gain")] = master_gain_;
  master[QStringLiteral("sink")] =
      QString::fromStdString(engine_.current_master_sink());

  QJsonObject root;
  root[QStringLiteral("version")] = kSessionVersion;
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
    addChannel(entry[QStringLiteral("name")].toString(), width);
    const int row = rowCount() - 1;
    if (row < 0) break;  // the graph is full

    setGain(row, entry[QStringLiteral("gain")].toDouble(1.0));
    setPan(row, entry[QStringLiteral("pan")].toDouble(0.0));
    if (entry[QStringLiteral("muted")].toBool()) toggleMute(row);
    if (entry[QStringLiteral("soloed")].toBool()) toggleSolo(row);

    const QString audio = entry[QStringLiteral("audioSource")].toString();
    if (!audio.isEmpty()) connectSource(row, audio, false);
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
