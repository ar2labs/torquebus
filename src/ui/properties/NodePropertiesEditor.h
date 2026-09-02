// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The settings of one node on the canvas, editable.
//
// Without this the canvas is decorative: a Lua ECU can be dropped onto it and
// wired up, but there is no way to give it a script, so it fails the next Start
// with "has no script" and there is nowhere to fix that.
//
// The form is built from the node type's ParameterDescriptor list rather than
// from a switch on the type name. That is the whole reason the catalog declares
// its parameters: a new node type becomes editable without this file changing.
// If a descriptor is added to a type, a row appears here.

#pragma once

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"

#include <QString>
#include <QWidget>

class QEvent;
class QFormLayout;
class QLabel;

namespace torquebus::ui {

class NodePropertiesEditor final : public QWidget {
    Q_OBJECT

public:
    /// Neither the description nor the catalog is owned.
    NodePropertiesEditor(GraphDescription& description,
                         const NodeCatalog& catalog,
                         QWidget* parent = nullptr);

    /// Shows one node's settings. An unknown id clears the form.
    void showNode(const QString& descriptionId);

    /// Shows the placeholder.
    void clear();

    [[nodiscard]] QString currentNode() const { return m_nodeId; }

protected:
    /// Commits a script editor when it loses focus.
    ///
    /// Not on every keystroke: each write re-validates the pipeline and logs a
    /// line, so typing a script would produce one report per character. Focus
    /// loss is the moment the user has finished with the field.
    bool eventFilter(QObject* watched, QEvent* event) override;

Q_SIGNALS:
    /// A setting changed and has already been written into the description.
    ///
    /// Carries the node so the window can re-validate and say whether the
    /// pipeline still builds - the same report an edit on the canvas produces.
    void nodeEdited(const QString& descriptionId);

private:
    void rebuild();

    /// Writes one value back, creating the parameter if it was unset.
    void store(const std::string& name, ParameterValue value);

    GraphDescription& m_description;
    const NodeCatalog& m_catalog;

    QString m_nodeId;
    QFormLayout* m_form{nullptr};
    QLabel* m_placeholder{nullptr};
    QWidget* m_formHost{nullptr};
};

} // namespace torquebus::ui
