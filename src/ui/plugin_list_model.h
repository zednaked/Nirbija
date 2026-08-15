#pragma once

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QSortFilterProxyModel>
#include <qqmlintegration.h>

#include <memory>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// Every plugin the compiled-in backends can see, scanned once at startup.
// Scanning walks the disk and dlopens modules, so it never happens on demand
// from a QML binding.
class PluginListModel : public QAbstractListModel {
  Q_OBJECT
  QML_ELEMENT
  // The one instance belongs to the mixer, which scans at startup; QML reaches
  // it through Mixer.plugins.
  QML_UNCREATABLE("Reach the plugin list through Mixer.plugins.")
  Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

 public:
  enum Roles {
    NameRole = Qt::UserRole + 1,
    VendorRole,
    FormatRole,
    UidRole,
  };

  explicit PluginListModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  Q_INVOKABLE void rescan();

  // Finds the row holding a given plugin, for restoring a saved session.
  // Returns -1 when the plugin is no longer installed.
  int rowFor(PluginFormat format, const std::string& uid) const;

  // Used by MixerModel to turn a picker row into a live plugin.
  std::unique_ptr<PluginInstance> instantiate(int row) const;
  const PluginDescriptor* descriptor(int row) const;

 signals:
  void countChanged();

 private:
  static QString format_name(PluginFormat format);

  std::vector<std::unique_ptr<PluginBackend>> backends_;
  struct Entry {
    PluginDescriptor descriptor;
    size_t backend_index;
  };
  std::vector<Entry> entries_;
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

 public:
  explicit PluginFilterModel(QObject* parent = nullptr);

  QString query() const { return query_; }
  void setQuery(const QString& query);

  // The row this proxy row stands for in the scan, which is what the mixer's
  // insert calls take.
  Q_INVOKABLE int sourceRow(int proxyRow) const;

 signals:
  void queryChanged();
  void countChanged();

 protected:
  bool filterAcceptsRow(int source_row,
                        const QModelIndex& source_parent) const override;

 private:
  QString query_;
};

}  // namespace nirbija
