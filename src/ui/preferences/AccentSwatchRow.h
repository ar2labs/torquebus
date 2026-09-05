// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The row of coloured circles that picks the accent.
//
// One widget for the whole row rather than eight small ones. Eight widgets
// would each need their own hover state, their own focus handling and their own
// place in the tab order, and the thing a user actually navigates is a row: one
// stop on the way through the dialog, arrow keys between the swatches. That is
// how a group of radio buttons behaves, and this is a group of radio buttons
// that happens to be drawn as circles.
//
// Each swatch is painted in the colour the accent will actually take on the
// theme that is running - not in a nominal red or blue. What you click is what
// you get, which for a palette derived per theme (AccentColor.h) is a claim
// worth being careful about.

#pragma once

#include "ui/common/Motion.h"
#include "ui/theme/AccentColor.h"

#include <QWidget>

namespace torquebus::ui {

class AccentSwatchRow final : public QWidget {
    Q_OBJECT

public:
    explicit AccentSwatchRow(QWidget* parent = nullptr);

    [[nodiscard]] AccentColor accent() const noexcept { return m_accent; }

    /// Moves the selection ring without emitting accentChanged.
    ///
    /// For the caller setting the row up, and for Reset. A setter that emitted
    /// would have the dialog answering its own question, which is how a revert
    /// on Cancel turns into a loop.
    void setAccent(AccentColor accent);

Q_SIGNALS:
    /// Emitted only for a change the user made.
    void accentChanged(torquebus::ui::AccentColor accent);

public:
    /// Public because QWidget's are: a layout asks for these through a base
    /// pointer, and so does the dialog when it wants a caption the same width
    /// as the row. Narrowing an override compiles right up until somebody
    /// calls it on the derived type - the worst place for the error to show up,
    /// because the declaration that caused it is nowhere near the call.
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    /// Centre of swatch `index`.
    [[nodiscard]] QPointF centreOf(int index) const;

    /// The swatch under `position`, or -1.
    [[nodiscard]] int indexAt(const QPointF& position) const;

    void choose(int index);

    AccentColor m_accent{AccentColor::TorqueBus};
    int m_hovered{-1};

    /// The ring slides from the swatch it was on to the one it is going to,
    /// which is what says the two are alternatives in one row rather than eight
    /// independent lights. Same idea as the marker under the dock tabs.
    Motion m_ring;

    /// Where the ring is coming *from*, in swatch coordinates. Recorded because
    /// m_accent has already changed by the time the animation starts.
    double m_ringFrom{0.0};

    /// The press ripple, and which swatch it belongs to.
    ///
    /// One, not one per swatch: a mouse presses a single circle at a time, and
    /// eight animations to express that would be seven more than the fact
    /// requires.
    Motion m_ripple;
    int m_rippleIndex{-1};
};

} // namespace torquebus::ui
