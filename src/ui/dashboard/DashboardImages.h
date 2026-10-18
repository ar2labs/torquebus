// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The pictures of a dashboard, loaded once and drawn many times.
//
// A widget names its picture as DashboardAssets.h stores it - relative to the project's folder when
// it can be - and the panel repaints twenty times a second. Between those two is a cache: a path is
// resolved and read once, and what was read is kept until the file changes. Without it a lamp with
// a PNG would decode that PNG on every frame it was lit.
//
// Three things here are about the file being wrong rather than right, because that is what a
// picture path is more often than a database one: a picture can be missing (the project was moved,
// or the file deleted), unreadable (it is not a picture, or is truncated), or replaced while the
// program is running (somebody saved a new version over it in an editor). The first two are
// reported as a reason a person can read, drawn by the widget as a placeholder rather than as
// nothing; the third is noticed within a couple of seconds, so the editor and the dashboard do not
// disagree about what the file is.
//
// Raster pictures are drawn smoothly and kept at a sensible size - an 8000-pixel photograph of a
// truck does not belong in the memory of a gauge. SVG is drawn by the renderer at the size it is
// asked for, which is what makes it the right format for an icon that is going to be scaled.

#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QSize>
#include <QString>

#include <memory>
#include <string>
#include <unordered_map>

class QPainter;
class QRectF;
class QSvgRenderer;

namespace torquebus::ui {

class DashboardImages final {
public:
    DashboardImages();
    ~DashboardImages();

    DashboardImages(const DashboardImages&) = delete;
    DashboardImages& operator=(const DashboardImages&) = delete;

    /// The project's folder, which a relative path is joined to. Empty for a project that has none.
    /// Changing it forgets everything: a relative path is not the same file in another folder.
    void setBaseDirectory(const QString& directory);

    [[nodiscard]] const QString& baseDirectory() const noexcept { return m_base; }

    /// Draws `path` - as a widget writes it - inside `target`, as large as it goes without changing
    /// its proportions and centred, at `opacity`. False, and nothing drawn, when there is no such
    /// picture; problemWith() says why.
    bool
    draw(QPainter& painter, const QRectF& target, const std::string& path, qreal opacity = 1.0);

    /// Why `path` cannot be drawn, or empty when it can (or is empty).
    [[nodiscard]] QString problemWith(const std::string& path);

    /// The picture's own size in pixels, or an empty size when it cannot be drawn.
    [[nodiscard]] QSize sizeOf(const std::string& path);

    /// Where `path` is on disk, as the cache resolves it.
    [[nodiscard]] QString absolutePathOf(const std::string& path) const;

    /// Forgets every picture, so that the next draw reads the files again.
    void clear();

private:
    struct Entry;

    /// The cached entry for `path`, loaded if it is new and read again if the file has changed
    /// since the last time it was looked at.
    Entry& lookup(const std::string& path);

    void load(Entry& entry, const QString& file);

    QString m_base;
    QElapsedTimer m_clock;
    std::unordered_map<std::string, std::unique_ptr<Entry>> m_entries;
};

} // namespace torquebus::ui
