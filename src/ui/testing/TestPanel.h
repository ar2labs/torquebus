// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The verdict of a test sequence, while it is still being reached.
//
// A run is watched as often as it is read afterwards, so results appear one at
// a time as each case finishes rather than all at once at the end. Somebody
// watching an eight-minute sequence should be able to see it failing at case
// three and stop, instead of waiting for a report that was already decided.
//
// Three things on screen, in the order they are looked at:
//
//   * **the summary**, which is the whole answer: how many passed, how many
//     failed, whether the run finished. Coloured, because "0 failed" and
//     "3 failed" get read at a glance and never carefully.
//   * **the cases**, each expandable into the checks it made. The checks are
//     there for the case somebody disputes: a report that lists only failures
//     cannot be read as evidence that anything was checked.
//   * **an export**, because a verdict that cannot leave the window is a
//     verdict nobody else can act on.
//
// The panel never decides anything. It reads a TestReport the executor fills -
// see that header for why appending to it is the one place in this project
// where the executor takes a lock and waits.

#pragma once

#include <QString>
#include <QWidget>

#include <cstdint>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace torquebus {
class TestReport;
struct TestCaseResult;
}

namespace torquebus::ui {

class TestPanel final : public QWidget {
    Q_OBJECT

public:
    explicit TestPanel(QWidget* parent = nullptr);

    /// The report this panel shows. Not owned; must outlive the panel.
    void setReport(TestReport* report);

Q_SIGNALS:
    /// A line for the Output panel - where a report was written, or why it
    /// could not be.
    void reported(const QString& text, bool isError);

private Q_SLOTS:
    /// Drains the report and repaints the summary. The panel's whole clock.
    void refresh();

    void onExport();
    void onThemeChanged();

private:
    void buildUi();

    /// Adds one finished case, with its checks underneath it.
    void appendCase(const TestCaseResult& result);

    void updateSummary();

    /// The whole run as Markdown: a heading, the summary, and a table of cases
    /// with the failing checks spelled out.
    ///
    /// Markdown rather than CSV or a screenshot: it is read as text by a person
    /// in a ticket and by a diff in a repository, and neither of those can do
    /// anything with a picture of a table.
    [[nodiscard]] QString buildMarkdown() const;

    /// Seconds since the measurement started, as text - the same clock the
    /// trace shows, so a failure and the frames that caused it line up.
    [[nodiscard]] static QString elapsedText(std::uint64_t nanoseconds);

    TestReport* m_report{nullptr};

    QLabel* m_summary{nullptr};
    QTreeWidget* m_cases{nullptr};
    QPushButton* m_export{nullptr};

    /// What the summary said last time, so the label is only rebuilt when it
    /// actually changed rather than twenty times a second.
    QString m_lastSummary;

    /// True once a run has been seen to start, so the panel can tell "no
    /// sequence in this graph" from "a sequence that has not reported yet".
    bool m_sawRun{false};
};

} // namespace torquebus::ui
