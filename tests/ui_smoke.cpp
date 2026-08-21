// Drives MixerModel the way the QML does — add a channel, move a fader, load an
// insert — without opening a window. Catches the wiring breaking between the UI
// and the engine, which a compile check cannot.

#include <QGuiApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include <QSettings>

#include "mixer_model.h"
#include "skin.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

QVariant field(nirbija::MixerModel& mixer, int row, int role) {
  return mixer.data(mixer.index(row), role);
}

// A kit that actually names its pads, standing in for DrumGizmo or any
// sampler that publishes clap.note-name. Used to prove the sequencer reads
// the next insert and nobody after it.
class NamedKit : public nirbija::PluginInstance {
 public:
  void set_channel_layout(int) override {}
  bool activate(double, uint32_t) override { return true; }
  void deactivate() override {}
  void process(const float* const*, float* const*, uint32_t) override {}

  std::vector<nirbija::ParameterInfo> parameters() const override { return {}; }
  double parameter_value(uint32_t) const override { return 0.0; }
  void set_parameter(uint32_t, double) override {}

  std::vector<uint8_t> save_state() const override { return {}; }
  bool load_state(const std::vector<uint8_t>&) override { return true; }

  const nirbija::PluginDescriptor& descriptor() const override { return desc_; }

  std::vector<nirbija::NoteName> note_names() const override {
    return {{38, "snare"}, {36, "kick"}, {36, "also-kick"}, {42, "hat"}};
  }

 private:
  nirbija::PluginDescriptor desc_{.format = nirbija::PluginFormat::Internal,
                                  .uid = "test.namedkit",
                                  .name = "Named Kit",
                                  .vendor = "Nirbija",
                                  .kind = nirbija::PluginKind::Instrument,
                                  .audio_inputs = 0,
                                  .audio_outputs = 2,
                                  .has_midi_input = true};
};

}  // namespace

int main(int argc, char* argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  // QSettings writes to the user's real configuration unless it is told
  // otherwise, and a test has no business resizing somebody's mixer.
  QCoreApplication::setOrganizationName(QStringLiteral("nirbija-test"));
  QCoreApplication::setApplicationName(QStringLiteral("ui-smoke"));
  QSettings::setDefaultFormat(QSettings::IniFormat);

  // Point the session somewhere disposable before building a mixer: the model
  // both loads a session on construction and saves one on destruction, so a
  // test left on the default path would restore, and then overwrite, whatever
  // the person using this machine had open.
  QTemporaryDir dir;
  if (!dir.isValid()) {
    fail("could not make a temporary directory");
    return 1;
  }
  qputenv("NIRBIJA_SESSION", (dir.path() + "/session.json").toLocal8Bit());

  nirbija::MixerModel mixer;
  if (!mixer.running()) {
    std::printf("no audio server available, skipping\n");
    return 0;
  }

  mixer.addChannel(QStringLiteral("Guitar"), 2);
  if (mixer.rowCount() != 1) {
    fail("addChannel did not add a row");
    return 1;
  }
  if (field(mixer, 0, nirbija::MixerModel::NameRole).toString() != "Guitar")
    fail("channel name did not survive addChannel");

  // The fader is in decibels, so a half-travel position is nowhere near half
  // the gain — this is the round-trip the QML relies on.
  const qreal gain = nirbija::MixerModel::faderToGain(0.5);
  mixer.setGain(0, gain);
  if (field(mixer, 0, nirbija::MixerModel::GainRole).toReal() != gain)
    fail("setGain did not reach the model");
  if (std::abs(nirbija::MixerModel::gainToFader(gain) - 0.5) > 1e-9)
    fail("fader position did not survive the gain round-trip");

  mixer.toggleMute(0);
  if (!field(mixer, 0, nirbija::MixerModel::MutedRole).toBool())
    fail("toggleMute did not reach the model");
  mixer.toggleMute(0);

  if (mixer.plugins()->rowCount() == 0) {
    std::printf("no plugins installed, skipping the insert check\n");
  } else {
    // Pick the first plugin that takes audio in, so the insert is a legitimate
    // one rather than an instrument.
    int chosen = -1;
    for (int i = 0; i < mixer.plugins()->rowCount(); ++i) {
      const nirbija::PluginDescriptor* desc = mixer.plugins()->descriptor(i);
      if (desc != nullptr && desc->audio_inputs >= 2) {
        chosen = i;
        break;
      }
    }
    if (chosen < 0) {
      std::printf("no stereo effect found, skipping the insert check\n");
    } else if (!mixer.addInsert(0, chosen)) {
      fail("addInsert failed for " + mixer.plugins()->descriptor(chosen)->name);
    } else {
      const QStringList inserts =
          field(mixer, 0, nirbija::MixerModel::InsertsRole).toStringList();
      if (inserts.size() != 1) fail("insert did not show up in the model");
      std::printf("  loaded insert: %s\n", inserts.value(0).toUtf8().constData());

      mixer.removeInsert(0, 0);
      const QStringList after =
          field(mixer, 0, nirbija::MixerModel::InsertsRole).toStringList();
      // Removal leaves the slot in place so the surviving indices stay stable.
      if (after.size() != 1 || !after.value(0).isEmpty())
        fail("removeInsert did not clear the slot label");

      // Adding again fills the hole the removal left, rather than growing the
      // chain downwards past it forever.
      if (mixer.addInsert(0, chosen)) {
        const QStringList refilled =
            field(mixer, 0, nirbija::MixerModel::InsertsRole).toStringList();
        if (refilled.size() != 1 || refilled.value(0).isEmpty())
          fail("adding after a removal did not reuse the hole");
        mixer.removeInsert(0, 0);
      }
    }
  }

  // --- editing what is already assigned --------------------------------------
  {
    int first = -1;
    int second = -1;
    for (int i = 0; i < mixer.plugins()->rowCount(); ++i) {
      const nirbija::PluginDescriptor* descriptor = mixer.plugins()->descriptor(i);
      if (descriptor == nullptr || descriptor->audio_inputs < 2) continue;
      if (first < 0) {
        first = i;
      } else {
        second = i;
        break;
      }
    }

    if (second < 0) {
      std::printf("not enough stereo effects to test reordering, skipping\n");
    } else {
      mixer.addChannel(QStringLiteral("Chain"), 2);
      const int row = mixer.rowCount() - 1;
      mixer.addInsert(row, first);
      mixer.addInsert(row, second);

      const QString top = mixer.plugins()->descriptor(first)->name.c_str();
      const QString bottom = mixer.plugins()->descriptor(second)->name.c_str();

      QStringList inserts =
          field(mixer, row, nirbija::MixerModel::InsertsRole).toStringList();
      if (inserts != QStringList{top, bottom})
        fail("inserts did not load in the order they were added");

      mixer.moveInsert(row, 0, 1);
      inserts = field(mixer, row, nirbija::MixerModel::InsertsRole).toStringList();
      if (inserts != QStringList{bottom, top})
        fail("moving an insert down did not reorder the chain");

      // Moving past either end is a no-op rather than a wrap-around.
      mixer.moveInsert(row, 0, -1);
      inserts = field(mixer, row, nirbija::MixerModel::InsertsRole).toStringList();
      if (inserts != QStringList{bottom, top})
        fail("moving the top insert up should have done nothing");

      mixer.renameChannel(row, QStringLiteral("Renamed"));
      if (field(mixer, row, nirbija::MixerModel::NameRole).toString() != "Renamed")
        fail("renaming a channel did not stick");

      const int before_rows = mixer.rowCount();
      mixer.removeChannel(row);
      if (mixer.rowCount() != before_rows - 1)
        fail("removing a channel did not drop the row");
    }
  }

  // --- mix buses ------------------------------------------------------------
  {
    const int before = mixer.rowCount();
    mixer.addBus(QStringLiteral("Drums"));
    if (mixer.rowCount() != before + 1) {
      fail("adding a bus did not add a row");
    } else {
      const int bus = mixer.rowCount() - 1;
      if (!field(mixer, bus, nirbija::MixerModel::IsBusRole).toBool())
        fail("the new row is not marked as a bus");

      // A channel can send to the bus.
      const QVariantList options = mixer.destinationsFor(0);
      if (options.size() < 2) fail("the bus is not offered as a destination");

      const int destination =
          options.last().toMap().value(QStringLiteral("destination")).toInt();
      mixer.setDestination(0, destination);
      if (field(mixer, 0, nirbija::MixerModel::DestinationRole).toInt() != destination)
        fail("the channel did not take the new destination");
      if (field(mixer, 0, nirbija::MixerModel::OutputLabelRole).toString() != "Drums")
        fail("the output label did not follow the destination");

      // A bus may not feed itself, so it is never in its own list.
      for (const QVariant& option : mixer.destinationsFor(bus)) {
        if (option.toMap().value(QStringLiteral("destination")).toInt() ==
            destination)
          fail("a bus was offered itself as a destination");
      }

      // A send feeds the bus on top of whatever the destination is.
      mixer.setSend(0, 0, destination, 0.5);
      QVariantList sends =
          field(mixer, 0, nirbija::MixerModel::SendsRole).toList();
      if (sends.size() != 1) {
        fail("the send did not show up on the channel");
      } else {
        const QVariantMap send = sends.first().toMap();
        if (send.value(QStringLiteral("bus")).toInt() != destination)
          fail("the send points at the wrong bus");
        if (send.value(QStringLiteral("name")).toString() != "Drums")
          fail("the send is not labelled with its bus");
      }

      mixer.removeSend(0, 0);
      sends = field(mixer, 0, nirbija::MixerModel::SendsRole).toList();
      if (!sends.isEmpty()) fail("removing the send left it in the list");

      mixer.setDestination(0, -1);
    }
  }

  // --- routing --------------------------------------------------------------
  // The master should already be connected: a mixer that opens silent is not
  // finished opening.
  if (mixer.masterSink().isEmpty())
    fail("master was not connected to an output on startup");

  const QStringList midi_sources = mixer.sources(true);
  if (midi_sources.isEmpty()) {
    std::printf("no MIDI sources on this machine, skipping the routing check\n");
  } else {
    const QString chosen = midi_sources.first();
    mixer.connectSource(0, chosen, true);

    const QString label =
        field(mixer, 0, nirbija::MixerModel::MidiLabelRole).toString();
    if (label == QStringLiteral("no MIDI"))
      fail("connecting a MIDI source did not change the strip label");
    else
      std::printf("  MIDI source: %s -> %s\n", chosen.toUtf8().constData(),
                  label.toUtf8().constData());

    mixer.connectSource(0, QString(), true);
    if (field(mixer, 0, nirbija::MixerModel::MidiLabelRole).toString() !=
        QStringLiteral("no MIDI"))
      fail("disconnecting a MIDI source did not clear the strip label");

    // The matrix path: individual links come and go without touching the rest.
    mixer.setMidiLink(0, midi_sources.first(), true);
    if (!mixer.midiLinked(0, midi_sources.first()))
      fail("the matrix link did not stick");
    if (midi_sources.size() > 1) {
      mixer.setMidiLink(0, midi_sources.at(1), true);
      if (!mixer.midiLinked(0, midi_sources.first()) ||
          !mixer.midiLinked(0, midi_sources.at(1)))
        fail("a second matrix link displaced the first");
      mixer.setMidiLink(0, midi_sources.at(1), false);
    }
    mixer.setMidiLink(0, midi_sources.first(), false);
    if (mixer.midiLinked(0, midi_sources.first()))
      fail("unlinking through the matrix did not disconnect");
  }

  // --- MIDI learn -------------------------------------------------------------
  {
    mixer.learnGain(0);
    if (!mixer.learning()) fail("arming learn did not enter learn mode");

    // The first message binds and is consumed rather than acted on.
    const qreal before = field(mixer, 0, nirbija::MixerModel::GainRole).toReal();
    mixer.injectControl(21, 0, 100);
    if (mixer.learning()) fail("the binding message did not close learn mode");
    if (field(mixer, 0, nirbija::MixerModel::GainRole).toReal() != before)
      fail("the binding message also moved the fader");

    // From then on the control drives the fader through its curve.
    mixer.injectControl(21, 0, 127);
    const qreal top = field(mixer, 0, nirbija::MixerModel::GainRole).toReal();
    mixer.injectControl(21, 0, 0);
    const qreal bottom = field(mixer, 0, nirbija::MixerModel::GainRole).toReal();
    if (top <= bottom || bottom != 0.0)
      fail("the bound control does not sweep the fader");

    // A different CC does nothing.
    mixer.injectControl(22, 0, 127);
    if (field(mixer, 0, nirbija::MixerModel::GainRole).toReal() != bottom)
      fail("an unbound CC moved the fader");

    mixer.clearMidiMaps(0);
    mixer.injectControl(21, 0, 127);
    if (field(mixer, 0, nirbija::MixerModel::GainRole).toReal() != bottom)
      fail("clearing the maps did not unbind the control");
  }

  // A toggle pad sends 127 then 0. Rec/Play have to flip on the press and
  // ignore the off, or mapping a latching button punches in and straight
  // back out.
  {
    const int fx = mixer.plugins()->rowFor(nirbija::PluginFormat::Internal,
                                           "nirbija.fxpad");
    if (fx >= 0 && mixer.addInsert(0, fx)) {
      mixer.learnInsertParam(0, 0, 16, 0.0, 1.0);  // Hold
      mixer.injectControl(40, 0, 127);
      mixer.injectControl(40, 0, 0);  // latch off: must not undo the press
      mixer.injectControl(40, 0, 127);
      if (!mixer.fxPadHold(0, 0))
        fail("a toggle pad did not latch Hold on");
      mixer.injectControl(40, 0, 0);
      mixer.injectControl(40, 0, 127);
      if (mixer.fxPadHold(0, 0))
        fail("a second press did not release Hold");
      mixer.removeInsert(0, 0);
    }
  }

  // --- the picker's kind filter --------------------------------------------
  //
  // Runs against whatever is installed rather than a fixture, so it asserts the
  // shape of the answer, not a count: kinds partition the list, the filter
  // narrows to exactly the rows carrying that kind, and search runs inside the
  // kind rather than reaching back outside it.
  {
    nirbija::PluginFilterModel filter;
    filter.setSourceModel(mixer.plugins());
    const int all = filter.rowCount();
    if (all != mixer.plugins()->rowCount())
      fail("an unfiltered picker did not show every plugin");

    int summed = 0;
    for (const int kind :
         {nirbija::PluginFilterModel::Instrument, nirbija::PluginFilterModel::Effect,
          nirbija::PluginFilterModel::MidiEffect,
          nirbija::PluginFilterModel::Analyzer, nirbija::PluginFilterModel::Utility,
          nirbija::PluginFilterModel::Unknown}) {
      filter.setKind(kind);
      const int count = filter.rowCount();
      summed += count;
      for (int row = 0; row < count; ++row) {
        const int source = filter.sourceRow(row);
        const QVariant got = mixer.plugins()->data(
            mixer.plugins()->index(source, 0), nirbija::PluginListModel::KindRole);
        if (got.toInt() != kind) fail("the kind filter let another kind through");
      }
    }
    if (summed != all) fail("the kinds did not add up to the whole list");

    // A kind and a query together are an and, not an or.
    filter.setKind(nirbija::PluginFilterModel::Instrument);
    filter.setQuery(QStringLiteral("zzz-no-such-plugin"));
    if (filter.rowCount() != 0)
      fail("a query that matches nothing still returned rows");

    filter.setQuery({});
    filter.setKind(nirbija::PluginFilterModel::AnyKind);
    if (filter.rowCount() != all) fail("clearing the filters did not restore it");
  }

  // --- duplicating a strip brings the chain with it --------------------------
  //
  // It used to copy the name, the fader and the pan and leave the chain empty,
  // which is not a copy of anything: the point of duplicating a strip is
  // having the same instrument twice.
  {
    int effect = -1;
    for (int i = 0; i < mixer.plugins()->rowCount(); ++i) {
      const nirbija::PluginDescriptor* descriptor = mixer.plugins()->descriptor(i);
      if (descriptor != nullptr && descriptor->audio_inputs >= 2) {
        effect = i;
        break;
      }
    }
    if (effect < 0) {
      std::printf("no stereo effect found, skipping the duplicate check\n");
    } else {
      mixer.addChannel(QStringLiteral("Source"), 2);
      const int source = mixer.rowCount() - 1;
      mixer.addInsert(source, effect);
      mixer.setGain(source, 0.42);
      mixer.setPan(source, -0.3);

      const QStringList before =
          field(mixer, source, nirbija::MixerModel::InsertsRole).toStringList();

      mixer.duplicateChannel(source);
      const int copy = mixer.rowCount() - 1;
      if (copy == source) {
        fail("duplicateChannel did not add a row");
      } else {
        const QStringList after =
            field(mixer, copy, nirbija::MixerModel::InsertsRole).toStringList();
        if (after != before)
          fail("the copy's chain does not match the original's");
        if (std::abs(field(mixer, copy, nirbija::MixerModel::GainRole).toReal()
                     - 0.42) > 1e-9)
          fail("the copy did not take the gain");
        if (std::abs(field(mixer, copy, nirbija::MixerModel::PanRole).toReal()
                     + 0.3) > 1e-9)
          fail("the copy did not take the pan");
      }
      mixer.removeChannel(mixer.rowCount() - 1);
      mixer.removeChannel(source);
    }
  }

  // --- the interface resizes, and remembers --------------------------------
  //
  // NIRBIJA_UI_SCALE only works by restarting, which is no use to somebody
  // sitting at a screen where everything is too small.
  {
    nirbija::Skin skin;
    const qreal start = nirbija::Skin::scale();
    const int strip = nirbija::Skin::stripWidth();

    skin.zoomIn();
    if (nirbija::Skin::scale() <= start) fail("zooming in did not grow the scale");
    if (nirbija::Skin::stripWidth() <= strip)
      fail("the scale moved but the sizes derived from it did not");

    skin.zoomOut();
    if (std::abs(nirbija::Skin::scale() - start) > 1e-6)
      fail("in then out did not come back to where it started");

    // Both ends are stops, not places to fall off.
    for (int i = 0; i < 60; ++i) skin.zoomOut();
    if (nirbija::Skin::scale() < nirbija::Skin::kMinScale - 1e-9)
      fail("zooming out went below the floor");
    for (int i = 0; i < 120; ++i) skin.zoomIn();
    if (nirbija::Skin::scale() > nirbija::Skin::kMaxScale + 1e-9)
      fail("zooming in went past the ceiling");

    skin.zoomReset();
    if (std::abs(nirbija::Skin::scale() - nirbija::Skin::startingScale()) > 1e-6)
      fail("reset did not return to the starting size");

    // What was chosen has to survive the program closing, which is the whole
    // point of choosing it on a machine whose screen does not change.
    skin.setScale(1.4);
    QSettings settings;
    settings.sync();
    if (std::abs(settings.value(QStringLiteral("ui/scale")).toReal() - 1.4) > 1e-6)
      fail("the chosen size was not written down");
    skin.zoomReset();
  }

  // --- no two strips wear the same colour -------------------------------------
  //
  // The accent used to come from the graph slot, and channels and buses are
  // handed slots from separate pools - so a channel and a bus could land on
  // the same colour with half the palette unused. Six is the palette; up to
  // six strips must all differ, and past that the wear has to stay even.
  {
    while (mixer.rowCount() > 0) mixer.removeChannel(0);

    // The shape a real session has when this bit: four channels, two buses,
    // and then a strip loaded in from a file. The old formula gave the fourth
    // channel and the first bus the same colour, and the fifth channel the
    // same as the second bus - which is exactly what showed up on screen.
    for (int i = 0; i < 4; ++i) mixer.addChannel({}, 2);
    for (int i = 0; i < 2; ++i) mixer.addBus({});

    QStringList seen;
    for (int row = 0; row < mixer.rowCount(); ++row)
      seen.append(field(mixer, row, nirbija::MixerModel::AccentRole).toString());

    QStringList unique = seen;
    unique.removeDuplicates();
    if (unique.size() != seen.size())
      fail("six strips share " + std::to_string(seen.size() - unique.size()) +
           " colour(s): " + seen.join(QStringLiteral(" ")).toStdString());

    // The palette holds six, so a seventh must repeat one - but only one, and
    // not a second time while another colour sits unused. This is the strip
    // being loaded in from a file.
    mixer.addChannel({}, 2);
    QStringList after;
    for (int row = 0; row < mixer.rowCount(); ++row)
      after.append(field(mixer, row, nirbija::MixerModel::AccentRole).toString());
    for (const QString& accent : unique) {
      const auto count = after.count(accent);
      if (count > 2)
        fail("a colour is worn " + std::to_string(count) +
             " times while others are free");
    }
    while (mixer.rowCount() > 0) mixer.removeChannel(0);
    mixer.addChannel(QStringLiteral("Guitar"), 2);
  }

  // --- a strip saved on its own comes back whole -----------------------------
  //
  // The point of the format: a chain you liked, with every plugin's state, in a
  // file you can hand to somebody. And when they do not have one of the
  // plugins, the strip still arrives - with a hole, and with the hole named.
  {
    int effect = -1;
    for (int i = 0; i < mixer.plugins()->rowCount(); ++i) {
      const nirbija::PluginDescriptor* descriptor = mixer.plugins()->descriptor(i);
      if (descriptor != nullptr && descriptor->audio_inputs >= 2) { effect = i; break; }
    }
    // The step sequencer is the interesting half: its state is a pattern, and a
    // preset that forgets the pattern is not a preset.
    const int sequencer =
        mixer.plugins()->rowFor(nirbija::PluginFormat::Internal, "nirbija.stepseq");

    if (effect < 0 || sequencer < 0) {
      std::printf("no plugin pair for the strip preset check, skipping\n");
    } else {
      mixer.addChannel(QStringLiteral("Goth"), 2);
      const int source = mixer.rowCount() - 1;
      mixer.addInsert(source, sequencer);
      mixer.addInsert(source, effect);
      mixer.setGain(source, 0.33);

      // A pattern nobody would land on by accident.
      mixer.setInsertParameter(source, 0, 16 + 0, 42.0);
      mixer.setInsertParameter(source, 0, 80 + 0, 1.0);
      mixer.setInsertParameter(source, 0, 1, 7.0);  // seven steps

      const QString path = dir.path() + QStringLiteral("/goth.strip.json");
      if (!mixer.saveChannelTo(source, QUrl::fromLocalFile(path)))
        fail("saving a strip failed");

      const int before = mixer.rowCount();
      if (!mixer.loadChannelFrom(QUrl::fromLocalFile(path))) {
        fail("loading a strip failed");
      } else if (mixer.rowCount() != before + 1) {
        fail("loading a strip did not add a row");
      } else {
        const int copy = mixer.rowCount() - 1;
        const QStringList chain =
            field(mixer, copy, nirbija::MixerModel::InsertsRole).toStringList();
        const QStringList want =
            field(mixer, source, nirbija::MixerModel::InsertsRole).toStringList();
        if (chain != want) fail("the loaded strip has a different chain");
        if (std::abs(field(mixer, copy, nirbija::MixerModel::GainRole).toReal()
                     - 0.33) > 1e-9)
          fail("the loaded strip did not take the gain");

        // The state is the whole reason for the feature. Step 1's note (id
        // 16) is no longer in insertParameters() - an 8x64 grid does not
        // list per-step IDs - so it comes from the snapshot instead, the
        // same place the editor itself reads it from.
        const QVariantList params = mixer.insertParameters(copy, 0);
        double steps = -1;
        for (const QVariant& value : params) {
          const QVariantMap p = value.toMap();
          if (p.value(QStringLiteral("id")).toInt() == 1) steps = p.value(QStringLiteral("value")).toDouble();
        }
        if (std::abs(steps - 7.0) > 1e-9)
          fail("the sequencer's length did not survive the preset");

        const QVariantMap snap = mixer.insertSequencerSnapshot(copy, 0);
        if (snap.isEmpty())
          fail("insertSequencerSnapshot was empty on a sequencer");
        const QVariantList notes = snap.value(QStringLiteral("note")).toList();
        if (notes.isEmpty() || std::abs(notes.value(0).toDouble() - 42.0) > 1e-9)
          fail("the sequencer's pattern did not survive the preset");
        const QVariantList ons = snap.value(QStringLiteral("on")).toList();
        if (ons.size() != 512)
          fail("sequencer snapshot on list was " + std::to_string(ons.size()) +
               ", wanted 512");
        mixer.removeChannel(copy);
      }

      // Now the same file with a plugin nobody has: it must still load, and
      // must say what is missing rather than failing or going quiet.
      QFile written(path);
      if (written.open(QIODevice::ReadOnly)) {
        QString text = QString::fromUtf8(written.readAll());
        written.close();
        text.replace(QStringLiteral("nirbija.stepseq"),
                     QStringLiteral("nobody.has.this"));
        const QString broken = dir.path() + QStringLiteral("/missing.strip.json");
        QFile out(broken);
        if (out.open(QIODevice::WriteOnly)) {
          out.write(text.toUtf8());
          out.close();

          QString reported;
          const auto link = QObject::connect(
              &mixer, &nirbija::MixerModel::errorOccurred,
              [&reported](const QString& message) { reported = message; });

          const int rows = mixer.rowCount();
          const bool ok = mixer.loadChannelFrom(QUrl::fromLocalFile(broken));
          QObject::disconnect(link);

          if (!ok) fail("a strip with one missing plugin refused to load at all");
          if (mixer.rowCount() != rows + 1)
            fail("a strip with a missing plugin did not arrive");
          if (!reported.contains(QStringLiteral("nobody.has.this")))
            fail("the missing plugin was not named: \"" + reported.toStdString() + "\"");
          if (mixer.rowCount() == rows + 1) mixer.removeChannel(mixer.rowCount() - 1);
        }
      }
      mixer.removeChannel(source);
    }
  }

  // --- sequencer pad names come from the next insert only --------------------
  {
    const int sequencer =
        mixer.plugins()->rowFor(nirbija::PluginFormat::Internal, "nirbija.stepseq");
    const int keyboard =
        mixer.plugins()->rowFor(nirbija::PluginFormat::Internal, "nirbija.keyboard");
    if (sequencer < 0) {
      std::printf("no step sequencer, skipping the pad-target check\n");
    } else {
      mixer.addChannel(QStringLiteral("Kit"), 2);
      const int row = mixer.rowCount() - 1;
      mixer.addInsert(row, sequencer);

      QVariantMap none = mixer.insertSequencerTarget(row, 0);
      if (!none.value(QStringLiteral("name")).toString().isEmpty())
        fail("a sequencer with nothing below it still named a target");
      if (!none.value(QStringLiteral("pads")).toList().isEmpty())
        fail("a sequencer with nothing below it still listed pads");

      nirbija::ChannelStrip* strip = nullptr;
      nirbija::AudioGraph& graph = mixer.engineForTests().graph();
      for (size_t i = 0; i < nirbija::kMaxChannels; ++i) {
        if (!graph.channel_alive(i)) continue;
        if (graph.channel(i).name() != "Kit") continue;
        strip = &graph.channel(i);
        break;
      }
      if (strip == nullptr || !strip->add_insert(std::make_unique<NamedKit>()))
        fail("could not add the named kit under the sequencer");
      if (keyboard >= 0) mixer.addInsert(row, keyboard);

      const QVariantMap target = mixer.insertSequencerTarget(row, 0);
      if (target.value(QStringLiteral("name")).toString() != QStringLiteral("Named Kit"))
        fail("the sequencer did not bind to the insert below it: \"" +
             target.value(QStringLiteral("name")).toString().toStdString() + "\"");
      const QVariantList pads = target.value(QStringLiteral("pads")).toList();
      if (pads.size() != 3)
        fail("pad list was " + std::to_string(pads.size()) +
             ", wanted 3 unique keys sorted");
      else {
        const int a = pads.value(0).toMap().value(QStringLiteral("note")).toInt();
        const int b = pads.value(1).toMap().value(QStringLiteral("note")).toInt();
        const int c = pads.value(2).toMap().value(QStringLiteral("note")).toInt();
        const QString kick =
            pads.value(0).toMap().value(QStringLiteral("name")).toString();
        if (a != 36 || b != 38 || c != 42)
          fail("pads were not sorted unique keys");
        if (kick != QStringLiteral("kick"))
          fail("duplicate key did not keep the first name");
      }

      mixer.removeChannel(row);
    }
  }

  // --- File -> Load session replaces the mixer, it does not add to it --------
  //
  // Loading over a mixer that already had strips left one behind, so a session
  // opened from the menu showed a stray empty channel that was not in the file.
  {
    const QString path = dir.path() + QStringLiteral("/two.json");
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
      fail("could not write a session to load");
    } else {
      out.write(R"({
        "version": 1, "tempo": 120, "master": { "gain": 0.8 },
        "channels": [
          { "name": "One", "isBus": false, "width": 2, "inserts": [], "sends": [] },
          { "name": "Two", "isBus": false, "width": 2, "inserts": [], "sends": [] },
          { "name": "Bus", "isBus": true, "width": 2, "inserts": [], "sends": [] }
        ]
      })");
      out.close();

      // Deliberately more strips than the file holds, so a load that appended
      // instead of replacing would leave the extras behind.
      while (mixer.rowCount() < 4) mixer.addChannel({}, 2);

      if (!mixer.loadSessionFrom(QUrl::fromLocalFile(path))) {
        fail("loading a valid session reported failure");
      } else if (mixer.rowCount() != 3) {
        fail(QStringLiteral("load left %1 strips, the file holds 3")
                 .arg(mixer.rowCount())
                 .toStdString());
      } else {
        for (const auto& [row, want] :
             {std::pair{0, "One"}, std::pair{1, "Two"}, std::pair{2, "Bus"}}) {
          if (field(mixer, row, nirbija::MixerModel::NameRole).toString() != want)
            fail("a loaded strip carries the wrong name");
        }
      }
    }
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
