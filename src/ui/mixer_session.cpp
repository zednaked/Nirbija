// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
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
#include <vector>

#include "mixer_model.h"
#include "core/sampler.h"

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

// The timer's end of the autosave. It never parks: a looper with Rec down
// says its state cannot be read under process(), and the old answer - fade
// the master out for a block - put a hole in the music one second into
// every take. Now the save simply waits and asks again a second later; Rec
// comes up, the next check passes, the session is written. Only an explicit
// save or quitting may park, and quitting can wait.
void MixerModel::autosave() {
  if (!dirty_flag_) return;
  if (anyInsertNeedsQuietSave()) {
    autosave_timer_.start();
    return;
  }
  saveSession();
}

// Every plugin's state, read with process() running. Every hosted format
// allows that - LV2's save() may run alongside run() and the plugin locks for
// itself, CLAP and VST3 save on the main thread with the plugin active - and
// the built-in plugins read atomics. Only a plugin that says it needs quiet
// right now (a looper mid-take, a sampler with a take waiting) parks the
// graph, and then only for this save. Anything else would put a hole in the
// master a second after every edit to a sequencer grid, which is how the
// autosave used to sound.
bool MixerModel::anyInsertNeedsQuietSave() const {
  for (size_t row = 0; row < channels_.size(); ++row) {
    ChannelStrip* strip = stripFor(static_cast<int>(row));
    if (strip == nullptr) continue;
    for (size_t slot = 0; slot < strip->insert_count(); ++slot) {
      PluginInstance* insert = strip->insert_at(slot);
      if (insert != nullptr && insert->save_needs_quiet()) return true;
    }
  }
  return false;
}

QVector<QVector<MixerModel::InsertState>> MixerModel::collectInsertStates(
    bool allow_park) const {
  QVector<QVector<InsertState>> states;
  states.resize(static_cast<qsizetype>(channels_.size()));

  auto* self = const_cast<MixerModel*>(this);
  const bool park = allow_park && anyInsertNeedsQuietSave();
  const bool quiet = park && self->engine_.park_graph();
  for (size_t row = 0; row < channels_.size(); ++row) {
    ChannelStrip* strip = stripFor(static_cast<int>(row));
    if (strip == nullptr) continue;
    for (size_t slot = 0; slot < strip->insert_count(); ++slot) {
      PluginInstance* insert = strip->insert_at(slot);
      if (insert == nullptr) continue;  // a hole left by a removal
      InsertState state;
      // Read only when that is safe: with the graph parked, or from a
      // plugin that never minds. The rest are marked, not guessed at.
      if (quiet || !insert->save_needs_quiet()) {
        const std::vector<uint8_t> blob = insert->save_state();
        state.blob = QByteArray(reinterpret_cast<const char*>(blob.data()),
                                static_cast<qsizetype>(blob.size()));
      } else {
        state.skipped = true;
      }
      states[static_cast<qsizetype>(row)].append(std::move(state));
    }
  }
  if (park) self->engine_.unpark_graph();
  return states;
}

// One channel, exactly as the session stores it. Pulled out of the save loop
// so a single strip can be written to a file of its own in the same shape: a
// strip preset is a session holding one channel, which is what keeps the two
// from drifting apart as either grows.
QJsonObject MixerModel::writeChannel(const ChannelUi& channel, size_t row,
                                     const QVector<InsertState>& row_states) const {

  QJsonObject entry;
  entry[QStringLiteral("name")] = channel.name;
  entry[QStringLiteral("isBus")] = channel.is_bus;
  // Which graph strip this row is, for an undo snapshot taken and restored
  // within one run: slots are never reused, so it names the strip exactly
  // where a name or a position could not. Meaningless across a restart and
  // ignored by readSession.
  entry[QStringLiteral("graphSlot")] = static_cast<int>(channel.slot);
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

    // The blob is whatever the plugin said its state was, stored verbatim. A
    // skipped one (see collectInsertStates) is left out, and says so, rather
    // than written as "no state".
    if (state_index < row_states.size()) {
      const InsertState& state = row_states[state_index];
      if (state.skipped)
        saved[QStringLiteral("stateSkipped")] = true;
      else if (!state.blob.isEmpty())
        saved[QStringLiteral("state")] =
            QString::fromLatin1(state.blob.toBase64());
    }
    ++state_index;
    inserts.append(saved);
  }
  // The plugins this machine could not make, written back exactly as they
  // were read, so a session that travelled through a box without them still
  // has them - blob and all - when it comes home.
  for (const ChannelUi::MissingInsert& ghost : channel.missing) {
    QJsonObject saved;
    saved[QStringLiteral("format")] = ghost.format;
    saved[QStringLiteral("uid")] = ghost.uid;
    saved[QStringLiteral("bypassed")] = ghost.bypassed;
    saved[QStringLiteral("postFader")] = ghost.post_fader;
    if (!ghost.state.isEmpty()) saved[QStringLiteral("state")] = ghost.state;
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
    if (map.cc >= 0 && map.kind != MidiMapping::Kind::SamplerPadNote)
      midi_maps_.push_back(map);
  }
}

QJsonObject MixerModel::buildSession(bool allow_park) const {
  // Taken before anything else, and the graph is running again by the time the
  // JSON below is built.
  const QVector<QVector<InsertState>> states = collectInsertStates(allow_park);

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
  master[QStringLiteral("limiter")] = masterLimiter();

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
  return root;
}

void MixerModel::writeSession(const QString& target) const {
  if (!engine_.running()) return;
  // An explicit save: allowed to park for the one plugin that asks.
  writeJson(target, buildSession(true));
}

bool MixerModel::writeJson(const QString& path, const QJsonObject& root) {
  QDir().mkpath(QFileInfo(path).absolutePath());

  // Written to a temporary first, then renamed over the old file. Rename on
  // the same filesystem is atomic; removing the old file first is not.
  QFile file(path + QStringLiteral(".tmp"));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
  if (file.write(payload) != payload.size()) {
    qWarning("session: short write to %s (%lld bytes)",
             qPrintable(file.fileName()),
             static_cast<long long>(payload.size()));
    file.close();
    QFile::remove(file.fileName());
    return false;
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
    return false;
  }
  return true;
}

void MixerModel::loadSession() { readSession(sessionPath()); }

bool MixerModel::saveSessionAs(const QUrl& file) {
  const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
  if (path.isEmpty()) return false;
  // writeSession runs with the mixer playing; see collectInsertStates().
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

  // The autosave on disk is not touched here: the old flow called
  // newSession(), which wrote an empty session over it before the file was
  // even read, so a load that then failed had already destroyed the last
  // good state. The mixer is cleared without saving, and a copy of the
  // autosave is kept beside it; only the next autosave, of a session that
  // did load, replaces it.
  const QString autosave = sessionPath();
  if (QFile::exists(autosave)) {
    const QString backup = autosave + QStringLiteral(".bak");
    QFile::remove(backup);
    QFile::copy(autosave, backup);
  }

  pushUndo();
  clearMixer();
  if (!readSession(path)) {
    emit errorOccurred(tr("Session could not be restored"));
    // Back to what was playing a moment ago, from the file that still holds it.
    readSession(autosave);
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
int MixerModel::restoreChannel(const QJsonObject& entry, QStringList* missing,
                               const QString& sample_dir) {

  const int width = entry[QStringLiteral("width")].toInt(2);
  const bool is_bus = entry[QStringLiteral("isBus")].toBool();
  const QString name = entry[QStringLiteral("name")].toString();

  // Buses are added in the same pass, and they keep their order because a bus
  // may only feed one that comes after it.
  const int row = is_bus ? addBus(name) : addChannel(name, width);
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
      // The plugin was uninstalled since the session was written. Skipping
      // it keeps the rest of the channel rather than losing the whole
      // session; the entry itself is kept on the row so the slot can show
      // the gap and the next save writes it back untouched.
      qWarning("session: plugin no longer installed: %s", uid.c_str());
      if (missing != nullptr) missing->append(QString::fromStdString(uid));
      channels_[row].missing.append(
          {saved[QStringLiteral("format")].toString(),
           QString::fromStdString(uid), saved[QStringLiteral("state")].toString(),
           saved[QStringLiteral("bypassed")].toBool(),
           saved[QStringLiteral("postFader")].toBool()});
      continue;
    }

    // The state goes in before the plugin reaches the audio thread - see
    // placeInsert - so nothing here needs the graph parked.
    const QByteArray bytes =
        QByteArray::fromBase64(saved[QStringLiteral("state")].toString().toLatin1());
    const std::vector<uint8_t> blob(bytes.begin(), bytes.end());
    // The slot the engine chose, not "the last one": add_insert fills the
    // first hole in the chain, so the two are not the same thing.
    insert_slot = placeInsert(row, plugin_row, -1, &blob, sample_dir);
    if (insert_slot < 0) continue;

    setInsertBypassed(row, insert_slot,
                      saved[QStringLiteral("bypassed")].toBool());
    setInsertPostFader(row, insert_slot,
                       saved[QStringLiteral("postFader")].toBool());
  }
  refreshInsertDetails(row);
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

  // Every plugin's state, read the way an explicit session save reads them:
  // live, unless one plugin says it needs quiet.
  const QVector<QVector<InsertState>> states = collectInsertStates(true);

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

  // Nothing parked: each plugin is loaded with its state before the audio
  // thread can see it (see placeInsert), so a strip arrives mid-song without
  // a hole in the master.
  QStringList missing;
  const int row =
      restoreChannel(entry, &missing, QFileInfo(path).absolutePath());
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

// --- a sampler pack, on its own -------------------------------------------
//
// The pads without the rest of the strip, and without the instrument's own
// knobs: swap the kit under a sequencer without touching what it plays or
// resetting gain, quantize or count-in. save_pads()/load_pads() are the
// engine's own pads-only shape; the paths inside resolve the same way a
// strip preset's do — against the pack file's own folder, not the session's.

bool MixerModel::saveSamplerPackTo(int row, int slot, const QUrl& file) {
  const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
  if (path.isEmpty()) return false;
  auto* sampler = dynamic_cast<SamplerInstance*>(insertFor(row, slot));
  if (sampler == nullptr) return false;

  // A pack save publishes a take the same way save_state() does, and a take
  // waiting to be published wants the audio thread out of the pads. Parked
  // only when the sampler says so, like any other save.
  if (sampler->save_needs_quiet() && !engine_.park_graph()) {
    engine_.unpark_graph();
    emit errorOccurred(tr("The audio graph would not settle; try again"));
    return false;
  }
  const std::vector<uint8_t> blob = sampler->save_pads();
  engine_.unpark_graph();

  QJsonObject root;
  root[QStringLiteral("version")] = kSessionVersion;
  root[QStringLiteral("kind")] = QStringLiteral("samplerPack");
  root[QStringLiteral("state")] = QString::fromLatin1(
      QByteArray(reinterpret_cast<const char*>(blob.data()),
                 static_cast<qsizetype>(blob.size()))
          .toBase64());

  QFile out(path);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    emit errorOccurred(tr("Could not write %1").arg(QFileInfo(path).fileName()));
    return false;
  }
  out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  out.close();
  return true;
}

bool MixerModel::loadSamplerPackFrom(int row, int slot, const QUrl& file) {
  const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
  if (path.isEmpty()) return false;
  auto* sampler = dynamic_cast<SamplerInstance*>(insertFor(row, slot));
  if (sampler == nullptr) return false;

  QFile in(path);
  if (!in.open(QIODevice::ReadOnly)) {
    emit errorOccurred(tr("Could not open %1").arg(QFileInfo(path).fileName()));
    return false;
  }
  const QJsonDocument document = QJsonDocument::fromJson(in.readAll());
  in.close();
  if (!document.isObject() ||
      document.object()[QStringLiteral("kind")].toString() !=
          QLatin1String("samplerPack")) {
    emit errorOccurred(
        tr("%1 is not a Nirbija sampler pack").arg(QFileInfo(path).fileName()));
    return false;
  }

  const QByteArray bytes = QByteArray::fromBase64(
      document.object()[QStringLiteral("state")].toString().toLatin1());
  const std::vector<uint8_t> blob(bytes.begin(), bytes.end());

  if (!engine_.park_graph()) {
    engine_.unpark_graph();
    emit errorOccurred(tr("The audio graph would not settle; try again"));
    return false;
  }
  const bool ok = sampler->load_pads(blob);
  if (ok)
    sampler->resolve_paths(QFileInfo(path).absolutePath().toStdString());
  engine_.unpark_graph();
  if (!ok) {
    emit errorOccurred(
        tr("%1 refused its own saved state").arg(QFileInfo(path).fileName()));
    return false;
  }
  markDirty();
  return true;
}

// Reads a session file onto a mixer that is expected to be empty. Nothing is
// parked: every plugin gets its state before it is published (placeInsert),
// and everything else here is a setter the UI calls with the music running.
bool MixerModel::readSession(const QString& target) {
  if (!engine_.running()) return false;

  QFile file(target);
  if (!file.open(QIODevice::ReadOnly)) return false;

  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  file.close();
  if (!document.isObject()) return false;

  const QJsonObject root = document.object();
  if (root[QStringLiteral("version")].toInt() != kSessionVersion) return false;

  // Restoring drives the same setters the UI does, and each of those would
  // otherwise queue a save of what is only half restored.
  restoring_ = true;

  const QJsonArray channels = root[QStringLiteral("channels")].toArray();
  std::vector<int> rows;
  rows.reserve(static_cast<size_t>(channels.size()));
  const QString sample_dir = QFileInfo(target).absolutePath();
  QStringList missing;
  for (const QJsonValue& value : channels)
    rows.push_back(restoreChannel(value.toObject(), &missing, sample_dir));

  // Destinations and sends last: both can name a bus that appears later in the
  // list, and only now is every row in place. An entry that found no room
  // (the graph was already full) has no row to wire up.
  for (int i = 0; i < static_cast<int>(channels.size()); ++i) {
    if (rows[static_cast<size_t>(i)] < 0) continue;
    restoreChannelLinks(rows[static_cast<size_t>(i)], channels[i].toObject());
  }

  applySessionGlobals(root);

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
      if (map.cc >= 0 && map.kind != MidiMapping::Kind::SamplerPadNote)
        midi_maps_.push_back(map);
    }
  }
  if (!midi_maps_.empty()) engine_.connect_all_midi_to_control();

  restoring_ = false;

  // Named, not just logged: a strip that plays without its synth is the
  // first thing to explain when a session comes up wrong. The entries stay
  // on their rows, struck through, and the next save keeps them.
  if (!missing.isEmpty()) {
    missing.removeDuplicates();
    emit errorOccurred(
        tr("Not installed here, kept in the session: %1").arg(missing.join(", ")));
  }
  return true;
}

void MixerModel::applySessionGlobals(const QJsonObject& root) {
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

  const QJsonObject master = root[QStringLiteral("master")].toObject();
  setMasterGain(master[QStringLiteral("gain")].toDouble(1.0));
  const QString sink = master[QStringLiteral("sink")].toString();
  if (!sink.isEmpty()) connectMaster(sink);
  if (master[QStringLiteral("dim")].toBool() != masterDim()) toggleMasterDim();
  if (master[QStringLiteral("mute")].toBool() != masterMute()) toggleMasterMute();
  if (master[QStringLiteral("mono")].toBool() != masterMono()) toggleMasterMono();
  if (master[QStringLiteral("limiter")].toBool(true) != masterLimiter())
    toggleMasterLimiter();
}

// --- undo and redo -----------------------------------------------------------
//
// An undo used to write the snapshot to disk, throw every strip away, park
// the graph and read the file back - every plugin re-instantiated, a hole in
// the master, for "put that channel back". This brings the mixer to the
// snapshot by difference instead. Strips are matched by graph slot (never
// reused within a run) and inserts by plugin, so anything that is already
// where the snapshot wants it is left as the very same object; only what the
// snapshot lacks is removed, only what it adds is made - loaded with its
// state before the audio thread sees it, so nothing is parked.
//
// Deliberately not compared: a kept plugin's state blob. The undo stack
// records structural edits (strips, inserts, order), not knob turns, and a
// sequencer whose pattern was edited for ten minutes must not snap back
// because a channel added before that is being taken away.

namespace {

bool same_kind(const QJsonObject& entry, PluginFormat format,
               const std::string& uid) {
  PluginFormat saved_format = PluginFormat::Lv2;
  if (!format_from_name(entry[QStringLiteral("format")].toString(),
                        &saved_format))
    return false;
  return saved_format == format &&
         entry[QStringLiteral("uid")].toString().toStdString() == uid;
}

}  // namespace

void MixerModel::applySnapshot(const QJsonObject& root) {
  if (!engine_.running()) return;
  const QJsonArray entries = root[QStringLiteral("channels")].toArray();
  const int wanted = static_cast<int>(entries.size());
  const QString sample_dir = QFileInfo(sessionPath()).absolutePath();

  restoring_ = true;

  // 1. Which live row each wanted strip is. By graph slot first - the exact
  //    identity within a run - then by name and kind for a snapshot that
  //    came from somewhere else.
  std::vector<int> row_for(static_cast<size_t>(wanted), -1);
  std::vector<bool> used(channels_.size(), false);
  for (int j = 0; j < wanted; ++j) {
    const QJsonObject entry = entries[j].toObject();
    const int slot = entry[QStringLiteral("graphSlot")].toInt(-1);
    const bool is_bus = entry[QStringLiteral("isBus")].toBool();
    if (slot < 0) continue;
    for (size_t r = 0; r < channels_.size(); ++r) {
      if (used[r] || channels_[r].is_bus != is_bus ||
          static_cast<int>(channels_[r].slot) != slot)
        continue;
      row_for[static_cast<size_t>(j)] = static_cast<int>(r);
      used[r] = true;
      break;
    }
  }
  for (int j = 0; j < wanted; ++j) {
    if (row_for[static_cast<size_t>(j)] >= 0) continue;
    const QJsonObject entry = entries[j].toObject();
    const bool is_bus = entry[QStringLiteral("isBus")].toBool();
    const int width = entry[QStringLiteral("width")].toInt(2);
    const QString name = entry[QStringLiteral("name")].toString();
    for (size_t r = 0; r < channels_.size(); ++r) {
      if (used[r] || channels_[r].is_bus != is_bus ||
          channels_[r].width != width || channels_[r].name != name)
        continue;
      row_for[static_cast<size_t>(j)] = static_cast<int>(r);
      used[r] = true;
      break;
    }
  }

  // 2. Strips the snapshot has no place for go, highest row first so the
  //    lower indices stay meaningful. Remembered by identity across the
  //    removals, since rows shift.
  std::vector<std::pair<size_t, bool>> keep(static_cast<size_t>(wanted),
                                            {0, false});
  std::vector<bool> kept(static_cast<size_t>(wanted), false);
  for (int j = 0; j < wanted; ++j) {
    const int r = row_for[static_cast<size_t>(j)];
    if (r < 0) continue;
    keep[static_cast<size_t>(j)] = {channels_[r].slot, channels_[r].is_bus};
    kept[static_cast<size_t>(j)] = true;
  }
  for (int r = static_cast<int>(channels_.size()) - 1; r >= 0; --r)
    if (!used[static_cast<size_t>(r)]) removeChannel(r);

  auto find_row = [this](size_t slot, bool is_bus) {
    for (size_t r = 0; r < channels_.size(); ++r)
      if (channels_[r].slot == slot && channels_[r].is_bus == is_bus)
        return static_cast<int>(r);
    return -1;
  };

  // 3. Strips the snapshot has and the mixer does not are made whole, chain
  //    and all. Kept strips are brought up to date one setting at a time.
  QStringList missing;
  for (int j = 0; j < wanted; ++j) {
    const QJsonObject entry = entries[j].toObject();
    int row = -1;
    if (kept[static_cast<size_t>(j)]) {
      row = find_row(keep[static_cast<size_t>(j)].first,
                     keep[static_cast<size_t>(j)].second);
    }
    if (row < 0) {
      row = restoreChannel(entry, &missing, sample_dir);
      if (row < 0) continue;
      keep[static_cast<size_t>(j)] = {channels_[row].slot, channels_[row].is_bus};
      kept[static_cast<size_t>(j)] = true;
      continue;
    }

    ChannelUi& channel = channels_[row];
    const QString name = entry[QStringLiteral("name")].toString();
    if (!name.isEmpty() && name != channel.name) renameChannel(row, name);
    setGain(row, entry[QStringLiteral("gain")].toDouble(1.0));
    setPan(row, entry[QStringLiteral("pan")].toDouble(0.0));
    if (entry[QStringLiteral("muted")].toBool() != channel.muted) toggleMute(row);
    if (entry[QStringLiteral("soloed")].toBool() != channel.soloed) toggleSolo(row);
    if (entry[QStringLiteral("armed")].toBool() != channel.armed) toggleArm(row);
    setMidiMask(row, entry[QStringLiteral("midiMask")].toInt(0xFFFF));
    const QString sink = entry[QStringLiteral("channelSink")].toString();
    if (sink != QString::fromStdString(engine_.current_channel_sink(channel.slot)))
      connectChannelSink(row, sink);
    if (!channel.is_bus) {
      const QString audio = entry[QStringLiteral("audioSource")].toString();
      if (audio != QString::fromStdString(engine_.current_source(channel.slot, false)))
        connectSource(row, audio, false);
      QStringList wanted_midi;
      for (const QJsonValue& source : entry[QStringLiteral("midiSources")].toArray())
        wanted_midi.append(source.toString());
      QStringList current_midi;
      for (const std::string& source : engine_.current_sources(channel.slot, true))
        current_midi.append(QString::fromStdString(source));
      for (const QString& port : current_midi)
        if (!wanted_midi.contains(port)) setMidiLink(row, port, false);
      for (const QString& port : wanted_midi)
        if (!current_midi.contains(port)) setMidiLink(row, port, true);
    }

    // The chain. Each wanted insert claims the first live insert of the same
    // plugin not yet claimed; the unclaimed are removed, the unmatched made,
    // and the survivors swapped into the snapshot's order.
    ChannelStrip* strip = stripFor(row);
    if (strip == nullptr) continue;
    const QJsonArray saved_inserts = entry[QStringLiteral("inserts")].toArray();
    struct Wanted {
      QJsonObject saved;
      int plugin_row = -1;
      int slot = -1;
    };
    std::vector<Wanted> targets;
    channel.missing.clear();
    for (const QJsonValue& value : saved_inserts) {
      const QJsonObject saved = value.toObject();
      PluginFormat format = PluginFormat::Lv2;
      if (!format_from_name(saved[QStringLiteral("format")].toString(), &format))
        continue;
      const std::string uid = saved[QStringLiteral("uid")].toString().toStdString();
      const int plugin_row = plugins_->rowFor(format, uid);
      if (plugin_row < 0) {
        missing.append(QString::fromStdString(uid));
        channel.missing.append({saved[QStringLiteral("format")].toString(),
                                QString::fromStdString(uid),
                                saved[QStringLiteral("state")].toString(),
                                saved[QStringLiteral("bypassed")].toBool(),
                                saved[QStringLiteral("postFader")].toBool()});
        continue;
      }
      targets.push_back({saved, plugin_row, -1});
    }

    const size_t count = strip->insert_count();
    std::vector<bool> claimed(count, false);
    for (Wanted& target : targets) {
      for (size_t s = 0; s < count; ++s) {
        PluginInstance* live = strip->insert_at(s);
        if (claimed[s] || live == nullptr) continue;
        const PluginDescriptor& descriptor = live->descriptor();
        if (!same_kind(target.saved, descriptor.format, descriptor.uid)) continue;
        target.slot = static_cast<int>(s);
        claimed[s] = true;
        break;
      }
    }
    for (size_t s = 0; s < count; ++s)
      if (!claimed[s] && strip->insert_at(s) != nullptr)
        removeInsert(row, static_cast<int>(s));
    for (Wanted& target : targets) {
      if (target.slot >= 0) continue;
      const QByteArray bytes = QByteArray::fromBase64(
          target.saved[QStringLiteral("state")].toString().toLatin1());
      const std::vector<uint8_t> blob(bytes.begin(), bytes.end());
      target.slot = placeInsert(row, target.plugin_row, -1, &blob, sample_dir);
    }
    for (Wanted& target : targets) {
      if (target.slot < 0) continue;
      setInsertBypassed(row, target.slot,
                        target.saved[QStringLiteral("bypassed")].toBool());
      setInsertPostFader(row, target.slot,
                         target.saved[QStringLiteral("postFader")].toBool());
    }
    // Into order: after this, wanted insert j sits in slot j. Whatever was
    // in slot j (another wanted insert, or a hole) takes the vacated slot.
    QStringList& labels = channel.inserts;
    for (size_t j = 0; j < targets.size(); ++j) {
      const int from = targets[j].slot;
      if (from < 0 || from == static_cast<int>(j)) continue;
      if (static_cast<int>(j) >= labels.size() || from >= labels.size()) continue;
      for (Wanted& other : targets)
        if (other.slot == static_cast<int>(j)) other.slot = from;
      strip->swap_inserts(j, static_cast<size_t>(from));
      labels.swapItemsAt(static_cast<int>(j), from);
      targets[j].slot = static_cast<int>(j);
    }
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {InsertsRole});
    refreshInsertDetails(row);
    engine_.latency_changed();
  }

  // 4. The snapshot's order, by swaps: after step j, row j is right.
  for (int j = 0; j < wanted; ++j) {
    if (!kept[static_cast<size_t>(j)]) continue;
    const int at = find_row(keep[static_cast<size_t>(j)].first,
                            keep[static_cast<size_t>(j)].second);
    if (at >= 0 && at != j && j < static_cast<int>(channels_.size()))
      swapRows(j, at);
  }

  // 5. Everything that names another strip, once every strip is in place.
  //    Sends are rebuilt from nothing so a send the snapshot lacks goes.
  for (int j = 0; j < wanted; ++j) {
    if (!kept[static_cast<size_t>(j)]) continue;
    const int row = find_row(keep[static_cast<size_t>(j)].first,
                             keep[static_cast<size_t>(j)].second);
    if (row < 0) continue;
    if (ChannelStrip* strip = stripFor(row)) {
      for (size_t i = 0; i < kMaxSends; ++i) strip->set_send(i, -1, 0.0f);
    }
    channels_[row].sends.clear();
    restoreChannelLinks(row, entries[j].toObject());
  }

  midi_maps_.clear();
  for (int j = 0; j < wanted; ++j) {
    if (!kept[static_cast<size_t>(j)]) continue;
    const int row = find_row(keep[static_cast<size_t>(j)].first,
                             keep[static_cast<size_t>(j)].second);
    if (row >= 0)
      applyMapsJson(row, entries[j].toObject()[QStringLiteral("midiMaps")].toArray());
  }
  if (!midi_maps_.empty()) engine_.connect_all_midi_to_control();

  applySessionGlobals(root);

  // Editors of inserts that are gone close; every other window stays open on
  // the very same plugin it was editing.
  std::erase_if(editors_, [this](const OpenEditor& editor) {
    for (size_t r = 0; r < channels_.size(); ++r) {
      ChannelStrip* strip = stripFor(static_cast<int>(r));
      if (strip == nullptr) continue;
      for (size_t s = 0; s < strip->insert_count(); ++s)
        if (strip->insert_at(s) == editor.insert) return false;
    }
    return true;
  });

  restoring_ = false;
  if (!channels_.empty())
    emit dataChanged(index(0), index(static_cast<int>(channels_.size()) - 1));
  if (!missing.isEmpty()) {
    missing.removeDuplicates();
    emit errorOccurred(
        tr("Not installed here, kept in the session: %1").arg(missing.join(", ")));
  }
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
