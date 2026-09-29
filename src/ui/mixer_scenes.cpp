// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
//
// Scenes, the UI's half: the list the ribbon draws, recording a scene by
// touching the strips, the hand that takes a control back from a scene, and
// the table the audio thread's conductor plays from. See design/scenes.md.

#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>

#include <cmath>

#include "mixer_model.h"

namespace nirbija {

namespace {

// The ribbon's colours, one per scene in turn: the same family of hues the
// editors give their parameters, so a scene looks like it belongs here.
constexpr qreal kSceneHues[] = {0.58, 0.08, 0.83, 0.47, 0.30, 0.72, 0.00, 0.13};

// Step Sequencer parameter ids (step_sequencer.cpp).
constexpr int kSeqPattern = 196;
constexpr int kSeqNextPattern = 197;

int bit_of(SceneTarget::What what) { return 1 << static_cast<int>(what); }

QString what_name(SceneTarget::What what) {
  switch (what) {
    case SceneTarget::What::Gate: return QStringLiteral("gate");
    case SceneTarget::What::Level: return QStringLiteral("level");
    case SceneTarget::What::Pattern: return QStringLiteral("pattern");
    case SceneTarget::What::Param: return QStringLiteral("param");
  }
  return {};
}

bool what_from_name(const QString& name, SceneTarget::What* out) {
  if (name == QLatin1String("gate")) *out = SceneTarget::What::Gate;
  else if (name == QLatin1String("level")) *out = SceneTarget::What::Level;
  else if (name == QLatin1String("pattern")) *out = SceneTarget::What::Pattern;
  else if (name == QLatin1String("param")) *out = SceneTarget::What::Param;
  else return false;
  return true;
}

// A chain tag to the insert's place among the live plugins, which is how the
// session lists them (holes left by removals are not written), and back.
int position_of_tag(const ChannelStrip* strip, uint32_t tag) {
  if (strip == nullptr || tag == 0) return -1;
  int position = 0;
  for (size_t slot = 0; slot < strip->insert_count(); ++slot) {
    if (strip->insert_at(slot) == nullptr) continue;
    if (strip->insert_tag(slot) == tag) return position;
    ++position;
  }
  return -1;
}

int slot_of_position(const ChannelStrip* strip, int position) {
  if (strip == nullptr || position < 0) return -1;
  for (size_t slot = 0; slot < strip->insert_count(); ++slot) {
    if (strip->insert_at(slot) == nullptr) continue;
    if (position-- == 0) return static_cast<int>(slot);
  }
  return -1;
}

}  // namespace

// --- identity ----------------------------------------------------------------

QString MixerModel::makeUid() {
  return QString::number(QRandomGenerator::global()->generate64(), 16)
      .rightJustified(16, QLatin1Char('0'));
}

int MixerModel::rowForUid(const QString& uid) const {
  if (uid.isEmpty()) return -1;
  for (size_t row = 0; row < channels_.size(); ++row)
    if (channels_[row].uid == uid) return static_cast<int>(row);
  return -1;
}

QString MixerModel::claimUid(const QString& wanted, int row) const {
  if (wanted.isEmpty()) return makeUid();
  const int holder = rowForUid(wanted);
  return holder < 0 || holder == row ? wanted : makeUid();
}

// --- the list ----------------------------------------------------------------

QVariantList MixerModel::scenes() const {
  QVariantList out;
  for (const SceneUi& scene : scenes_) {
    int lost = 0;
    for (const SceneUi::Target& target : scene.targets) {
      const int row = rowForUid(target.strip);
      if (row < 0) {
        ++lost;
        continue;
      }
      const bool on_plugin = target.what == SceneTarget::What::Pattern ||
                             target.what == SceneTarget::What::Param;
      if (on_plugin && position_of_tag(stripFor(row), target.insert_tag) < 0) ++lost;
    }
    out.append(QVariantMap{
        {QStringLiteral("name"), scene.name},
        {QStringLiteral("hue"), scene.hue},
        {QStringLiteral("bars"), scene.bars},
        {QStringLiteral("fade"), scene.fade},
        {QStringLiteral("count"), static_cast<int>(scene.targets.size())},
        {QStringLiteral("lost"), lost},
    });
  }
  return out;
}

int MixerModel::recordScene() const {
  if (scene_current_ >= 0 && scene_current_ < static_cast<int>(scenes_.size()))
    return scene_current_;
  if (scene_armed_ >= 0 && scene_armed_ < static_cast<int>(scenes_.size()))
    return scene_armed_;
  return scenes_.empty() ? -1 : 0;
}

void MixerModel::scenesEdited() {
  publishScenes();
  emit scenesChanged();
  emit sceneStateChanged();
  announceSceneMarks();
  markDirty();
}

int MixerModel::addScene() {
  pushUndo();
  SceneUi scene;
  const size_t index = scenes_.size();
  scene.name = tr("Scene %1").arg(index + 1);
  scene.hue = kSceneHues[index % std::size(kSceneHues)];
  scenes_.push_back(std::move(scene));
  scenesEdited();
  return static_cast<int>(index);
}

void MixerModel::removeScene(int scene) {
  if (scene < 0 || scene >= static_cast<int>(scenes_.size())) return;
  pushUndo();
  scenes_.erase(scenes_.begin() + scene);
  // The conductor counts by index: whatever it was playing past this one is
  // now one lower, and the one removed must not go on being current.
  if (scene_armed_ == scene) armScene(-1);
  scenesEdited();
}

void MixerModel::renameScene(int scene, const QString& name) {
  if (scene < 0 || scene >= static_cast<int>(scenes_.size()) || name.isEmpty()) return;
  pushUndo();
  scenes_[static_cast<size_t>(scene)].name = name;
  scenesEdited();
}

void MixerModel::setSceneBars(int scene, int bars) {
  if (scene < 0 || scene >= static_cast<int>(scenes_.size())) return;
  pushUndo();
  scenes_[static_cast<size_t>(scene)].bars = std::clamp(bars, 0, 999);
  scenesEdited();
}

void MixerModel::setSceneFade(int scene, int bars) {
  if (scene < 0 || scene >= static_cast<int>(scenes_.size())) return;
  pushUndo();
  scenes_[static_cast<size_t>(scene)].fade = std::clamp(bars, 0, 64);
  scenesEdited();
}

void MixerModel::moveScene(int scene, int direction) {
  const int other = scene + (direction < 0 ? -1 : 1);
  if (scene < 0 || other < 0 || scene >= static_cast<int>(scenes_.size()) ||
      other >= static_cast<int>(scenes_.size()))
    return;
  pushUndo();
  std::swap(scenes_[static_cast<size_t>(scene)], scenes_[static_cast<size_t>(other)]);
  scenesEdited();
}

void MixerModel::clearScene(int scene) {
  if (scene < 0 || scene >= static_cast<int>(scenes_.size())) return;
  pushUndo();
  scenes_[static_cast<size_t>(scene)].targets.clear();
  scenesEdited();
}

// --- playing -----------------------------------------------------------------

void MixerModel::armScene(int scene) {
  if (scene >= static_cast<int>(scenes_.size())) return;
  if (scene >= 0 && scene == scene_armed_) scene = -1;
  EngineCommand command;
  command.kind = EngineCommand::Kind::SceneArm;
  command.value = static_cast<float>(scene);
  if (!engine_.post(command)) {
    emit errorOccurred(tr("Audio thread is not keeping up"));
    return;
  }
  // Shown at once; the next poll says what the audio thread made of it.
  scene_armed_ = scene;
  emit sceneStateChanged();
}

void MixerModel::setSceneAuto(bool on) {
  if (on == sceneAuto()) return;
  engine_.scenes().set_auto(on);
  emit sceneStateChanged();
  markDirty();
}

void MixerModel::setSceneHold(bool on) {
  if (on == sceneHold()) return;
  engine_.scenes().set_hold(on);
  emit sceneStateChanged();
}

void MixerModel::setSceneRecording(bool on) {
  if (on == scene_recording_) return;
  // Recording with no scene yet makes the first one to record into.
  if (on && scenes_.empty()) addScene();
  scene_recording_ = on;
  emit sceneStateChanged();
  announceSceneMarks();
}

void MixerModel::toggleSceneOn(int row) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  ChannelUi& channel = channels_[static_cast<size_t>(row)];
  channel.scene_on = !channel.scene_on;
  sceneTouched(row, SceneTarget::What::Gate, channel.scene_on ? 1.0f : 0.0f);
  post(EngineCommand::Kind::SetSceneGate, row, channel.scene_on ? 1.0f : 0.0f);
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {SceneOnRole});
  markDirty();
}

void MixerModel::setFollowScenes(int row, bool on) {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return;
  ChannelUi& channel = channels_[static_cast<size_t>(row)];
  if (channel.follow_scenes == on) return;
  pushUndo();
  channel.follow_scenes = on;
  channel.scene_hands = 0;
  publishScenes();
  const QModelIndex idx = index(row);
  emit dataChanged(idx, idx, {FollowScenesRole, SceneMarksRole, SceneHandsRole});
  markDirty();
}

bool MixerModel::sceneParamAllowed(const PluginInstance* insert) {
  if (insert == nullptr) return false;
  const PluginDescriptor& descriptor = insert->descriptor();
  if (descriptor.format == PluginFormat::Lv2) return true;
  if (descriptor.format != PluginFormat::Internal) return false;
  // The built-ins whose parameters are settings. The sequencer, looper and
  // sampler have parameters that are buttons (clear, record, mutate): a
  // scene that pressed them again on every bar line would be a bug, and the
  // sequencer's part in a scene is its pattern. The script recompiles its
  // tables on the UI thread when a knob moves, which the audio thread cannot.
  static const char* const kAllowed[] = {"nirbija.fxpad", "nirbija.drone",
                                         "nirbija.arp", "nirbija.chord"};
  for (const char* uid : kAllowed)
    if (descriptor.uid == uid) return true;
  return false;
}

bool MixerModel::sceneParamStepped(const PluginInstance* insert, uint32_t id) {
  if (insert == nullptr || insert->descriptor().format != PluginFormat::Internal)
    return false;
  // Nothing says which parameters take whole values (design/scenes.md,
  // phase 3 reads it from the formats that know). For the built-ins a range
  // of whole numbers wider than 0..1 is a mode, a division or a note - and
  // a mode walked through its neighbours on the way is not a fade.
  for (const ParameterInfo& info : insert->parameters()) {
    if (info.id != id) continue;
    auto whole = [](double v) { return std::floor(v) == v; };
    return whole(info.min_value) && whole(info.max_value) &&
           whole(info.default_value) && info.max_value - info.min_value >= 2.0;
  }
  return false;
}

void MixerModel::sceneParamTouched(int row, int slot, uint32_t id, float value) {
  PluginInstance* insert = insertFor(row, slot);
  ChannelStrip* strip = stripFor(row);
  if (strip == nullptr || !sceneParamAllowed(insert)) return;
  sceneTouched(row, SceneTarget::What::Param, value,
               strip->insert_tag(static_cast<size_t>(slot)), id,
               sceneParamStepped(insert, id));
}

void MixerModel::sceneTouched(int row, SceneTarget::What what, float value,
                              uint32_t insert_tag, uint32_t param, bool stepped) {
  if (restoring_ || row < 0 || row >= static_cast<int>(channels_.size())) return;
  ChannelUi& channel = channels_[static_cast<size_t>(row)];
  if (!channel.follow_scenes) return;

  if (scene_recording_) {
    const int into = recordScene();
    if (into < 0) return;
    SceneUi& scene = scenes_[static_cast<size_t>(into)];
    bool found = false;
    for (SceneUi::Target& target : scene.targets) {
      if (target.strip != channel.uid || target.what != what) continue;
      if (what == SceneTarget::What::Pattern && target.insert_tag != insert_tag) continue;
      if (what == SceneTarget::What::Param &&
          (target.insert_tag != insert_tag || target.param != param))
        continue;
      target.value = value;
      found = true;
      break;
    }
    if (!found) {
      scene.targets.push_back({channel.uid, what, insert_tag, value, param, stepped});
      // Only a new control changes what the ribbon and the strip show.
      emit scenesChanged();
      const QModelIndex idx = index(row);
      emit dataChanged(idx, idx, {SceneMarksRole});
    }
    publishScenes();
    markDirty();
    return;
  }

  // Playing, not recording: the hand wins. The conductor stops walking the
  // fader; the strip shows which of its controls are the player's now.
  if (scene_current_ < 0) return;
  if (what == SceneTarget::What::Level || what == SceneTarget::What::Param) {
    EngineCommand command;
    command.kind = EngineCommand::Kind::SceneHand;
    command.channel = channel.slot;
    command.bus = channel.is_bus;
    command.value = static_cast<float>(static_cast<int>(what));
    command.tag = insert_tag;
    command.param = param;
    engine_.post(command);
  }
  if ((sceneMarksFor(row) & bit_of(what)) != 0 &&
      (channel.scene_hands & bit_of(what)) == 0) {
    channel.scene_hands |= bit_of(what);
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {SceneHandsRole});
  }
}

int MixerModel::sceneMarksFor(int row) const {
  if (row < 0 || row >= static_cast<int>(channels_.size())) return 0;
  const ChannelUi& channel = channels_[static_cast<size_t>(row)];
  if (!channel.follow_scenes) return 0;
  const int shown = scene_recording_ ? recordScene() : scene_current_;
  if (shown < 0 || shown >= static_cast<int>(scenes_.size())) return 0;
  int marks = 0;
  for (const SceneUi::Target& target : scenes_[static_cast<size_t>(shown)].targets)
    if (target.strip == channel.uid) marks |= bit_of(target.what);
  return marks;
}

void MixerModel::announceSceneMarks() {
  if (channels_.empty()) return;
  emit dataChanged(index(0), index(static_cast<int>(channels_.size()) - 1),
                   {SceneMarksRole, SceneHandsRole});
}

// --- the conductor's table ------------------------------------------------------

quint64 MixerModel::sceneTableSignature() const {
  quint64 hash = 1469598103934665603ull;
  auto mix = [&hash](quint64 value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  for (const ChannelUi& channel : channels_) {
    mix(qHash(channel.uid));
    mix(channel.slot);
    mix(channel.is_bus ? 1 : 0);
    mix(channel.follow_scenes ? 1 : 0);
  }
  return hash;
}

void MixerModel::publishScenes() {
  auto table = std::make_shared<SceneTable>();
  table->scenes.reserve(scenes_.size());
  for (const SceneUi& scene : scenes_) {
    SceneTable::Scene entry;
    entry.bars = static_cast<uint32_t>(std::max(0, scene.bars));
    entry.fade_bars = static_cast<uint32_t>(std::max(0, scene.fade));
    entry.first = static_cast<uint32_t>(table->targets.size());
    for (const SceneUi::Target& target : scene.targets) {
      const int row = rowForUid(target.strip);
      if (row < 0) continue;
      const ChannelUi& channel = channels_[static_cast<size_t>(row)];
      if (!channel.follow_scenes) continue;
      SceneTarget out;
      out.what = target.what;
      out.bus = channel.is_bus;
      out.strip = static_cast<uint16_t>(channel.slot);
      out.insert_tag = target.insert_tag;
      out.param = target.param;
      out.stepped = target.stepped;
      out.value = target.value;
      table->targets.push_back(out);
    }
    entry.count = static_cast<uint32_t>(table->targets.size()) - entry.first;
    table->scenes.push_back(entry);
  }
  engine_.scenes().publish(std::move(table));
  scene_table_signature_ = sceneTableSignature();
}

void MixerModel::pollScenes() {
  SceneConductor& conductor = engine_.scenes();
  conductor.reclaim(engine_.running());
  // Strips came, went, moved or stopped following: the table names graph
  // slots, so it is rebuilt before a scene can start on the old picture.
  if (sceneTableSignature() != scene_table_signature_) publishScenes();

  bool changed = false;
  const uint32_t changes = conductor.scene_changes();
  if (changes != scene_changes_seen_) {
    scene_changes_seen_ = changes;
    for (ChannelUi& channel : channels_) channel.scene_hands = 0;
    changed = true;
  }
  const int current = conductor.current();
  const int armed = conductor.armed();
  const int bar = conductor.bar_in_scene();
  if (current != scene_current_ || armed != scene_armed_ || bar != scene_bar_) {
    const bool shown_moved = current != scene_current_;
    scene_current_ = current;
    scene_armed_ = armed;
    scene_bar_ = bar;
    changed = true;
    if (shown_moved) announceSceneMarks();
  } else if (changed) {
    announceSceneMarks();
  }
  if (engine_.playing()) {
    const int num = std::max(1, engine_.time_numerator());
    const qreal phase = std::fmod(engine_.transport_beats(), num) / num;
    if (phase != scene_bar_phase_) {
      scene_bar_phase_ = phase;
      changed = true;
    }
  }

  for (size_t row = 0; row < channels_.size(); ++row) {
    ChannelUi& channel = channels_[row];
    ChannelStrip* strip = stripFor(static_cast<int>(row));
    if (strip == nullptr) continue;
    QList<int> roles;
    // The scene moved this fader: the fader on screen goes with it, unless
    // the player has hold of it.
    const uint32_t writes = conductor.level_writes(channel.is_bus, channel.slot);
    if (writes != channel.level_writes_seen) {
      channel.level_writes_seen = writes;
      const qreal gain = strip->gain_target();
      if ((channel.scene_hands & bit_of(SceneTarget::What::Level)) == 0 &&
          std::abs(gain - channel.gain) > 1e-7) {
        channel.gain = gain;
        roles.append(GainRole);
      }
    }
    const bool on = strip->scene_on();
    if (on != channel.scene_on) {
      channel.scene_on = on;
      roles.append(SceneOnRole);
    }
    if (!roles.isEmpty()) {
      const QModelIndex idx = index(static_cast<int>(row));
      emit dataChanged(idx, idx, roles);
      markDirty(false);
    }
  }
  if (changed) emit sceneStateChanged();
}

// --- the session -----------------------------------------------------------------

QJsonObject MixerModel::scenesJson() const {
  QJsonArray items;
  for (const SceneUi& scene : scenes_) {
    QJsonArray targets;
    for (const SceneUi::Target& target : scene.targets) {
      QJsonObject saved;
      saved[QStringLiteral("strip")] = target.strip;
      saved[QStringLiteral("what")] = what_name(target.what);
      saved[QStringLiteral("value")] = static_cast<double>(target.value);
      if (target.what == SceneTarget::What::Pattern ||
          target.what == SceneTarget::What::Param) {
        // A tag is only good for this run; the session names the insert by
        // its place in the chain, which is how the strip itself lists them.
        const int row = rowForUid(target.strip);
        const int position = row < 0 ? -1 : position_of_tag(stripFor(row), target.insert_tag);
        if (position < 0) continue;
        saved[QStringLiteral("insert")] = position;
      }
      if (target.what == SceneTarget::What::Param) {
        saved[QStringLiteral("id")] = static_cast<qint64>(target.param);
        if (target.stepped) saved[QStringLiteral("stepped")] = true;
      }
      targets.append(saved);
    }
    QJsonObject entry;
    entry[QStringLiteral("name")] = scene.name;
    entry[QStringLiteral("hue")] = scene.hue;
    entry[QStringLiteral("bars")] = scene.bars;
    entry[QStringLiteral("fade")] = scene.fade;
    entry[QStringLiteral("targets")] = targets;
    items.append(entry);
  }
  QJsonObject json;
  json[QStringLiteral("auto")] = sceneAuto();
  json[QStringLiteral("items")] = items;
  return json;
}

void MixerModel::applyScenesJson(const QJsonObject& json) {
  scenes_.clear();
  for (const QJsonValue& value : json[QStringLiteral("items")].toArray()) {
    const QJsonObject entry = value.toObject();
    SceneUi scene;
    scene.name = entry[QStringLiteral("name")].toString();
    if (scene.name.isEmpty()) scene.name = tr("Scene %1").arg(scenes_.size() + 1);
    scene.hue = std::clamp(entry[QStringLiteral("hue")].toDouble(0.58), 0.0, 1.0);
    scene.bars = std::clamp(entry[QStringLiteral("bars")].toInt(8), 0, 999);
    scene.fade = std::clamp(entry[QStringLiteral("fade")].toInt(1), 0, 64);
    for (const QJsonValue& item : entry[QStringLiteral("targets")].toArray()) {
      const QJsonObject saved = item.toObject();
      SceneUi::Target target;
      if (!what_from_name(saved[QStringLiteral("what")].toString(), &target.what)) continue;
      target.strip = saved[QStringLiteral("strip")].toString();
      target.value = static_cast<float>(saved[QStringLiteral("value")].toDouble());
      if (target.what == SceneTarget::What::Gate)
        target.value = target.value >= 0.5f ? 1.0f : 0.0f;
      if (target.what == SceneTarget::What::Level)
        target.value = std::clamp(target.value, 0.0f, 4.0f);
      if (target.what == SceneTarget::What::Pattern) {
        target.value = std::clamp(std::round(target.value), 0.0f, 15.0f);
        // Back to a tag, and only for a sequencer: a chain that changed shape
        // since the save leaves the target in the scene, pointing nowhere,
        // rather than switching patterns on some other plugin.
        const int row = rowForUid(target.strip);
        const int slot = row < 0 ? -1
                                 : slot_of_position(stripFor(row),
                                                    saved[QStringLiteral("insert")].toInt(-1));
        if (slot >= 0 && insertIsStepSequencer(row, slot))
          target.insert_tag = stripFor(row)->insert_tag(static_cast<size_t>(slot));
      }
      if (target.what == SceneTarget::What::Param) {
        target.param = static_cast<uint32_t>(saved[QStringLiteral("id")].toInteger(-1));
        target.stepped = saved[QStringLiteral("stepped")].toBool(false);
        const int row = rowForUid(target.strip);
        const int slot = row < 0 ? -1
                                 : slot_of_position(stripFor(row),
                                                    saved[QStringLiteral("insert")].toInt(-1));
        // Only onto a plugin the audio thread may set, whatever the file says.
        if (slot >= 0 && sceneParamAllowed(insertFor(row, slot)))
          target.insert_tag = stripFor(row)->insert_tag(static_cast<size_t>(slot));
      }
      scene.targets.push_back(target);
    }
    scenes_.push_back(std::move(scene));
  }
  engine_.scenes().set_auto(json[QStringLiteral("auto")].toBool(true));
  publishScenes();
  emit scenesChanged();
  emit sceneStateChanged();
  announceSceneMarks();
}

}  // namespace nirbija
