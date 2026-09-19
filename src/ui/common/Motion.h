// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The one animation primitive the chrome is built from.
//
// Qt style sheets have no transitions. Every animated state in this window is
// therefore a value that eases toward a target while a widget repaints itself -
// which is a small amount of machinery, and worth having exactly once rather
// than four times.
//
// **Nothing here may touch the frame path.** These animate chrome: tabs,
// buttons, the dividers between panels. The trace and the pipeline canvas never
// animate, so a measurement at two hundred thousand frames a second pays
// nothing for any of it. A widget that repaints at 60 Hz while the bus is
// saturated would be spending the exact budget the pipeline was designed to
// protect.

#pragma once

#include <QColor>
#include <QEasingCurve>
#include <QObject>
#include <QVariant>
#include <QVariantAnimation>
#include <QWidget>

namespace torquebus::ui {

/// A number between 0 and 1 that slides to its target instead of jumping.
///
/// Held by value inside the widget it animates. The widget repaints on every
/// step, so the animation runs only while something is actually moving - there
/// is no timer ticking behind an idle window.
class Motion final {
public:
    /// `owner` is repainted on each step and owns this object's lifetime.
    ///
    /// 130 ms is the standard here: long enough to read as movement rather
    /// than a jump, short enough that it never delays a click. Anything past
    /// about 200 ms starts to feel like the application is thinking.
    explicit Motion(QWidget* owner, int durationMs = 130)
    {
        m_animation.setDuration(durationMs);

        // Out-easing: fast at the start, settling at the end. Movement that
        // begins immediately reads as responsive; movement that begins slowly
        // reads as lag, whatever its total duration.
        m_animation.setEasingCurve(QEasingCurve::OutCubic);
        m_animation.setStartValue(0.0);
        m_animation.setEndValue(0.0);

        QObject::connect(
            &m_animation, &QVariantAnimation::valueChanged, owner, [owner] { owner->update(); });
    }

    Motion(const Motion&) = delete;
    Motion& operator=(const Motion&) = delete;

    /// Eases toward `target` from wherever the value currently is.
    ///
    /// Restarting from the current value, not from the last target, is what
    /// makes a cursor swept quickly across several tabs look continuous rather
    /// than jumping back to zero at each boundary.
    void setTarget(double target)
    {
        if (qFuzzyCompare(target + 1.0, m_target + 1.0)) {
            return;
        }

        const double from = value();
        m_target = target;

        m_animation.stop();
        m_animation.setStartValue(from);
        m_animation.setEndValue(target);
        m_animation.start();
    }

    /// Jumps to `target` with no animation. For the first paint, where there is
    /// no previous state to move from and easing in from zero would look like
    /// a glitch.
    void jumpTo(double target)
    {
        m_animation.stop();
        m_target = target;
        m_animation.setStartValue(target);
        m_animation.setEndValue(target);
    }

    /// Runs from 0 to 1 once, for a one-shot effect such as a click ripple.
    void restart(int durationMs)
    {
        m_animation.stop();
        m_animation.setDuration(durationMs);
        m_target = 1.0;
        m_animation.setStartValue(0.0);
        m_animation.setEndValue(1.0);
        m_animation.start();
    }

    [[nodiscard]] double value() const
    {
        const QVariant current = m_animation.currentValue();
        return current.isValid() ? current.toDouble() : m_target;
    }

    [[nodiscard]] bool isRunning() const
    {
        return m_animation.state() == QAbstractAnimation::Running;
    }

private:
    QVariantAnimation m_animation;
    double m_target{0.0};
};

/// Linear interpolation between two colours, in straight RGB.
///
/// Not a perceptual blend. The distances involved here are a few points of
/// luminance between two neighbouring theme colours, where a perceptual space
/// would cost more than the difference it makes.
[[nodiscard]] inline QColor mix(const QColor& from, const QColor& to, double amount)
{
    const auto blend = [amount](float a, float b) {
        return static_cast<float>(a + (b - a) * amount);
    };

    return QColor::fromRgbF(blend(from.redF(), to.redF()),
                            blend(from.greenF(), to.greenF()),
                            blend(from.blueF(), to.blueF()));
}

} // namespace torquebus::ui
