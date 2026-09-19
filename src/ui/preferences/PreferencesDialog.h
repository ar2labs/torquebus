// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Preferences: a list of pages on the left, the page on the right.
//
// Two decisions shape the whole file.
//
// **Everything applies as you change it.** Not on OK. The application already
// switches theme instantly from the View menu, and a dialog that made the same
// change wait for a button would be the one place in the window that behaves
// differently. It also removes the guessing: an accent is a colour, and the
// only way to know whether you want it is to see the window in it. Cancel
// restores what was in force when the dialog opened, so live is not a trap.
//
// **Only settings the application actually honours get a control.** The
// Shortcuts and Plugins pages exist and say what they will hold and when; they
// have no controls at all. A checkbox that does nothing is worse than a page
// that admits it is empty, because the first one is a bug the user has to
// discover and the second is a plan they can read.

#pragma once

#include "core/can/CanTypes.h"
#include "ui/theme/AccentColor.h"
#include "ui/theme/Theme.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

namespace torquebus::services {
class SettingsStore;
}

namespace torquebus::ui {

class AccentSwatchRow;
class ThemeCard;
class ThemeManager;

class PreferencesDialog final : public QDialog {
    Q_OBJECT

public:
    /// `devices` is the interface list the window has already enumerated.
    ///
    /// Handed in rather than looked up, so opening Preferences does not rescan
    /// the hardware behind a running measurement. The dialog shows what the
    /// Hardware menu last found, which is also what the user is looking at in
    /// the Project Explorer.
    PreferencesDialog(ThemeManager& themes,
                      services::SettingsStore& settings,
                      CanDeviceInfoList devices,
                      QWidget* parent = nullptr);

    /// How often the trace view picks up new rows, in milliseconds.
    [[nodiscard]] int traceRefreshMs() const;

    /// True when identifiers should be shown in decimal rather than hex.
    [[nodiscard]] bool decimalIdentifiers() const;

Q_SIGNALS:
    /// Emitted whenever a Trace or Transmit setting changes, including while
    /// the dialog is open. The window applies it to the panels; this dialog
    /// does not know they exist.
    void tracePreferencesChanged();

    /// A channel's bitrate changed. The window rebinds - which it can only do
    /// while stopped, and which is why the page says so.
    void hardwarePreferencesChanged();

public:
    // --- Defaults, shared with the window so Reset and first-run agree ------

    static constexpr int kDefaultTraceRefreshMs = 40; // 25 Hz
    static constexpr bool kDefaultDecimalIdentifiers = false;
    static constexpr bool kDefaultRestoreLayout = true;

    /// On by default. An engineering tool that opens where you left it is the
    /// convention, and the alternative - an empty canvas every morning - makes
    /// somebody find the same file in the same folder every day.
    static constexpr bool kDefaultRestoreLastProject = true;

public Q_SLOTS:
    /// Puts everything back the way it was when the dialog opened.
    ///
    /// Public because QDialog::reject() is: Esc, the close button and the
    /// Cancel button all reach it through a base-class pointer, and narrowing
    /// an override is a trap for whoever calls it next.
    void reject() override;

private Q_SLOTS:
    void onThemeCardClicked(torquebus::ui::ThemeVariant variant);
    void onFollowSystemToggled(bool follow);
    void onAccentChanged(torquebus::ui::AccentColor accent);
    void onDensityChanged(int index);
    void onBitrateChanged(int row);
    void onResetPreferences();
    void onOpenSettingsFolder();

private:
    void buildUi();
    [[nodiscard]] QWidget* buildGeneralPage();
    [[nodiscard]] QWidget* buildAppearancePage();
    [[nodiscard]] QWidget* buildTracePage();
    [[nodiscard]] QWidget* buildHardwarePage();

    /// A page with nothing on it but an honest sentence about when it will
    /// have something.
    [[nodiscard]] QWidget*
    buildPlannedPage(const QString& title, const QString& description, const QString& milestone);

    /// Pushes every appearance control's current value at the ThemeManager.
    void applyAppearance();

    /// Reads the settings store into the controls, without emitting.
    void loadFromSettings();

    /// Writes the controls into the settings store and flushes it.
    void storeToSettings();

    /// Puts the appearance back the way it was when the dialog opened.
    void revertAppearance();

    ThemeManager& m_themes;
    services::SettingsStore& m_settings;

    /// The appearance in force when the dialog opened. Cancel's whole job.
    ThemeVariant m_openingVariant{ThemeVariant::Dark};
    AccentColor m_openingAccent{AccentColor::TorqueBus};
    Density m_openingDensity{Density::Comfortable};
    bool m_openingFollowSystem{false};

    /// The Trace settings as they were when the dialog opened. Cancel has to
    /// undo these too - they apply live like everything else here, and a
    /// cancelled dialog that left the refresh rate changed would be exactly the
    /// trap that applying live is supposed to avoid.
    int m_openingTraceRefreshMs{kDefaultTraceRefreshMs};
    bool m_openingDecimalIdentifiers{kDefaultDecimalIdentifiers};
    bool m_openingRestoreLayout{kDefaultRestoreLayout};
    bool m_openingRestoreLastProject{kDefaultRestoreLastProject};

    /// True while the dialog is writing into its own controls, so a
    /// programmatic change is not read back as the user having made one.
    bool m_loading{false};

    QListWidget* m_pageList{nullptr};
    QStackedWidget* m_pages{nullptr};

    ThemeCard* m_darkCard{nullptr};
    ThemeCard* m_lightCard{nullptr};
    QCheckBox* m_followSystem{nullptr};
    AccentSwatchRow* m_accentRow{nullptr};
    QComboBox* m_density{nullptr};

    QCheckBox* m_restoreLayout{nullptr};
    QCheckBox* m_restoreLastProject{nullptr};
    QLabel* m_settingsPath{nullptr};

    QSpinBox* m_traceRefresh{nullptr};
    QCheckBox* m_decimalIdentifiers{nullptr};

    /// The interfaces the window found, in the order the engine binds them -
    /// so row N of the table is CAN N+1, and the page can say so.
    CanDeviceInfoList m_devices;
    QTableWidget* m_hardware{nullptr};
};

} // namespace torquebus::ui
