// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The Playback panel: the transport for a recorded measurement.
//
// A replay block on the canvas says *what* is played. This says how it is
// played - stopped at the interesting second, taken back ten, run at a tenth
// speed to watch a burst, or run at maximum to reach minute nineteen of a
// twenty minute log.
//
// It talks to the executor thread through a ReplayControl and nothing else:
// atomics in, atomics out, no lock shared with the frame path (rule #5). What
// this panel does on its timer is read four numbers and repaint; what a button
// does is store one. Neither ever waits for the pipeline, and the pipeline
// never waits for the window.
//
// The panel is deliberately useless when nothing is being replayed: no file
// means the buttons are disabled and the panel says why, rather than offering a
// transport that quietly does nothing.

#pragma once

#include <QString>
#include <QWidget>
#include <QtGlobal>

class QComboBox;
class QLabel;
class QToolButton;

namespace torquebus {
class ReplayControl;
}

namespace torquebus::ui {

class TimelineBar;

class PlaybackPanel final : public QWidget {
    Q_OBJECT

public:
    explicit PlaybackPanel(QWidget* parent = nullptr);

    /// Attaches the transport this panel drives. Not owned; must outlive the
    /// panel. Null detaches it, which is what a measurement with no replay
    /// block looks like.
    void setControl(ReplayControl* control);

    /// Names what is being played, for the label above the bar. Empty when
    /// nothing is.
    void setSourceName(const QString& name);

private Q_SLOTS:
    /// Reads the control and repaints. The panel's whole clock.
    void refresh();

    void onPlayPauseClicked();
    void onSkipBackClicked();
    void onSkipForwardClicked();
    void onRestartClicked();
    void onSpeedChanged(int index);
    void onSeekRequested(quint64 positionNs);
    void onThemeChanged();

private:
    void buildUi();

    /// Enables or disables the transport as a whole, and says why when it is
    /// off.
    void updateAvailability();

    ReplayControl* m_control{nullptr};

    QLabel* m_sourceLabel{nullptr};
    QLabel* m_positionLabel{nullptr};
    QLabel* m_stateLabel{nullptr};

    QToolButton* m_playPause{nullptr};
    QToolButton* m_skipBack{nullptr};
    QToolButton* m_skipForward{nullptr};
    QToolButton* m_restart{nullptr};

    QComboBox* m_speed{nullptr};
    TimelineBar* m_timeline{nullptr};

    /// What the button currently shows, so the icon is only swapped when the
    /// state actually changed rather than twenty times a second.
    bool m_shownAsPaused{false};

    /// Whether the transport was last drawn as available, for the same reason.
    bool m_shownAsActive{false};
};

} // namespace torquebus::ui
