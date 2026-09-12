// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/plugins/PluginsDialog.h"

#include "plugins/host/PluginLoader.h"
#include "ui/theme/ThemeManager.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace torquebus::ui {
namespace {

enum Column : int {
    ColumnName = 0,
    ColumnVersion,
    ColumnFile,
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

[[nodiscard]] QString fileName(const std::string& path)
{
    return QFileInfo{QString::fromStdString(path)}.fileName();
}

} // namespace

PluginsDialog::PluginsDialog(const plugins::PluginLoader& loader, QWidget* parent)
    : QDialog{parent}
{
    setWindowTitle(tr("Plugins"));
    resize(880, 460);

    const Theme theme = currentTheme();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* summary = new QLabel;
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);

    if (loader.loaded().empty() && loader.rejected().empty()) {
        // Said in words, because an empty table is also what a bug would look
        // like. A build with no plugins is the ordinary case, not a fault.
        summary->setText(tr("No plugins found beside the application. Everything "
                            "TorqueBus can do right now is built in."));
    } else {
        summary->setText(tr("%n plugin(s) loaded", nullptr,
                            static_cast<int>(loader.loaded().size()))
                         + (loader.rejected().empty()
                                ? QString{}
                                : tr(", %n refused", nullptr,
                                     static_cast<int>(loader.rejected().size()))));
    }

    layout->addWidget(summary);

    auto* tree = new QTreeWidget;
    tree->setColumnCount(ColumnCount);
    tree->setHeaderLabels({tr("Plugin"), tr("Version"), tr("File"), tr("Detail")});
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);

    tree->header()->setSectionResizeMode(ColumnName, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(ColumnVersion, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(ColumnFile, QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(ColumnDetail, QHeaderView::Stretch);
    tree->setColumnWidth(ColumnName, 200);
    tree->setColumnWidth(ColumnFile, 220);

    for (const plugins::LoadedPlugin& plugin : loader.loaded()) {
        auto* row = new QTreeWidgetItem(tree);
        row->setText(ColumnName, QString::fromStdString(
                                     plugin.displayName.empty() ? plugin.name
                                                                : plugin.displayName));
        row->setText(ColumnVersion, QString::fromStdString(plugin.version));
        row->setText(ColumnFile, fileName(plugin.path));
        row->setText(ColumnDetail, tr("loaded"));
        row->setForeground(ColumnDetail, theme.success);
        row->setToolTip(ColumnFile, QString::fromStdString(plugin.path));
    }

    for (const plugins::RejectedPlugin& plugin : loader.rejected()) {
        auto* row = new QTreeWidgetItem(tree);

        // No name: a refused plugin is one this build could not ask what it is
        // called, and inventing one from the file name would be guessing in the
        // one place somebody is looking for facts.
        row->setText(ColumnName, tr("—"));
        row->setText(ColumnFile, fileName(plugin.path));
        row->setText(ColumnDetail, QString::fromStdString(plugin.reason));
        row->setForeground(ColumnDetail, theme.error);
        row->setToolTip(ColumnFile, QString::fromStdString(plugin.path));
        row->setToolTip(ColumnDetail, QString::fromStdString(plugin.reason));
    }

    layout->addWidget(tree, 1);

    // The host key, because the commonest refusal is a key that does not match
    // and the message shows both - so this is the half somebody needs to give
    // to whoever built the plugin.
    auto* key = new QLabel(tr("This build accepts plugins marked  %1")
                               .arg(QString::fromUtf8(
                                   plugins::PluginLoader::hostBuildKey().data(),
                                   static_cast<qsizetype>(
                                       plugins::PluginLoader::hostBuildKey().size()))));
    key->setTextInteractionFlags(Qt::TextSelectableByMouse);
    key->setStyleSheet(QStringLiteral("color: %1;").arg(theme.textMuted.name()));
    layout->addWidget(key);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

} // namespace torquebus::ui
