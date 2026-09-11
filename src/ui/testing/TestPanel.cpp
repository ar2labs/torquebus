// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/testing/TestPanel.h"

#include "core/testing/TestReport.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <vector>

namespace torquebus::ui {
namespace {

/// 20 Hz, like every other panel that reads shared state on a timer.
constexpr int kRefreshMs = 50;

enum Column : int {
    ColumnCase = 0,
    ColumnResult,
    ColumnTime,
    ColumnDetail,
    ColumnCount,
};

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

[[nodiscard]] QColor colourFor(TestOutcome outcome, const Theme& theme)
{
    switch (outcome) {
    case TestOutcome::Passed:
        return theme.success;
    case TestOutcome::Failed:
        return theme.error;
    case TestOutcome::Errored:
        return theme.warning;
    case TestOutcome::Running:
        break;
    }

    return theme.textMuted;
}

} // namespace

TestPanel::TestPanel(QWidget* parent)
    : QWidget{parent}
{
    buildUi();

    auto* timer = new QTimer(this);
    timer->setInterval(kRefreshMs);
    connect(timer, &QTimer::timeout, this, &TestPanel::refresh);
    timer->start();

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { onThemeChanged(); });
    }

    updateSummary();
}

void TestPanel::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(6);

    // --- The answer, at the top, where it is looked for -------------------
    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(6);

    m_summary = new QLabel;
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    header->addWidget(m_summary, 1);

    m_export = new QPushButton(tr("Export..."));
    m_export->setToolTip(tr("Writes the whole run as Markdown - a verdict that cannot "
                            "leave the window is a verdict nobody else can act on."));
    connect(m_export, &QPushButton::clicked, this, &TestPanel::onExport);
    header->addWidget(m_export);

    layout->addLayout(header);

    // --- The cases, each opening into the checks it made -------------------
    m_cases = new QTreeWidget;
    m_cases->setColumnCount(ColumnCount);
    m_cases->setHeaderLabels({tr("Case"), tr("Result"), tr("Time"), tr("Detail")});
    m_cases->setRootIsDecorated(true);
    m_cases->setUniformRowHeights(true);
    m_cases->setAlternatingRowColors(true);
    m_cases->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_cases->setEditTriggers(QAbstractItemView::NoEditTriggers);

    m_cases->header()->setSectionResizeMode(ColumnCase, QHeaderView::Interactive);
    m_cases->header()->setSectionResizeMode(ColumnResult, QHeaderView::ResizeToContents);
    m_cases->header()->setSectionResizeMode(ColumnTime, QHeaderView::ResizeToContents);
    m_cases->header()->setSectionResizeMode(ColumnDetail, QHeaderView::Stretch);
    m_cases->setColumnWidth(ColumnCase, 320);

    layout->addWidget(m_cases, 1);
}

void TestPanel::setReport(TestReport* report)
{
    m_report = report;

    m_cases->clear();
    m_sawRun = false;
    m_lastSummary.clear();

    updateSummary();
}

void TestPanel::refresh()
{
    if (m_report == nullptr) {
        return;
    }

    const TestSummary summary = m_report->summary();

    // A run that started while the panel was showing the last one's results.
    // Detected here rather than signalled, because the node's prepare() is on
    // the executor thread and this is the panel's own clock.
    if (summary.started && !m_sawRun) {
        m_cases->clear();
        m_sawRun = true;
    }

    if (!summary.started && m_sawRun) {
        m_sawRun = false;
    }

    for (const TestCaseResult& result : m_report->takeNew()) {
        appendCase(result);
    }

    updateSummary();
}

void TestPanel::appendCase(const TestCaseResult& result)
{
    const Theme theme = currentTheme();

    const QString outcomeText = result.outcome == TestOutcome::Passed  ? tr("Passed")
        : result.outcome == TestOutcome::Failed                       ? tr("Failed")
        : result.outcome == TestOutcome::Errored                      ? tr("Error")
                                                                      : tr("Running");

    auto* item = new QTreeWidgetItem(m_cases);
    item->setText(ColumnCase, QString::fromStdString(result.name));
    item->setText(ColumnResult, outcomeText);
    item->setText(ColumnTime, elapsedText(result.finishedNs));
    item->setText(ColumnDetail, QString::fromStdString(result.message));

    const QBrush tint{colourFor(result.outcome, theme)};
    item->setForeground(ColumnResult, tint);
    item->setForeground(ColumnDetail, tint);

    // How long the case itself took, which is not the same as when it ended and
    // is the number that says "this one waited for its timeout".
    const std::uint64_t duration = result.finishedNs > result.startedNs
        ? result.finishedNs - result.startedNs
        : 0;

    item->setToolTip(ColumnTime,
                     tr("Started at %1, took %2")
                         .arg(elapsedText(result.startedNs), elapsedText(duration)));

    for (const TestCheck& check : result.checks) {
        auto* child = new QTreeWidgetItem(item);
        child->setText(ColumnCase, QString::fromStdString(check.description));
        child->setText(ColumnResult, check.passed ? tr("ok") : tr("failed"));
        child->setText(ColumnDetail, QString::fromStdString(check.detail));

        const QBrush checkTint{check.passed ? theme.textMuted : theme.error};
        child->setForeground(ColumnResult, checkTint);
        child->setForeground(ColumnDetail, checkTint);
    }

    // Failures open themselves; passes stay folded. The checks of a passing
    // case are evidence somebody may want later, and noise in the meantime -
    // the failing one is what the panel is being looked at for.
    item->setExpanded(result.outcome != TestOutcome::Passed);

    // Scrolled to only when the view was already at the bottom, so reading a
    // failure twenty rows up is not interrupted by the next case finishing.
    if (QScrollBar* bar = m_cases->verticalScrollBar();
        bar == nullptr || bar->value() >= bar->maximum() - 4) {
        m_cases->scrollToItem(item);
    }
}

void TestPanel::updateSummary()
{
    const Theme theme = currentTheme();

    QString text;
    QColor colour = theme.textMuted;

    if (m_report == nullptr) {
        text = tr("No measurement.");
    } else {
        const TestSummary summary = m_report->summary();

        if (!summary.started) {
            // Said plainly, because the alternative reading of an empty table
            // is "everything passed".
            text = tr("No test sequence in this graph. Drop a Test Sequence block "
                      "to check the bus rather than only watch it.");
        } else {
            const std::size_t finished = summary.passed + summary.failed + summary.errored;

            text = tr("%1 of %2 case(s) finished  ·  %3 passed  ·  %4 failed  ·  %5 in error")
                       .arg(finished)
                       .arg(summary.total)
                       .arg(summary.passed)
                       .arg(summary.failed)
                       .arg(summary.errored);

            if (summary.complete) {
                text = (summary.failed == 0 && summary.errored == 0)
                    ? tr("Passed  ·  %1 of %2 case(s)").arg(summary.passed).arg(summary.total)
                    : tr("FAILED  ·  %1 passed, %2 failed, %3 in error, of %4")
                          .arg(summary.passed)
                          .arg(summary.failed)
                          .arg(summary.errored)
                          .arg(summary.total);
            }

            // Red as soon as the first failure lands, not only at the end. A
            // run that is already lost should say so while there is still time
            // to stop it.
            colour = (summary.failed > 0 || summary.errored > 0) ? theme.error
                : summary.complete                              ? theme.success
                                                                : theme.text;
        }
    }

    if (text == m_lastSummary) {
        return;
    }

    m_lastSummary = text;

    m_summary->setText(text);
    m_summary->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;")
                                 .arg(colour.name()));

    m_export->setEnabled(m_report != nullptr && m_cases->topLevelItemCount() > 0);
}

QString TestPanel::elapsedText(std::uint64_t nanoseconds)
{
    // Milliseconds, three decimals, like the trace's own time column - the two
    // are read side by side and a different unit would have to be converted in
    // somebody's head at the exact moment they are trying to think about a bug.
    const double seconds = static_cast<double>(nanoseconds) / 1'000'000'000.0;
    return QStringLiteral("%1").arg(seconds, 0, 'f', 3);
}

QString TestPanel::buildMarkdown() const
{
    QString text;
    QTextStream out{&text};

    out << "# Test run\n\n";

    if (m_report != nullptr) {
        const TestSummary summary = m_report->summary();

        out << (summary.complete ? "**Complete.**" : "**Stopped before the end.**") << "  \n";
        out << summary.passed << " passed, " << summary.failed << " failed, "
            << summary.errored << " in error, of " << summary.total << " declared.\n\n";
    }

    out << "| Result | Case | Time (s) | Detail |\n";
    out << "|---|---|---|---|\n";

    for (int index = 0; index < m_cases->topLevelItemCount(); ++index) {
        const QTreeWidgetItem* item = m_cases->topLevelItem(index);

        out << "| " << item->text(ColumnResult) << " | " << item->text(ColumnCase) << " | "
            << item->text(ColumnTime) << " | " << item->text(ColumnDetail) << " |\n";
    }

    // The checks of every case that did not pass, spelled out underneath.
    // A table row saying "Failed" is where somebody starts; the check that
    // failed and what it saw is where they finish.
    for (int index = 0; index < m_cases->topLevelItemCount(); ++index) {
        const QTreeWidgetItem* item = m_cases->topLevelItem(index);

        if (item->text(ColumnResult) == tr("Passed") || item->childCount() == 0) {
            continue;
        }

        out << "\n## " << item->text(ColumnCase) << "\n\n";

        for (int child = 0; child < item->childCount(); ++child) {
            const QTreeWidgetItem* check = item->child(child);

            out << "- " << check->text(ColumnResult) << ": " << check->text(ColumnCase);

            if (!check->text(ColumnDetail).isEmpty()) {
                out << " — " << check->text(ColumnDetail);
            }

            out << "\n";
        }
    }

    return text;
}

void TestPanel::onExport()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Test Report"), QStringLiteral("test-report.md"),
        tr("Markdown (*.md);;All files (*)"));

    if (path.isEmpty()) {
        return;
    }

    QFile file{path};

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        Q_EMIT reported(tr("The report could not be written: %1").arg(file.errorString()), true);
        return;
    }

    QTextStream out{&file};
    out << buildMarkdown();

    Q_EMIT reported(tr("Test report written to %1.").arg(path), false);
}

void TestPanel::onThemeChanged()
{
    // The summary is repainted by clearing the cached text: it holds a colour
    // in a style sheet, and the colour is what just changed.
    m_lastSummary.clear();
    updateSummary();

    // The rows hold their tint in the item, so they are re-tinted here rather
    // than repainted - and a report already on screen must not turn grey
    // because somebody switched to the light theme mid-run.
    const Theme theme = currentTheme();

    for (int index = 0; index < m_cases->topLevelItemCount(); ++index) {
        QTreeWidgetItem* item = m_cases->topLevelItem(index);

        const TestOutcome outcome = item->text(ColumnResult) == tr("Passed")
            ? TestOutcome::Passed
            : item->text(ColumnResult) == tr("Failed") ? TestOutcome::Failed
                                                       : TestOutcome::Errored;

        const QBrush tint{colourFor(outcome, theme)};
        item->setForeground(ColumnResult, tint);
        item->setForeground(ColumnDetail, tint);

        for (int child = 0; child < item->childCount(); ++child) {
            QTreeWidgetItem* check = item->child(child);

            const QBrush checkTint{check->text(ColumnResult) == tr("ok") ? theme.textMuted
                                                                        : theme.error};
            check->setForeground(ColumnResult, checkTint);
            check->setForeground(ColumnDetail, checkTint);
        }
    }
}

} // namespace torquebus::ui
