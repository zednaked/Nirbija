// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "plugin_list_model.h"

#include <QString>
#include <QTimer>

#include <algorithm>
#include <chrono>

namespace nirbija {

PluginListModel::PluginListModel(QObject* parent) : QAbstractListModel(parent) {
  backends_ = make_all_backends();
  rescan();
}

PluginListModel::~PluginListModel() {
  // The worker holds raw pointers into backends_; it has to be done before
  // they go. A future from std::async joins in its destructor anyway, but
  // saying so here is what keeps the order obvious.
  if (pending_.valid()) pending_.wait();
}

void PluginListModel::rescan() {
  // Two scans over the same backends at once is not something they promise
  // to survive. The earlier one is short by now, or nearly.
  waitForScan(-1);

  const int generation = ++scan_generation_;
  scanning_ = true;
  emit scanningChanged();

  // The backends are only read by the worker while it runs; every other
  // path onto them (instantiate) waits for the scan first.
  auto* backends = &backends_;
  pending_ = std::async(std::launch::async, [backends] {
    std::vector<Entry> found;
    for (size_t i = 0; i < backends->size(); ++i)
      for (auto& descriptor : (*backends)[i]->scan())
        found.push_back({std::move(descriptor), i});

    // Name order, with the format as tie-breaker, so the same plugin in two
    // formats sits together in the picker.
    std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) {
      if (a.descriptor.name != b.descriptor.name)
        return a.descriptor.name < b.descriptor.name;
      return a.descriptor.format < b.descriptor.format;
    });
    return found;
  });

  // The model can only change on its own thread, so the worker's result is
  // picked up from the event loop. Checked on a short timer rather than
  // posted from the worker: a posted call would have to outlive a model
  // that may already be gone, and a timer parented here cannot.
  auto* watch = new QTimer(this);
  watch->setInterval(30);
  connect(watch, &QTimer::timeout, this, [this, watch, generation] {
    if (pending_.valid() &&
        pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return;
    watch->stop();
    watch->deleteLater();
    applyScan(generation);
  });
  watch->start();
}

bool PluginListModel::waitForScan(int milliseconds) {
  if (!pending_.valid()) return true;
  if (milliseconds < 0) {
    pending_.wait();
  } else if (pending_.wait_for(std::chrono::milliseconds(milliseconds)) !=
             std::future_status::ready) {
    return false;
  }
  applyScan(scan_generation_);
  return true;
}

void PluginListModel::applyScan(int generation) {
  if (generation <= applied_generation_ || !pending_.valid()) return;
  applied_generation_ = generation;

  beginResetModel();
  entries_ = pending_.get();
  endResetModel();
  scanning_ = false;
  emit countChanged();
  emit scanningChanged();
  emit scanFinished();
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
    case CategoryRole: return QString::fromStdString(descriptor.category);
    case KindRole: return static_cast<int>(descriptor.kind);
    default: return {};
  }
}

QHash<int, QByteArray> PluginListModel::roleNames() const {
  return {
      {NameRole, "name"},
      {VendorRole, "vendor"},
      {FormatRole, "format"},
      {UidRole, "uid"},
      {CategoryRole, "category"},
      {KindRole, "kind"},
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

std::unique_ptr<PluginInstance> PluginListModel::instantiate(int row) {
  waitForScan(-1);
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

// The QML enum and the core one have to agree, since the role carries a plain
// int across. Checked here rather than trusted.
static_assert(PluginFilterModel::Instrument ==
              static_cast<int>(PluginKind::Instrument));
static_assert(PluginFilterModel::Effect == static_cast<int>(PluginKind::Effect));
static_assert(PluginFilterModel::MidiEffect ==
              static_cast<int>(PluginKind::MidiEffect));
static_assert(PluginFilterModel::Analyzer ==
              static_cast<int>(PluginKind::Analyzer));
static_assert(PluginFilterModel::Utility ==
              static_cast<int>(PluginKind::Utility));
static_assert(PluginFilterModel::Unknown ==
              static_cast<int>(PluginKind::Unknown));

void PluginFilterModel::refilter() {
  // Only the rows are filtered here, so this is the right call. Qt deprecates
  // it in favour of begin/endFilterChange(), which the 6.5 floor this project
  // declares does not have - so the warning is silenced rather than the call
  // changed. Drop the pragmas once the minimum Qt is new enough.
  QT_WARNING_PUSH
  QT_WARNING_DISABLE_DEPRECATED
  invalidateRowsFilter();
  QT_WARNING_POP
  emit countChanged();
}

void PluginFilterModel::setQuery(const QString& query) {
  if (query_ == query) return;
  query_ = query;
  refilter();
  emit queryChanged();
}

void PluginFilterModel::setKind(int kind) {
  if (kind_ == kind) return;
  kind_ = kind;
  refilter();
  emit kindChanged();
}

int PluginFilterModel::sourceRow(int proxyRow) const {
  const QModelIndex proxy = index(proxyRow, 0);
  if (!proxy.isValid()) return -1;
  return mapToSource(proxy).row();
}

bool PluginFilterModel::filterAcceptsRow(int source_row,
                                         const QModelIndex& source_parent) const {
  const QAbstractItemModel* source = sourceModel();
  if (source == nullptr) return true;
  const QModelIndex index = source->index(source_row, 0, source_parent);

  // The kind is a gate, not another thing to match: picking "instrument" and
  // then typing has to search among instruments, not add synths back in.
  if (kind_ != AnyKind &&
      source->data(index, PluginListModel::KindRole).toInt() != kind_)
    return false;

  if (query_.isEmpty()) return true;

  // Name, maker, format and the plugin's own category all match, so "clap" or
  // "surge" or "reverb" or the vendor's name each narrow the list — a picker
  // where only the name matched meant knowing what a plugin was called before
  // you could find it.
  for (const int role : {PluginListModel::NameRole, PluginListModel::VendorRole,
                         PluginListModel::FormatRole,
                         PluginListModel::CategoryRole}) {
    if (source->data(index, role).toString().contains(query_,
                                                      Qt::CaseInsensitive))
      return true;
  }
  return false;
}

}  // namespace nirbija
