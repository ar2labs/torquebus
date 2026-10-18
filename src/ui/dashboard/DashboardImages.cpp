// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/dashboard/DashboardImages.h"

#include "core/dashboard/DashboardAssets.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QObject>
#include <QPainter>
#include <QRectF>
#include <QSvgRenderer>

namespace torquebus::ui {
namespace {

/// How often a file is asked whether it has changed. Often enough that a picture saved over in an
/// editor shows within a couple of seconds, rarely enough that a dashboard of twenty pictures does
/// not stat twenty files twenty times a second.
constexpr qint64 kRecheckMs = 2000;

/// The longest side a raster picture is kept at. A gauge is never drawn at this size, and a
/// photograph is not a thing to hold in memory at its own.
constexpr int kLongestSide = 1600;

[[nodiscard]] bool isSvg(const QString& file)
{
    return file.endsWith(QLatin1String{".svg"}, Qt::CaseInsensitive);
}

/// `size` fitted inside `target` without changing its proportions, centred.
[[nodiscard]] QRectF fitted(const QSizeF& size, const QRectF& target)
{
    if (size.isEmpty() || target.isEmpty()) {
        return {};
    }

    QRectF result{QPointF{}, size.scaled(target.size(), Qt::KeepAspectRatio)};
    result.moveCenter(target.center());
    return result;
}

} // namespace

struct DashboardImages::Entry {
    QImage image;
    std::unique_ptr<QSvgRenderer> svg;

    /// Empty when the picture loaded.
    QString problem;
    QSize size;

    /// When the file was last written, and whether it was there: what a re-check compares.
    QDateTime modified;
    bool existed{false};

    /// The clock reading at which the file was last looked at.
    qint64 checkedAt{0};
};

DashboardImages::DashboardImages()
{
    m_clock.start();
}

DashboardImages::~DashboardImages() = default;

void DashboardImages::setBaseDirectory(const QString& directory)
{
    if (directory == m_base) {
        return;
    }

    m_base = directory;
    clear();
}

void DashboardImages::clear()
{
    m_entries.clear();
}

QString DashboardImages::absolutePathOf(const std::string& path) const
{
    return QString::fromStdString(resolveAsset(path, m_base.toStdString()));
}

DashboardImages::Entry& DashboardImages::lookup(const std::string& path)
{
    auto found = m_entries.find(path);

    if (found == m_entries.end()) {
        found = m_entries.emplace(path, std::make_unique<Entry>()).first;
        load(*found->second, absolutePathOf(path));
        found->second->checkedAt = m_clock.elapsed();
        return *found->second;
    }

    Entry& entry = *found->second;

    if (m_clock.elapsed() - entry.checkedAt >= kRecheckMs) {
        entry.checkedAt = m_clock.elapsed();

        const QString file = absolutePathOf(path);
        const QFileInfo info{file};

        if (info.exists() != entry.existed || info.lastModified() != entry.modified) {
            load(entry, file);
        }
    }

    return entry;
}

void DashboardImages::load(Entry& entry, const QString& file)
{
    entry.image = QImage{};
    entry.svg.reset();
    entry.problem.clear();
    entry.size = QSize{};

    const QFileInfo info{file};
    entry.existed = info.exists();
    entry.modified = info.lastModified();

    if (!info.exists() || !info.isFile()) {
        entry.problem = QObject::tr("No such file: %1").arg(QDir::toNativeSeparators(file));
        return;
    }

    if (isSvg(file)) {
        auto renderer = std::make_unique<QSvgRenderer>(file);

        if (!renderer->isValid() || renderer->defaultSize().isEmpty()) {
            entry.problem = QObject::tr("Not a usable SVG: %1").arg(QDir::toNativeSeparators(file));
            return;
        }

        entry.size = renderer->defaultSize();
        entry.svg = std::move(renderer);
        return;
    }

    QImageReader reader{file};
    reader.setAutoTransform(true);

    // Scaled while it is read, so that a very large picture is never held at its own size.
    if (const QSize original = reader.size();
        original.isValid()
        && (original.width() > kLongestSide || original.height() > kLongestSide)) {
        reader.setScaledSize(original.scaled(kLongestSide, kLongestSide, Qt::KeepAspectRatio));
    }

    QImage image = reader.read();

    if (image.isNull()) {
        entry.problem = QObject::tr("Cannot read %1: %2")
                            .arg(QDir::toNativeSeparators(file), reader.errorString());
        return;
    }

    entry.size = image.size();
    entry.image = std::move(image);
}

QString DashboardImages::problemWith(const std::string& path)
{
    if (path.empty()) {
        return {};
    }

    return lookup(path).problem;
}

QSize DashboardImages::sizeOf(const std::string& path)
{
    if (path.empty()) {
        return {};
    }

    return lookup(path).size;
}

bool DashboardImages::draw(QPainter& painter,
                           const QRectF& target,
                           const std::string& path,
                           qreal opacity)
{
    if (path.empty()) {
        return false;
    }

    const Entry& entry = lookup(path);

    if (!entry.problem.isEmpty() || entry.size.isEmpty()) {
        return false;
    }

    const QRectF where = fitted(QSizeF{entry.size}, target);

    if (where.isEmpty()) {
        return false;
    }

    painter.save();
    painter.setOpacity(painter.opacity() * opacity);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (entry.svg) {
        entry.svg->render(&painter, where);
    } else {
        painter.drawImage(where, entry.image);
    }

    painter.restore();
    return true;
}

} // namespace torquebus::ui
