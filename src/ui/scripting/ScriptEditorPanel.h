// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The Script Editor: edit a simulated ECU and reload it without stopping.
//
// Before this panel, changing one number in a script meant Stop, edit the
// parameter in a form, Start - and Start rebuilds the graph, reopens the
// channels and throws the trace away. The state somebody was debugging, the
// fault they had just provoked, the forty seconds of traffic leading up to it:
// all gone, to change a cycle time from 10 to 20.
//
// So: **Reload replaces the script in the running node and nothing else.** The
// measurement keeps running, the trace keeps filling, the clock keeps counting,
// and the other ECUs on the canvas never notice. The whole mechanism is in
// ScriptLibrary.h and LuaEcuNode::reload; this is the window onto it.
//
// The rule that makes it usable rather than merely fast is stated there and
// shown here: *a script that fails to load leaves the running one alone.* A
// refused reload paints the offending line and says why, and the ECU on the bus
// carries on exactly as it was.
//
// Where the text lives, and why the panel cares:
//
//   * A script kept **inline** in the project belongs to the project, so Reload
//     writes it into the node's `script` parameter as well as offering it to
//     the running node. Otherwise the reload would work and Save would write
//     the old text back.
//
//   * A script kept in a **file** belongs to the file, so Reload writes the
//     file. That also means somebody can keep editing it in their own editor
//     and press Reload here - which is the arrangement most people who write a
//     lot of Lua will actually want.

#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

namespace torquebus {
class GraphDescription;
class ScriptLibrary;
} // namespace torquebus

namespace torquebus::ui {

class LuaHighlighter;
class ScriptEdit;

class ScriptEditorPanel final : public QWidget {
    Q_OBJECT

public:
    /// The description is not owned and must outlive the panel.
    explicit ScriptEditorPanel(GraphDescription& description, QWidget* parent = nullptr);

    /// Where edited scripts are handed to running nodes. Not owned; null
    /// detaches, and the panel then only edits the project.
    void setLibrary(ScriptLibrary* library);

    /// Whether a measurement is running, which is what decides between
    /// "reload it now" and "it will be used at the next Start".
    void setRunning(bool running);

    /// Shows one node's script. A node that is not a `lua.ecu` - or an id that
    /// is not in the description - shows the placeholder instead.
    void showNode(const QString& descriptionId);

    /// The directory a relative `scriptPath` resolves against: the project's
    /// own folder, as everywhere else.
    void setBasePath(const QString& path) { m_basePath = path; }

    void clear();

    [[nodiscard]] QString currentNode() const { return m_nodeId; }

    /// True while the editor holds text that has not been written back.
    [[nodiscard]] bool isModified() const;

Q_SIGNALS:
    /// The script was written into the description; the window re-validates and
    /// marks the project dirty, exactly as for an edit in the Block panel.
    void nodeEdited(const QString& descriptionId);

    /// A line for the Output panel - what a reload did, or why it was refused.
    void reported(const QString& text, bool isError);

private Q_SLOTS:
    /// Collects reload outcomes. The panel's whole clock.
    void refresh();

    void onReload();
    void onRevert();
    void onTextChanged();
    void onThemeChanged();

private:
    void buildUi();
    void updateAvailability();

    /// Writes the editor's text where this script lives - the file, or the
    /// node's parameter. Returns false and says why on a file it cannot write.
    bool commit(QString& error);

    /// Reads the script this node runs, from wherever it is kept.
    [[nodiscard]] QString readSource(bool& fromFile, QString& path) const;

    /// The line number in a Lua error - `ecu.lua:42: ...` - or 0.
    [[nodiscard]] static int errorLineOf(const QString& message);

    void setStatus(const QString& text, bool isError);

    GraphDescription& m_description;
    ScriptLibrary* m_library{nullptr};

    QString m_nodeId;
    QString m_basePath;

    /// True when this node's script is a file rather than inline text.
    bool m_isFile{false};
    QString m_filePath;

    bool m_running{false};

    /// What was last committed, so Revert has something to go back to and the
    /// modified marker means something.
    QString m_committed;

    ScriptEdit* m_editor{nullptr};
    LuaHighlighter* m_highlighter{nullptr};

    QLabel* m_header{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_placeholder{nullptr};
    QWidget* m_body{nullptr};

    /// Whether the status line is currently saying something went wrong, so a
    /// theme change repaints it in the colour it already had.
    bool m_statusIsError{false};

    QPushButton* m_reload{nullptr};
    QPushButton* m_revert{nullptr};
};

} // namespace torquebus::ui
