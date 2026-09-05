// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A thumbnail of what a theme looks like, drawn from that theme's own colours.
//
// Drawn rather than shipped as an image, for two reasons and the second is the
// important one. It is a handful of rectangles, so a bitmap would be more bytes
// for less. And because it paints from Theme::forVariant(), it cannot go stale:
// change a surface colour in Theme.cpp and the card in Preferences shows the
// new one on the next build, with nobody having to remember that a screenshot
// somewhere needs retaking.
//
// It also shows the *chosen accent*, which is what makes the two cards useful
// side by side: picking a hue repaints both, and the difference between the
// luminous version and the deep one is visible before you commit to either.

#pragma once

#include "ui/common/Motion.h"
#include "ui/theme/AccentColor.h"
#include "ui/theme/Theme.h"

#include <QWidget>

namespace torquebus::ui {

class ThemeCard final : public QWidget {
    Q_OBJECT

public:
    explicit ThemeCard(ThemeVariant variant, QWidget* parent = nullptr);

    [[nodiscard]] ThemeVariant variant() const noexcept { return m_variant; }

    /// Draws the card with this accent, so the two cards preview the choice.
    void setAccent(AccentColor accent);

    /// Marks this card as the theme in use. Purely visual; clicking is what
    /// actually changes anything.
    void setSelected(bool selected);
    [[nodiscard]] bool isSelected() const noexcept { return m_selected; }

Q_SIGNALS:
    void clicked(torquebus::ui::ThemeVariant variant);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

    [[nodiscard]] QSize sizeHint() const override;

private:
    ThemeVariant m_variant;
    AccentColor m_accent{AccentColor::TorqueBus};
    bool m_selected{false};
    bool m_pressed{false};

    /// Hover and selection are animated rather than switched, for the same
    /// reason the tab marker slides: a border that appears is a different event
    /// from one that grows, and only the second reads as the card responding to
    /// you. See Motion.h.
    Motion m_hover;
    Motion m_ring;
};

} // namespace torquebus::ui
