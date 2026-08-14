// Drives MixerModel the way the QML does — add a channel, move a fader, load an
// insert — without opening a window. Catches the wiring breaking between the UI
// and the engine, which a compile check cannot.

#include <QGuiApplication>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

#include "mixer_model.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

QVariant field(nirbija::MixerModel& mixer, int row, int role) {
  return mixer.data(mixer.index(row), role);
}

}  // namespace

int main(int argc, char* argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);

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
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
