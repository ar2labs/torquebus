// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The timeline: where in the recording we are, and a way to change it.
//
// Hand-painted rather than a QSlider, for one reason that matters and one that
// is only tidiness. The one that matters: a slider's value is an int, and a
// recording is nanoseconds - a twenty-minute log divided into 2^31 steps is
// fine, but a four-hour one is not, and the failure would be a handle that
// cannot reach certain seconds rather than anything that looks like a bug. The
// tidy one: the elapsed portion is drawn in the accent colour, which a QSlider
// only does through a style sheet that has to be kept in step with the theme.
//
// While a drag is in progress the widget shows *the finger*, not the replay:
// position updates arriving from the executor are ignored until the button
// comes back up. Otherwise the handle fights the person holding it.

#pragma once

#include <QWidget>
#include <QtGlobal>

namespace torquebus::ui {

class TimelineBar final : public QWidget {
    Q_OBJECT

public:
    explicit TimelineBar(QWidget* parent = nullptr);

    /// Total length of the recording, in nanoseconds. Zero means unknown - the
    /// bar then draws an empty track and refuses to be dragged, which is
    /// honest: there is nowhere to drag to.
    void setDurationNs(quint64 durationNs);

    [[nodiscard]] quint64 durationNs() const noexcept { return m_durationNs; }

    /// Where the replay has got to. Ignored while the user is dragging.
    void setPositionNs(quint64 positionNs);

    /// Where the handle is - the drag position while dragging, the replay's
    /// otherwise.
    [[nodiscard]] quint64 positionNs() const noexcept
    {
        return m_dragging ? m_dragPositionNs : m_positionNs;
    }

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

Q_SIGNALS:
    /// The user asked to continue from `positionNs`.
    ///
    /// Emitted continuously through a drag, not only when it ends: a seek
    /// request replaces any earlier one that has not been served, so the cost
    /// of a drag is the position the finger stopped at, and playing while
    /// scrubbing is what makes finding a moment possible at all.
    void seekRequested(quint64 positionNs);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    /// Where along the track a position sits, in widget coordinates.
    [[nodiscard]] int xForPosition(quint64 positionNs) const;

    /// What a click at `x` means, clamped to the recording.
    [[nodiscard]] quint64 positionForX(int x) const;

    /// The track, inset by the handle's radius so the handle stays inside the
    /// widget at both ends.
    [[nodiscard]] QRect trackRect() const;

    quint64 m_durationNs{0};
    quint64 m_positionNs{0};

    bool m_dragging{false};
    quint64 m_dragPositionNs{0};

    bool m_hovered{false};
};

} // namespace torquebus::ui
