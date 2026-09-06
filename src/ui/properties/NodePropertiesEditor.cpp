// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/properties/NodePropertiesEditor.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QFrame>
#include <QLayoutItem>
#include <QVBoxLayout>

#include <limits>
#include <set>
#include <string>

namespace torquebus::ui {
namespace {

/// Parameters whose value is a whole Lua script rather than a line of text.
///
/// Named here rather than guessed from the length of the string: "script" is a
/// contract with NodeCatalog, and a text box the size of a paragraph is the
/// difference between a usable ECU editor and a QLineEdit holding 60 lines of
/// Lua on one line.
[[nodiscard]] bool isSourceCode(std::string_view name)
{
    return name == "script";
}

/// Property holding the parameter name a script editor writes back into.
constexpr auto kParameterProperty = "torquebusParameter";

/// Parameters that name a file, and so deserve a Browse button.
///
/// By name, like isSourceCode above, because the type says only "Text" and a
/// path is not a kind of text a descriptor can announce. Adding a filter here
/// is what a new file-shaped parameter costs.
[[nodiscard]] bool isFilePath(std::string_view name)
{
    return name == "scriptPath" || name == "database" || name == "path";
}

/// The file dialog's filter and title for a path parameter.
struct FileChoice final {
    QString title;
    QString filter;
};

[[nodiscard]] FileChoice fileChoiceFor(std::string_view name)
{
    if (name == "database") {
        return {NodePropertiesEditor::tr("Choose a database"),
                NodePropertiesEditor::tr("CAN databases (*.dbc);;All files (*)")};
    }

    if (name == "path") {
        return {NodePropertiesEditor::tr("Choose a recording"),
                NodePropertiesEditor::tr("TorqueBus logs (*.tblog);;All files (*)")};
    }

    return {NodePropertiesEditor::tr("Choose a script"),
            NodePropertiesEditor::tr("Lua scripts (*.lua);;All files (*)")};
}

[[nodiscard]] QString toQt(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

/// An undeclared parameter's value, as text for a single-line field.
[[nodiscard]] QString scriptValueText(const ParameterValue& value)
{
    switch (value.type()) {
    case ParameterValue::Type::Boolean:
        return value.asBoolean() ? QStringLiteral("true") : QStringLiteral("false");
    case ParameterValue::Type::Integer:
        return QString::number(value.asInteger());
    case ParameterValue::Type::Real:
        return QString::number(value.asReal());
    case ParameterValue::Type::Text:
        return QString::fromStdString(value.asText());
    }

    return {};
}

/// The type of an undeclared parameter, inferred from what was typed.
///
/// Inference rather than a type picker beside every field: these are script
/// settings, the script reads them as Lua values, and "0x101" or "12.5" or
/// "true" already says which is meant. A picker would put a control next to
/// every row to state something the value states already.
///
/// Hex is accepted because CAN identifiers are written that way everywhere else
/// in this application, and a field that rejects 0x18FEE500 would be the only
/// one that does.
[[nodiscard]] ParameterValue parseScriptValue(const QString& text)
{
    const QString trimmed = text.trimmed();

    if (trimmed.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0) {
        return ParameterValue::fromBoolean(true);
    }
    if (trimmed.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0) {
        return ParameterValue::fromBoolean(false);
    }

    bool ok = false;

    if (trimmed.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        const qlonglong hex = trimmed.mid(2).toLongLong(&ok, 16);
        if (ok) {
            return ParameterValue::fromInteger(hex);
        }
    }

    const qlonglong integer = trimmed.toLongLong(&ok);
    if (ok) {
        return ParameterValue::fromInteger(integer);
    }

    const double real = trimmed.toDouble(&ok);
    if (ok) {
        return ParameterValue::fromReal(real);
    }

    return ParameterValue::fromText(trimmed.toStdString());
}

} // namespace

NodePropertiesEditor::NodePropertiesEditor(GraphDescription& description,
                                           const NodeCatalog& catalog,
                                           QWidget* parent)
    : QWidget{parent}
    , m_description{description}
    , m_catalog{catalog}
{
    m_placeholder = new QLabel(tr("Select a block on the Pipeline canvas."), this);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_placeholder->setProperty("torquebusRole", QStringLiteral("placeholder"));

    m_formHost = new QWidget;
    m_form = new QFormLayout(m_formHost);
    m_form->setContentsMargins(8, 8, 8, 8);
    m_form->setHorizontalSpacing(10);
    m_form->setVerticalSpacing(6);
    m_form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    // Scrolled: a Lua script field is tall, and a node with eight parameters
    // must not push the last of them off the bottom of a narrow dock.
    auto* scroll = new QScrollArea(this);
    scroll->setWidget(m_formHost);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_placeholder);
    layout->addWidget(scroll);

    clear();
}

void NodePropertiesEditor::clear()
{
    m_nodeId.clear();
    rebuild();
}

void NodePropertiesEditor::showNode(const QString& descriptionId)
{
    m_nodeId = descriptionId;
    rebuild();
}

void NodePropertiesEditor::store(const std::string& name, ParameterValue value)
{
    NodeDescription* node = const_cast<NodeDescription*>(
        m_description.find(m_nodeId.toStdString()));

    if (node == nullptr) {
        return;
    }

    node->parameters.set(name, std::move(value));
    Q_EMIT nodeEdited(m_nodeId);
}

void NodePropertiesEditor::rebuild()
{
    // Rebuilt rather than repopulated. The widget for a parameter depends on
    // its declared type, so a different node needs different widgets, and
    // reusing them would mean tracking which is which.
    while (QLayoutItem* item = m_form->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const NodeDescription* node = m_nodeId.isEmpty()
        ? nullptr
        : m_description.find(m_nodeId.toStdString());

    const NodeTypeInfo* info = node == nullptr ? nullptr : m_catalog.find(node->typeName);

    m_placeholder->setVisible(node == nullptr);
    m_formHost->setVisible(node != nullptr);

    if (node == nullptr || info == nullptr) {
        return;
    }

    // Identity first, read-only: the id is set by renaming the block on the
    // canvas, where it is also what the wires refer to.
    m_form->addRow(tr("Block"), new QLabel(m_nodeId));
    m_form->addRow(tr("Type"), new QLabel(QString::fromStdString(info->displayName)));

    auto* enabled = new QCheckBox(tr("Enabled"));
    enabled->setChecked(node->enabled);
    enabled->setToolTip(tr("A disabled block stays in the project with its settings "
                           "and position, and is skipped when the pipeline is built."));
    connect(enabled, &QCheckBox::toggled, this, [this](bool on) {
        if (NodeDescription* target =
                const_cast<NodeDescription*>(m_description.find(m_nodeId.toStdString()))) {
            target->enabled = on;
            Q_EMIT nodeEdited(m_nodeId);
        }
    });
    m_form->addRow(QString{}, enabled);

    for (const ParameterDescriptor& parameter : info->parameters) {
        addDeclaredRow(*node, parameter);
    }

    addScriptParameterRows(*node, *info);
}

void NodePropertiesEditor::addDeclaredRow(const NodeDescription& node,
                                          const ParameterDescriptor& parameter)
{
    const std::string name{parameter.name};
    const QString label = parameter.required
        ? tr("%1 *").arg(toQt(parameter.displayName))
        : toQt(parameter.displayName);

    const NodeParameters& values = node.parameters;

    switch (parameter.type) {
    case ParameterValue::Type::Boolean: {
        auto* box = new QCheckBox;
        box->setChecked(values.boolean(name));
        box->setToolTip(toQt(parameter.description));
        connect(box, &QCheckBox::toggled, this, [this, name](bool on) {
            store(name, ParameterValue::fromBoolean(on));
        });
        m_form->addRow(label, box);
        break;
    }

    case ParameterValue::Type::Integer: {
        auto* spin = new QSpinBox;
        // A CAN identifier does not fit in a default 0-99 range, and a
        // spin box that silently clamps 0x18FEE500 to 99 is worse than no
        // editor at all.
        spin->setRange(std::numeric_limits<int>::min(),
                       std::numeric_limits<int>::max());
        spin->setValue(static_cast<int>(values.integer(name)));
        spin->setToolTip(toQt(parameter.description));
        connect(spin, &QSpinBox::valueChanged, this, [this, name](int value) {
            store(name, ParameterValue::fromInteger(value));
        });
        m_form->addRow(label, spin);
        break;
    }

    case ParameterValue::Type::Real: {
        auto* spin = new QDoubleSpinBox;
        spin->setRange(-1e9, 1e9);
        spin->setDecimals(4);
        spin->setValue(values.real(name));
        spin->setToolTip(toQt(parameter.description));
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, name](double value) {
            store(name, ParameterValue::fromReal(value));
        });
        m_form->addRow(label, spin);
        break;
    }

    case ParameterValue::Type::Text: {
        if (isSourceCode(parameter.name)) {
            auto* editor = new QPlainTextEdit;
            editor->setPlainText(QString::fromStdString(values.text(name)));
            editor->setToolTip(toQt(parameter.description));
            editor->setMinimumHeight(140);
            editor->setLineWrapMode(QPlainTextEdit::NoWrap);
            editor->setProperty("torquebusRole", QStringLiteral("code"));

            // Committed on focus loss - see eventFilter below. The
            // parameter name rides on the widget so the filter can find it
            // without a second map to keep in step.
            editor->setProperty(kParameterProperty, QString::fromStdString(name));
            editor->installEventFilter(this);

            m_form->addRow(label, editor);
            break;
        }

        auto* edit = new QLineEdit(QString::fromStdString(values.text(name)));
        edit->setToolTip(toQt(parameter.description));
        edit->setPlaceholderText(parameter.required ? tr("required") : tr("optional"));

        connect(edit, &QLineEdit::editingFinished, this, [this, name, edit] {
            store(name, ParameterValue::fromText(edit->text().toStdString()));
        });

        if (isFilePath(parameter.name)) {
            auto* row = new QWidget;
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(4);
            rowLayout->addWidget(edit, 1);

            const FileChoice choice = fileChoiceFor(parameter.name);

            auto* browse = new QPushButton(tr("..."));
            browse->setFixedWidth(28);
            browse->setToolTip(choice.title);
            connect(browse, &QPushButton::clicked, this, [this, name, edit, choice] {
                const QString chosen = QFileDialog::getOpenFileName(
                    this, choice.title, edit->text(), choice.filter);

                if (!chosen.isEmpty()) {
                    edit->setText(chosen);
                    store(name, ParameterValue::fromText(chosen.toStdString()));
                }
            });
            rowLayout->addWidget(browse);

            m_form->addRow(label, row);
            break;
        }

        m_form->addRow(label, edit);
        break;
    }
    }
}

void NodePropertiesEditor::addScriptParameterRows(const NodeDescription& node,
                                                  const NodeTypeInfo& info)
{
    if (!info.acceptsExtraParameters) {
        return;
    }

    // Which names the type already claimed, so they are not offered twice.
    std::set<std::string> declared;
    for (const ParameterDescriptor& parameter : info.parameters) {
        declared.emplace(parameter.name);
    }

    auto* heading = new QLabel(tr("Script parameters"));
    heading->setProperty("torquebusRole", QStringLiteral("panelHeading"));
    heading->setToolTip(tr("Read by the script through its `parameters` table. "
                           "The names are the script's own."));
    m_form->addRow(heading);

    for (const auto& [name, value] : node.parameters.values()) {
        if (declared.contains(name)) {
            continue;
        }

        // Typed by what the value already is. There is no descriptor to consult
        // - that is the whole point of these - so the value carries its own
        // type, and the project file preserved it precisely so this works.
        auto* edit = new QLineEdit(scriptValueText(value));
        edit->setToolTip(tr("Read by the script as parameters.%1")
                             .arg(QString::fromStdString(name)));

        const std::string key = name;
        connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] {
            store(key, parseScriptValue(edit->text()));
        });

        m_form->addRow(QString::fromStdString(name), edit);
    }

    // Adding one. Without this a freshly dropped ECU could never be given the
    // settings its script reads, and the only way to configure it would be to
    // hand-edit the .tbsproj.
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(4);

    auto* name = new QLineEdit;
    name->setPlaceholderText(tr("name"));

    auto* add = new QPushButton(tr("Add"));
    add->setEnabled(false);

    connect(name, &QLineEdit::textChanged, add, [add](const QString& text) {
        add->setEnabled(!text.trimmed().isEmpty());
    });

    connect(add, &QPushButton::clicked, this, [this, name] {
        const QString key = name->text().trimmed();
        if (key.isEmpty()) {
            return;
        }

        // Created empty and typed on first edit. Guessing a type from a name
        // would be guessing.
        store(key.toStdString(), ParameterValue::fromText(std::string{}));

        // Rebuilt so the new row appears where the others are, in order.
        rebuild();
    });

    rowLayout->addWidget(name, 1);
    rowLayout->addWidget(add);

    m_form->addRow(QString{}, row);
}

bool NodePropertiesEditor::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FocusOut) {
        if (auto* editor = qobject_cast<QPlainTextEdit*>(watched)) {
            const QString parameter = editor->property(kParameterProperty).toString();

            if (!parameter.isEmpty()) {
                store(parameter.toStdString(),
                      ParameterValue::fromText(editor->toPlainText().toStdString()));
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

} // namespace torquebus::ui
