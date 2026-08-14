#pragma once

#include <QAbstractListModel>

#include <memory>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// Every plugin the compiled-in backends can see, scanned once at startup.
// Scanning walks the disk and dlopens modules, so it never happens on demand
// from a QML binding.
class PluginListModel : public QAbstractListModel {
  Q_OBJECT
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

}  // namespace nirbija
