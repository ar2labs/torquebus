// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/playback/PlaybackPanel.h"

#include "core/log/ReplayControl.h"
#include "ui/playback/TimelineBar.h"
#include "ui/theme/ThemeManager.h"

#include <QComboBox>
#include <QIcon>
#include <QSignalBlocker>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QString>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <array>

namespace torquebus::ui {
namespace {

/// 20 Hz, the same as the Graph panel. A position readout that ticks slower
/// than this looks stuck; one that ticks faster is repainting digits nobody can
/// read.
constexpr int kRefreshMs = 50;

/// What the skip buttons move. Ten seconds is the industry's habit and it is
/// the right one: it is long enough to be worth a button and short enough that
/// pressing it twice is still aiming.
constexpr quint64 kSkipNs = 10'000'000'000ULL;

struct SpeedChoice final {
    const char* label;
    double multiplier;
};

/// The speeds offered, ending where every playback menu ends.
constexpr std::array<SpeedChoice, 8> kSpeeds{{
    {"0.1x", 0.1},
    {"0.25x", 0.25},
    {"0.5x", 0.5},
    {"1x", 1.0},
    {"2x", 2.0},
    {"5x", 5.0},
    {"10x", 10.0},
    {"Maximum", ReplayControl::kUnlimitedSpeed},
}};

/// Fixed width, so the row does not shift sideways as the digits change.
///
/// hh:mm:ss.mmm rather than the raw seconds a log carries: a fault at 1284
/// seconds is a number somebody has to divide, and a fault at 00:21:24 is one
/// they can say out loud to the person sitting in the vehicle.
[[nodiscard]] QString formatTime(quint64 nanoseconds)
{
    const quint64 milliseconds = nanoseconds / 1'000'000ULL;

    const quint64 hours = milliseconds / 3'600'000ULL;
    const quint64 minutes = (milliseconds / 60'000ULL) % 60ULL;
    const quint64 seconds = (milliseconds / 1000ULL) % 60ULL;
    const quint64 remainder = milliseconds % 1000ULL;

    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(remainder, 3, 10, QLatin1Char('0'));
}

} // namespace

PlaybackPanel::PlaybackPanel(QWidget* parent)
    : QWidget{parent}
{
    buildUi();

    auto* timer = new QTimer(this);
    timer->setInterval(kRefreshMs);
    connect(timer, &QTimer::timeout, this, &PlaybackPanel::refresh);
    timer->start();

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { onThemeChanged(); });
    }

    updateAvailability();
}

void PlaybackPanel::buildUi()
{
    const auto icon = [](const char* name) {
        if (const ThemeManager* themes = ThemeManager::instance()) {
            return themes->icon(QString::fromLatin1(name));
        }
        return QIcon{};
    };

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(6);

    // --- What is playing --------------------------------------------------
    m_sourceLabel = new QLabel;
    m_sourceLabel->setProperty("torquebusRole", QStringLiteral("caption"));
    m_sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_sourceLabel);

    // --- The bar ----------------------------------------------------------
    m_timeline = new TimelineBar;
    connect(m_timeline, &TimelineBar::seekRequested, this, &PlaybackPanel::onSeekRequested);
    layout->addWidget(m_timeline);

    // --- Transport --------------------------------------------------------
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);

    const auto button = [&icon](const char* iconName, const QString& tip) {
        auto* control = new QToolButton;
        control->setIcon(icon(iconName));
        control->setToolTip(tip);
        control->setAutoRaise(true);
        return control;
    };

    m_restart = button("replay", tr("Back to the start"));
    connect(m_restart, &QToolButton::clicked, this, &PlaybackPanel::onRestartClicked);
    row->addWidget(m_restart);

    m_skipBack = button("skip-back", tr("Back ten seconds"));
    connect(m_skipBack, &QToolButton::clicked, this, &PlaybackPanel::onSkipBackClicked);
    row->addWidget(m_skipBack);

    m_playPause = button("pause", tr("Pause the replay"));
    connect(m_playPause, &QToolButton::clicked, this, &PlaybackPanel::onPlayPauseClicked);
    row->addWidget(m_playPause);

    m_skipForward = button("skip-forward", tr("Forward ten seconds"));
    connect(m_skipForward, &QToolButton::clicked, this, &PlaybackPanel::onSkipForwardClicked);
    row->addWidget(m_skipForward);

    row->addSpacing(8);

    // --- Position ---------------------------------------------------------
    m_positionLabel = new QLabel;
    m_positionLabel->setProperty("torquebusRole", QStringLiteral("metric"));
    m_positionLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    row->addWidget(m_positionLabel);

    row->addStretch(1);

    m_stateLabel = new QLabel;
    m_stateLabel->setProperty("torquebusRole", QStringLiteral("caption"));
    row->addWidget(m_stateLabel);

    row->addSpacing(8);

    m_speed = new QComboBox;
    m_speed->setToolTip(tr("How much faster than real time the recording plays."));

    for (const SpeedChoice& choice : kSpeeds) {
        m_speed->addItem(QString::fromLatin1(choice.label), choice.multiplier);
    }

    m_speed->setCurrentIndex(3); // 1x
    connect(m_speed, &QComboBox::currentIndexChanged, this, &PlaybackPanel::onSpeedChanged);
    row->addWidget(m_speed);

    layout->addLayout(row);
    layout->addStretch(1);
}

void PlaybackPanel::setControl(ReplayControl* control)
{
    m_control = control;

    if (m_control != nullptr) {
        // The block's own Speed parameter reached the control at build time, so
        // the panel starts from what the project said rather than snapping it
        // to 1x the moment a measurement starts.
        const double speed = m_control->speed();

        for (int index = 0; index < static_cast<int>(kSpeeds.size()); ++index) {
            if (qFuzzyCompare(kSpeeds[static_cast<std::size_t>(index)].multiplier, speed)) {
                const QSignalBlocker blocker{m_speed};
                m_speed->setCurrentIndex(index);
                break;
            }
        }
    }

    refresh();
    updateAvailability();
}

void PlaybackPanel::setSourceName(const QString& name)
{
    m_sourceLabel->setText(name.isEmpty() ? tr("No recording is being replayed.")
                                          : tr("Replaying %1").arg(name));
}

void PlaybackPanel::updateAvailability()
{
    const bool active = m_control != nullptr && m_control->isActive();

    for (QWidget* control : {static_cast<QWidget*>(m_playPause), static_cast<QWidget*>(m_skipBack),
                             static_cast<QWidget*>(m_skipForward),
                             static_cast<QWidget*>(m_restart), static_cast<QWidget*>(m_speed),
                             static_cast<QWidget*>(m_timeline)}) {
        control->setEnabled(active);
    }

    if (!active) {
        // Says what is missing rather than showing four buttons that do
        // nothing. The block is the answer to "why can I not press play".
        m_stateLabel->setText(tr("Add a Log Replay block and press Start."));
        m_positionLabel->setText(formatTime(0) + QStringLiteral(" / ") + formatTime(0));
        m_timeline->setDurationNs(0);
        m_timeline->setPositionNs(0);
    }

    m_shownAsActive = active;
}

void PlaybackPanel::refresh()
{
    if (m_control == nullptr) {
        return;
    }

    const bool active = m_control->isActive();

    if (active != m_shownAsActive) {
        updateAvailability();
    }

    if (!active) {
        return;
    }

    // Hidden panels are not repainted, so there is nothing to update - but the
    // control is read anyway above, because becoming active while hidden is
    // what happens on every Start with this dock behind another tab.
    if (!isVisible()) {
        return;
    }

    const quint64 position = m_control->positionNs();
    const quint64 duration = m_control->durationNs();

    m_timeline->setDurationNs(duration);
    m_timeline->setPositionNs(position);

    m_positionLabel->setText(formatTime(position) + QStringLiteral(" / ")
                             + formatTime(duration));

    const bool paused = m_control->isPaused();

    if (paused != m_shownAsPaused) {
        const auto icon = [](const char* name) {
            if (const ThemeManager* themes = ThemeManager::instance()) {
                return themes->icon(QString::fromLatin1(name));
            }
            return QIcon{};
        };

        // The button shows what pressing it does, which is the convention every
        // player follows and the opposite of showing the current state.
        m_playPause->setIcon(icon(paused ? "start" : "pause"));
        m_playPause->setToolTip(paused ? tr("Resume the replay") : tr("Pause the replay"));

        m_shownAsPaused = paused;
    }

    if (m_control->finished()) {
        // Distinguished from paused on purpose: "the file ended" and "you
        // stopped it" look identical on a bar that has stopped moving, and only
        // one of them is answered by pressing play.
        m_stateLabel->setText(tr("End of recording"));
    } else {
        m_stateLabel->setText(paused ? tr("Paused") : tr("Playing"));
    }
}

void PlaybackPanel::onPlayPauseClicked()
{
    if (m_control == nullptr) {
        return;
    }

    const bool paused = m_control->isPaused();

    // Play at the end means play again from the top: a play button that does
    // nothing because the file is finished is the commonest small annoyance in
    // a player, and the fix costs one seek.
    if (paused && m_control->finished()) {
        m_control->requestSeek(0);
    }

    m_control->setPaused(!paused);
    refresh();
}

void PlaybackPanel::onSkipBackClicked()
{
    if (m_control == nullptr) {
        return;
    }

    const quint64 position = m_control->positionNs();
    m_control->requestSeek(position > kSkipNs ? position - kSkipNs : 0);
}

void PlaybackPanel::onSkipForwardClicked()
{
    if (m_control == nullptr) {
        return;
    }

    const quint64 duration = m_control->durationNs();
    const quint64 wanted = m_control->positionNs() + kSkipNs;

    m_control->requestSeek(duration > 0 ? std::min(wanted, duration) : wanted);
}

void PlaybackPanel::onRestartClicked()
{
    if (m_control == nullptr) {
        return;
    }

    m_control->requestSeek(0);
}

void PlaybackPanel::onSpeedChanged(int index)
{
    if (m_control == nullptr || index < 0 || index >= static_cast<int>(kSpeeds.size())) {
        return;
    }

    m_control->setSpeed(kSpeeds[static_cast<std::size_t>(index)].multiplier);
}

void PlaybackPanel::onSeekRequested(quint64 positionNs)
{
    if (m_control == nullptr) {
        return;
    }

    m_control->requestSeek(positionNs);
}

void PlaybackPanel::onThemeChanged()
{
    const auto icon = [](const char* name) {
        if (const ThemeManager* themes = ThemeManager::instance()) {
            return themes->icon(QString::fromLatin1(name));
        }
        return QIcon{};
    };

    // Icons are recoloured per theme, so they are asked for again rather than
    // kept: the same trap the toolbar fell into in v0.4.
    m_restart->setIcon(icon("replay"));
    m_skipBack->setIcon(icon("skip-back"));
    m_skipForward->setIcon(icon("skip-forward"));
    m_playPause->setIcon(icon(m_shownAsPaused ? "start" : "pause"));

    m_timeline->update();
}

} // namespace torquebus::ui
