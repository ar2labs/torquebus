// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/trace/TraceModel.h"

#include "ui/theme/ThemeManager.h"

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QTimer>

namespace torquebus::ui {
namespace {

constexpr int kDefaultRefreshMs = 40; // 25 Hz

/// Formats a nanosecond timestamp as seconds with microsecond resolution -
/// "12.345678" - which is what every tool in this family shows and what an
/// engineer compares against an oscilloscope.
[[nodiscard]] QString formatTimestamp(std::uint64_t nanoseconds)
{
    const std::uint64_t microseconds = nanoseconds / 1000ULL;
    return QStringLiteral("%1.%2")
        .arg(microseconds / 1'000'000ULL)
        .arg(microseconds % 1'000'000ULL, 6, 10, QLatin1Char('0'));
}

/// Milliseconds with three decimals, or blank for zero.
///
/// Blank rather than "0.000" on purpose: zero means "not measured yet" - a
/// first sighting, or the first row - and printing a number there would read as
/// "arriving instantly", which is a different and wrong statement.
[[nodiscard]] QString formatInterval(std::uint32_t microseconds)
{
    if (microseconds == 0) {
        return {};
    }
    return QStringLiteral("%1.%2")
        .arg(microseconds / 1000U)
        .arg(microseconds % 1000U, 3, 10, QLatin1Char('0'));
}

[[nodiscard]] QString formatPayload(const CanFrame& frame)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";

    const std::size_t count = std::min<std::size_t>(frame.length, kMaxCanPayload);
    if (count == 0) {
        return {};
    }

    QString text;
    text.reserve(static_cast<qsizetype>(count * 3));

    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            text.append(QLatin1Char(' '));
        }
        const std::uint8_t byte = frame.data[index];
        text.append(QLatin1Char(kDigits[byte >> 4U]));
        text.append(QLatin1Char(kDigits[byte & 0x0FU]));
    }

    return text;
}

/// The short flag string an engineer scans for: error frames, remote frames,
/// bit rate switch, error state indicator.
[[nodiscard]] QString formatFlags(const CanFrame& frame)
{
    QString flags;
    if (frame.error) { flags.append(QStringLiteral("ERR ")); }
    if (frame.rtr)   { flags.append(QStringLiteral("RTR ")); }
    if (frame.brs)   { flags.append(QStringLiteral("BRS ")); }
    if (frame.esi)   { flags.append(QStringLiteral("ESI ")); }
    return flags.trimmed();
}

} // namespace

TraceModel::TraceModel(QObject* parent)
    : QAbstractTableModel{parent}
    , m_timer{new QTimer(this)}
{
    m_timer->setInterval(kDefaultRefreshMs);
    m_timer->setTimerType(Qt::CoarseTimer);
    connect(m_timer, &QTimer::timeout, this, &TraceModel::pollStore);
    m_timer->start();
}

TraceModel::~TraceModel() = default;

void TraceModel::setStore(const TraceStore* store)
{
    beginResetModel();
    m_store = store;
    m_visibleRows = 0;
    m_lastDiscarded = 0;
    endResetModel();
}

void TraceModel::setRefreshIntervalMs(int milliseconds)
{
    m_timer->setInterval(std::max(milliseconds, 1));
}

void TraceModel::setFrozen(bool frozen)
{
    m_frozen = frozen;

    // The timer keeps running while frozen so that unfreezing catches up in one
    // batch on the next tick, rather than waiting for a full interval.
    if (!frozen) {
        pollStore();
    }
}

void TraceModel::reset()
{
    beginResetModel();
    m_visibleRows = 0;
    m_lastDiscarded = m_store != nullptr ? m_store->discarded() : 0;
    endResetModel();
}

void TraceModel::pollStore()
{
    if (m_store == nullptr || m_frozen) {
        return;
    }

    const std::size_t available = m_store->size();
    const std::uint64_t discarded = m_store->discarded();

    // The ring wrapped: every row index the view is holding now refers to a
    // different frame. Shifting a million indices would cost more than the
    // reset and would be wrong in the middle of it, so the model resets.
    //
    // On a bounded trace this happens once, when the buffer first fills, and
    // then on every subsequent batch - which is why the trace visibly stops
    // growing and starts scrolling at capacity, and why the panel says how many
    // frames have fallen off.
    if (discarded != m_lastDiscarded) {
        m_lastDiscarded = discarded;
        beginResetModel();
        m_visibleRows = available;
        endResetModel();
        return;
    }

    if (available <= m_visibleRows) {
        return;
    }

    const int first = static_cast<int>(m_visibleRows);
    const int last = static_cast<int>(available) - 1;

    beginInsertRows({}, first, last);
    m_visibleRows = available;
    endInsertRows();

    Q_EMIT rowsAppended(first, last);
}

int TraceModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_visibleRows);
}

int TraceModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant TraceModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || m_store == nullptr) {
        return {};
    }

    const auto row = static_cast<std::size_t>(index.row());
    if (row >= m_visibleRows || row >= m_store->size()) {
        return {};
    }

    const TraceRow& entry = m_store->row(row);

    switch (role) {
    case Qt::DisplayRole:
        return textFor(entry, index.column());

    case Qt::ForegroundRole:
        return colourFor(entry, index.column());

    case Qt::TextAlignmentRole:
        // Numbers right, payload and text left. A column of right-aligned
        // times is scannable; a ragged one is not.
        switch (index.column()) {
        case Time:
        case Delta:
        case Cycle:
        case Count:
        case Dlc:
            return QVariant{Qt::AlignRight | Qt::AlignVCenter};
        default:
            return QVariant{Qt::AlignLeft | Qt::AlignVCenter};
        }

    case Qt::FontRole:
        // Monospace where digits must line up between rows.
        switch (index.column()) {
        case Time:
        case Delta:
        case Cycle:
        case Identifier:
        case Data:
            return QFontDatabase::systemFont(QFontDatabase::FixedFont);
        default:
            return {};
        }

    default:
        return {};
    }
}

QString TraceModel::textFor(const TraceRow& row, int column) const
{
    const CanFrame& frame = row.frame;

    switch (column) {
    case Time:
        return formatTimestamp(frame.timestampNs);

    case Delta:
        return formatInterval(row.deltaUs);

    case Channel:
        return QStringLiteral("CAN %1").arg(frame.channel + 1);

    case Direction:
        return frame.isRx() ? QStringLiteral("Rx") : QStringLiteral("Tx");

    case Identifier:
        return QString::fromStdString(toIdentifierString(frame));

    case Name:
        // Filled by the DBC decoder from v0.8. Blank rather than a placeholder:
        // an empty cell reads as "no database loaded", "-" reads as "no name",
        // and those are different.
        return {};

    case Type: {
        QString type = frame.isExtended() ? QStringLiteral("EXT") : QStringLiteral("STD");
        if (frame.fd) {
            type.append(QStringLiteral(" FD"));
        }
        return type;
    }

    case Dlc:
        return QString::number(frame.dlc);

    case Data:
        return formatPayload(frame);

    case Cycle:
        return formatInterval(row.cycleUs);

    case Count:
        return QString::number(row.occurrence);

    case Flags:
        return formatFlags(frame);

    default:
        return {};
    }
}

QVariant TraceModel::colourFor(const TraceRow& row, int column) const
{
    const ThemeManager* themes = ThemeManager::instance();
    if (themes == nullptr) {
        return {};
    }

    const Theme& theme = themes->theme();

    // An error frame colours the whole row: it is not a property of one cell,
    // and it is the thing the eye should catch while scrolling.
    if (row.frame.error) {
        return theme.error;
    }

    switch (column) {
    case Direction:
    case Identifier:
        // Direction is what the trace is scanned by, so it carries the colour.
        return theme.directionColor(!row.frame.isRx());

    case Delta:
    case Cycle:
    case Count:
        // Derived numbers are supporting information, not the data itself.
        return theme.textMuted;

    default:
        return {};
    }
}

QVariant TraceModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    switch (section) {
    case Time:       return tr("Time");
    case Delta:      return tr("Delta");
    case Channel:    return tr("Ch");
    case Direction:  return tr("Dir");
    case Identifier: return tr("ID");
    case Name:       return tr("Name");
    case Type:       return tr("Type");
    case Dlc:        return tr("DLC");
    case Data:       return tr("Data");
    case Cycle:      return tr("Cycle");
    case Count:      return tr("Count");
    case Flags:      return tr("Flags");
    default:         return {};
    }
}

} // namespace torquebus::ui
