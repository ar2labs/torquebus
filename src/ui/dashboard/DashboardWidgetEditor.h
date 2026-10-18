// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The settings of one dashboard widget: what it is bound to, and how it reads.
//
// A sibling of NodePropertiesEditor, deliberately: a widget on a dashboard and
// a block on the canvas are the same kind of thing from the user's side -
// something selected, described in a form beside it, and written straight back
// into the description. Two panels that answer the same question differently
// would be two things to learn.
//
// It shows nothing at all outside edit mode. A form whose fields cannot be
// applied is a form that invites being filled in.
//
// --- Configuring from a database -------------------------------------------------------------
//
// Given the loaded databases the form stops being a place to type names exactly and becomes a place
// to choose them:
//
//   * The message and the signal complete as they are typed, from the databases; a signal typed
//     before its message completes as "Message.Signal" and sets both.
//   * **Browse...** opens the signal picker, which searches every signal of every database and
//   shows
//     what each is.
//   * Choosing a signal fills in what the database knows - unit, range, decimals, the names of its
//     values, how long it may be silent - and **Fill from database** does it again later, after the
//     widget has been changed by hand and the hand turned out to be wrong.
//   * Under the fields, one line says what the binding is: the signal and its unit and range, or
//     that the message has no such signal and what it was probably meant to be. A second line says
//     whether the signal is arriving - and when it is not, what has to be wired for it to.
//
// None of it is required. With no database loaded the form is the one that was always here, and
// everything typed is kept as typed.

#pragma once

#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/SignalDictionary.h"
#include "ui/dashboard/DashboardImages.h"

#include <QString>
#include <QWidget>

#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QCompleter;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QStringListModel;
class QTableWidget;
class QTimer;

namespace torquebus {
class SignalSeriesStore;
} // namespace torquebus

namespace torquebus::ui {

class DashboardWidgetEditor final : public QWidget {
    Q_OBJECT

public:
    /// Not owned; must outlive the editor.
    explicit DashboardWidgetEditor(DashboardDescription& dashboard, QWidget* parent = nullptr);
    ~DashboardWidgetEditor() override;

    /// Shows one widget's settings. An unknown id shows the placeholder.
    void showWidget(const QString& widgetId);

    void clear();

    /// Which widget is on the form, or empty.
    [[nodiscard]] QString currentWidget() const { return m_widgetId; }

    /// Re-reads the widget from the description, for a change that came from
    /// somewhere else - a drag on the panel moves the same widget this form is
    /// showing the size of.
    void refreshFromDescription() { showWidget(m_widgetId); }

    /// The loaded databases: what the fields complete from and what the binding is checked against.
    void setDatabases(const std::vector<std::shared_ptr<const CanDatabase>>& databases);

    /// Where the live line reads whether the signal is arriving. Not owned; null says nothing.
    void setPlotStore(const SignalSeriesStore* store);

    /// The project's folder, which the picture of a widget is relative to.
    void setAssetBase(const QString& directory);

    /// Binds the widget on the form to `entry` and fills in what the database knows (applyEntry).
    void bindTo(const DictionaryEntry& entry);

    /// What the line under the binding says: that it is found and what it is, or what is wrong.
    [[nodiscard]] QString bindingText() const;

    /// What the live line says: whether the signal is arriving.
    [[nodiscard]] QString liveText() const;

Q_SIGNALS:
    /// A setting changed and has already been written into the description.
    void widgetEdited(const QString& widgetId);

private Q_SLOTS:
    void onFieldChanged();
    void onBrowse();
    void onFillFromDatabase();
    void onKindChosen();
    void onSignalCompleted(const QString& text);
    void onMessageCompleted(const QString& text);
    void onBrowsePicture();
    void onClearPicture();
    void onAddState();
    void onRemoveState();
    void onStatePicture();
    void onStatesFromDatabase();
    void updateLiveStatus();

private:
    void buildUi();

    /// Repopulates every field from the widget. Nothing it does is written back: filling a combo
    /// box emits currentIndexChanged, and a form that writes back while it is being read would save
    /// the value it is about to load.
    void reload();

    /// Shows only the fields the kind actually uses. A Label with a range and a
    /// unit is four questions nobody has to answer.
    void applyVisibility(const DashboardWidget& widget);

    /// The widget being edited, or nullptr.
    [[nodiscard]] DashboardWidget* current();

    /// What the databases say about the binding, written under the fields.
    void updateBindingInfo();

    /// The names the message and signal fields complete from.
    void updateCompleters();

    /// Fills the table of states from the widget.
    void loadStates(const DashboardWidget& widget);

    /// Reads the table of states back.
    [[nodiscard]] std::vector<DashboardState> statesFromTable() const;

    /// Adds one row to the table of states.
    void addStateRow(const DashboardState& state);

    /// Shows the picture, or why it cannot be.
    void updatePictureInfo(const DashboardWidget& widget);

    /// Opens the file dialog for a picture and returns the file chosen, as the project would store
    /// it.
    [[nodiscard]] QString choosePicture();

    DashboardDescription& m_dashboard;
    QString m_widgetId;

    SignalDictionary m_dictionary;
    const SignalSeriesStore* m_plots{nullptr};

    /// The dictionary's entry for the binding on the form, or nullptr.
    const DictionaryEntry* m_entry{nullptr};

    /// True while the form is being filled in.
    bool m_populating{false};

    DashboardImages m_images;

    QLabel* m_placeholder{nullptr};
    QScrollArea* m_scroll{nullptr};
    QWidget* m_form{nullptr};

    QLabel* m_kindLabel{nullptr};
    QComboBox* m_showAs{nullptr};

    /// A cluster's data profile: where each of its values comes from. In place of the binding.
    QComboBox* m_profile{nullptr};

    QComboBox* m_source{nullptr};
    QWidget* m_messageRow{nullptr};
    QLineEdit* m_message{nullptr};
    QPushButton* m_browse{nullptr};
    QLineEdit* m_signal{nullptr};
    QLineEdit* m_variable{nullptr};

    QLabel* m_bindingInfo{nullptr};
    QLabel* m_liveInfo{nullptr};
    QPushButton* m_fill{nullptr};

    QLineEdit* m_title{nullptr};
    QLineEdit* m_unit{nullptr};

    QDoubleSpinBox* m_minimum{nullptr};
    QDoubleSpinBox* m_maximum{nullptr};
    QDoubleSpinBox* m_threshold{nullptr};
    QSpinBox* m_decimals{nullptr};

    /// Warning below, warning above, critical below, critical above - each a tick and a number.
    QWidget* m_limitsBox{nullptr};
    QCheckBox* m_limitOn[4]{};
    QDoubleSpinBox* m_limitValue[4]{};

    QWidget* m_windowRow{nullptr};
    QDoubleSpinBox* m_window{nullptr};

    QWidget* m_staleRow{nullptr};
    QCheckBox* m_staleOn{nullptr};
    QSpinBox* m_stale{nullptr};

    QWidget* m_pictureRow{nullptr};
    QLineEdit* m_picture{nullptr};
    QLabel* m_pictureInfo{nullptr};

    QWidget* m_statesBox{nullptr};
    QTableWidget* m_states{nullptr};

    QLabel* m_hint{nullptr};

    /// Rows, so a whole row can be hidden with its label rather than leaving a
    /// labelled gap where a field used to be.
    QFormLayout* m_layout{nullptr};

    QCompleter* m_messageCompleter{nullptr};
    QCompleter* m_signalCompleter{nullptr};
    QStringListModel* m_messageNames{nullptr};
    QStringListModel* m_signalNames{nullptr};

    /// What the signal completer is showing the signals of: a message, or every message when empty.
    QString m_completingFor{QStringLiteral("\x01")};

    QTimer* m_liveTimer{nullptr};
};

} // namespace torquebus::ui
