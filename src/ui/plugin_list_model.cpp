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

}  // namespace nirbija
