#include "plugin_list_model.h"

#include <QString>

#include <algorithm>

namespace nirbija {

PluginListModel::PluginListModel(QObject* parent) : QAbstractListModel(parent) {
  backends_ = make_all_backends();
  rescan();
}

void PluginListModel::rescan() {
  beginResetModel();
  entries_.clear();
  for (size_t i = 0; i < backends_.size(); ++i)
    for (auto& descriptor : backends_[i]->scan())
      entries_.push_back({std::move(descriptor), i});

  // Name order, with the format as tie-breaker, so the same plugin in two
  // formats sits together in the picker.
  std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
    if (a.descriptor.name != b.descriptor.name)
      return a.descriptor.name < b.descriptor.name;
    return a.descriptor.format < b.descriptor.format;
  });
  endResetModel();
  emit countChanged();
}

int PluginListModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;
  return static_cast<int>(entries_.size());
}

QString PluginListModel::format_name(PluginFormat format) {
  switch (format) {
    case PluginFormat::Lv2: return QStringLiteral("LV2");
    case PluginFormat::Clap: return QStringLiteral("CLAP");
    case PluginFormat::Vst3: return QStringLiteral("VST3");
    case PluginFormat::Internal: return QStringLiteral("Built-in");
  }
  return {};
}

QVariant PluginListModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= static_cast<int>(entries_.size())) return {};
  const PluginDescriptor& descriptor = entries_[index.row()].descriptor;
  switch (role) {
    case NameRole: return QString::fromStdString(descriptor.name);
    case VendorRole: return QString::fromStdString(descriptor.vendor);
    case FormatRole: return format_name(descriptor.format);
    case UidRole: return QString::fromStdString(descriptor.uid);
    default: return {};
  }
}

QHash<int, QByteArray> PluginListModel::roleNames() const {
  return {
      {NameRole, "name"},
      {VendorRole, "vendor"},
      {FormatRole, "format"},
      {UidRole, "uid"},
  };
}

int PluginListModel::rowFor(PluginFormat format, const std::string& uid) const {
  for (size_t i = 0; i < entries_.size(); ++i) {
    const PluginDescriptor& descriptor = entries_[i].descriptor;
    if (descriptor.format == format && descriptor.uid == uid)
      return static_cast<int>(i);
  }
  return -1;
}

const PluginDescriptor* PluginListModel::descriptor(int row) const {
  if (row < 0 || row >= static_cast<int>(entries_.size())) return nullptr;
  return &entries_[row].descriptor;
}

std::unique_ptr<PluginInstance> PluginListModel::instantiate(int row) const {
  if (row < 0 || row >= static_cast<int>(entries_.size())) return nullptr;
  const Entry& entry = entries_[row];
  return backends_[entry.backend_index]->instantiate(entry.descriptor);
}

// --- PluginFilterModel -------------------------------------------------------

PluginFilterModel::PluginFilterModel(QObject* parent)
    : QSortFilterProxyModel(parent) {
  connect(this, &QAbstractItemModel::rowsInserted, this,
          &PluginFilterModel::countChanged);
  connect(this, &QAbstractItemModel::rowsRemoved, this,
          &PluginFilterModel::countChanged);
  connect(this, &QAbstractItemModel::modelReset, this,
          &PluginFilterModel::countChanged);
}

void PluginFilterModel::setQuery(const QString& query) {
  if (query_ == query) return;
  query_ = query;
  // Only the rows are filtered here, so this is the right call. Qt deprecates
  // it in favour of begin/endFilterChange(), which the 6.5 floor this project
  // declares does not have - so the warning is silenced rather than the call
  // changed. Drop the pragmas once the minimum Qt is new enough.
  QT_WARNING_PUSH
  QT_WARNING_DISABLE_DEPRECATED
  invalidateRowsFilter();
  QT_WARNING_POP
  emit queryChanged();
  emit countChanged();
}

int PluginFilterModel::sourceRow(int proxyRow) const {
  const QModelIndex proxy = index(proxyRow, 0);
  if (!proxy.isValid()) return -1;
  return mapToSource(proxy).row();
}

bool PluginFilterModel::filterAcceptsRow(int source_row,
                                         const QModelIndex& source_parent) const {
  if (query_.isEmpty()) return true;
  const QAbstractItemModel* source = sourceModel();
  if (source == nullptr) return true;

  // Name, maker and format all match, so "clap" or "surge" or the vendor's name
  // each narrow the list — a picker where only the name matched meant knowing
  // what a plugin was called before you could find it.
  const QModelIndex index = source->index(source_row, 0, source_parent);
  for (const int role : {PluginListModel::NameRole, PluginListModel::VendorRole,
                         PluginListModel::FormatRole}) {
    if (source->data(index, role).toString().contains(query_,
                                                      Qt::CaseInsensitive))
      return true;
  }
  return false;
}

}  // namespace nirbija
