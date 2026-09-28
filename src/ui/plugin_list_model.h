// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QSortFilterProxyModel>
#include <qqmlintegration.h>

#include <future>
#include <memory>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// Every plugin the compiled-in backends can see, scanned once at startup.
// Scanning walks the disk and dlopens modules, so it never happens on demand
// from a QML binding - and it never happens on the GUI thread either: the
// scan runs on a worker and the list fills in when it returns, so the window
// is on screen while the disk is being walked instead of black until then.
// The backends keep an on-disk scan cache, so the second start is quick.
class PluginListModel : public QAbstractListModel {
  Q_OBJECT
  QML_ELEMENT
  // The one instance belongs to the mixer, which scans at startup; QML reaches
  // it through Mixer.plugins.
  QML_UNCREATABLE("Reach the plugin list through Mixer.plugins.")
  Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
  Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)

 public:
  enum Roles {
    NameRole = Qt::UserRole + 1,
    VendorRole,
    FormatRole,
    UidRole,
    CategoryRole,  // the plugin's own words, shown beside the name
    KindRole,      // the bucket, as an int matching PluginKind, for filtering
  };

  explicit PluginListModel(QObject* parent = nullptr);
  ~PluginListModel() override;

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  // Starts a scan on the worker; the model resets when it comes back and
  // scanFinished() says so. A scan already running is waited for first.
  Q_INVOKABLE void rescan();
  bool scanning() const { return scanning_; }
  // Blocks the caller until the running scan has been applied. Negative
  // waits as long as it takes; returns false if the time ran out first.
  Q_INVOKABLE bool waitForScan(int milliseconds = -1);

  // Finds the row holding a given plugin, for restoring a saved session.
  // Returns -1 when the plugin is no longer installed.
  int rowFor(PluginFormat format, const std::string& uid) const;

  // Used by MixerModel to turn a picker row into a live plugin. Waits for a
  // running scan first: the backends are the worker's while it scans.
  std::unique_ptr<PluginInstance> instantiate(int row);
  const PluginDescriptor* descriptor(int row) const;

 signals:
  void countChanged();
  void scanningChanged();
  void scanFinished();

 private:
  static QString format_name(PluginFormat format);

  std::vector<std::unique_ptr<PluginBackend>> backends_;
  struct Entry {
    PluginDescriptor descriptor;
    size_t backend_index;
  };
  std::vector<Entry> entries_;

  // Takes the worker's result into the model. `generation` names the scan it
  // came from: the queued completion and an explicit waitForScan() can both
  // arrive with the same result, and only the first may apply it.
  void applyScan(int generation);
  std::future<std::vector<Entry>> pending_;
  int scan_generation_ = 0;
  int applied_generation_ = 0;
  bool scanning_ = false;
};

// The picker's search, done by the model instead of by the delegate.
//
// The list used to be filtered in QML by giving every non-matching delegate
// `visible: false` and zero height: the view still built a delegate for each of
// the hundreds of installed plugins, and typing a letter re-laid out all of
// them. Filtering here means the view only ever sees the rows that match.
class PluginFilterModel : public QSortFilterProxyModel {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
  Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
  // -1 shows everything; otherwise a PluginKind. Two hundred plugins is too
  // many to read, and "I want a synth" is the question people actually arrive
  // with, so the kind filter narrows before a single letter is typed.
  Q_PROPERTY(int kind READ kind WRITE setKind NOTIFY kindChanged)

 public:
  // Mirrors nirbija::PluginKind so QML can name a filter without knowing the
  // core header. Kept in the same order, and checked against it at compile
  // time in the .cpp.
  enum Kind {
    AnyKind = -1,
    Instrument = 0,
    Effect,
    MidiEffect,
    Analyzer,
    Utility,
    Unknown,
  };
  Q_ENUM(Kind)

  explicit PluginFilterModel(QObject* parent = nullptr);

  QString query() const { return query_; }
  void setQuery(const QString& query);

  int kind() const { return kind_; }
  void setKind(int kind);

  // The row this proxy row stands for in the scan, which is what the mixer's
  // insert calls take.
  Q_INVOKABLE int sourceRow(int proxyRow) const;

 signals:
  void queryChanged();
  void countChanged();
  void kindChanged();

 protected:
  bool filterAcceptsRow(int source_row,
                        const QModelIndex& source_parent) const override;

 private:
  void refilter();

  QString query_;
  int kind_ = AnyKind;
};

}  // namespace nirbija
