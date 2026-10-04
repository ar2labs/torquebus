// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/properties/NodePropertiesEditor.h"

#include "ui/theme/ThemeManager.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include <limits>
#include <set>
#include <string>

namespace torquebus::ui {
namespace {

[[nodiscard]] QString iconNameForNodeType(const QString& typeName)
{
    if (typeName == QLatin1String("can.source")) {
        return QStringLiteral("hardware");
    }
    if (typeName == QLatin1String("transmit.list")) {
        return QStringLiteral("transmit");
    }
    if (typeName == QLatin1String("log.source")) {
        return QStringLiteral("replay");
    }
    if (typeName == QLatin1String("can.filter")) {
        return QStringLiteral("filter");
    }
    if (typeName == QLatin1String("dbc.decoder")) {
        return QStringLiteral("database");
    }
    if (typeName == QLatin1String("j1939.decoder")) {
        return QStringLiteral("network");
    }
    if (typeName == QLatin1String("lua.ecu")) {
        return QStringLiteral("script");
    }
    if (typeName == QLatin1String("sim.restbus")) {
        return QStringLiteral("restbus");
    }
    if (typeName == QLatin1String("lua.test")) {
        return QStringLiteral("test");
    }
    if (typeName == QLatin1String("tinyml.ecu")) {
        return QStringLiteral("neural");
    }
    if (typeName == QLatin1String("uds.client")) {
        return QStringLiteral("diagnostics");
    }
    if (typeName == QLatin1String("isotp.transport")) {
        return QStringLiteral("network");
    }
    if (typeName == QLatin1String("can.transmit")) {
        return QStringLiteral("transmit");
    }
    if (typeName == QLatin1String("trace.sink")) {
        return QStringLiteral("trace");
    }
    if (typeName == QLatin1String("can.log")) {
        return QStringLiteral("save");
    }
    if (typeName == QLatin1String("signal.plot")) {
        return QStringLiteral("graph");
    }
    return QStringLiteral("hardware");
}

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
    return name == "scriptPath" || name == "database" || name == "path" || name == "modelPath";
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

    if (name == "modelPath") {
        return {NodePropertiesEditor::tr("Choose a TinyML model"),
                NodePropertiesEditor::tr("TorqueBus TinyML models (*.tbusml);;All files (*)")};
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

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { rebuild(); });
    }

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
    NodeDescription* node =
        const_cast<NodeDescription*>(m_description.find(m_nodeId.toStdString()));

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

    const NodeDescription* node =
        m_nodeId.isEmpty() ? nullptr : m_description.find(m_nodeId.toStdString());

    const NodeTypeInfo* info = node == nullptr ? nullptr : m_catalog.find(node->typeName);

    m_placeholder->setVisible(node == nullptr);
    m_formHost->setVisible(node != nullptr);

    if (node == nullptr || info == nullptr) {
        return;
    }

    ThemeManager* themes = ThemeManager::instance();

    // High quality Block Summary Card with crisp SVG icon and metadata
    auto* headerCard = new QFrame(m_formHost);
    headerCard->setObjectName(QStringLiteral("nodeHeaderCard"));
    headerCard->setFrameShape(QFrame::StyledPanel);
    headerCard->setStyleSheet(
        QStringLiteral("QFrame#nodeHeaderCard { "
                       "  background: rgba(128, 128, 128, 0.08); "
                       "  border: 1px solid rgba(128, 128, 128, 0.2); "
                       "  border-radius: 6px; padding: 6px; margin-bottom: 6px; "
                       "}"));

    auto* cardLayout = new QHBoxLayout(headerCard);
    cardLayout->setContentsMargins(6, 4, 6, 4);
    cardLayout->setSpacing(8);

    auto* iconLabel = new QLabel(headerCard);
    iconLabel->setFixedSize(28, 28);
    iconLabel->setAlignment(Qt::AlignCenter);
    const QString nodeIcon = iconNameForNodeType(QString::fromStdString(node->typeName));
    if (themes != nullptr) {
        iconLabel->setPixmap(themes->icon(nodeIcon).pixmap(24, 24));
    }
    cardLayout->addWidget(iconLabel);

    auto* textLayout = new QVBoxLayout;
    textLayout->setSpacing(1);
    textLayout->setContentsMargins(0, 0, 0, 0);

    auto* titleLabel = new QLabel(m_nodeId, headerCard);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    textLayout->addWidget(titleLabel);

    const QString subtitle = QStringLiteral("%1  •  %2")
                                 .arg(QString::fromStdString(info->displayName))
                                 .arg(QString::fromStdString(info->category));
    auto* subtitleLabel = new QLabel(subtitle, headerCard);
    subtitleLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
    textLayout->addWidget(subtitleLabel);

    cardLayout->addLayout(textLayout, 1);

    auto* enabled = new QCheckBox(tr("Enabled"), headerCard);
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
    cardLayout->addWidget(enabled);

    m_form->addRow(headerCard);

    for (const ParameterDescriptor& parameter : info->parameters) {
        addDeclaredRow(*node, parameter);
    }

    addScriptParameterRows(*node, *info);

    auto* deleteButton = new QPushButton(tr("Delete Block"), m_formHost);
    deleteButton->setObjectName(QStringLiteral("deleteBlockButton"));
    deleteButton->setToolTip(tr("Delete this block and all its connections from the Pipeline"));
    if (themes != nullptr) {
        deleteButton->setIcon(themes->icon(QStringLiteral("clear")));
        deleteButton->setIconSize(QSize(14, 14));
    }
    deleteButton->setStyleSheet(
        QStringLiteral("QPushButton#deleteBlockButton { "
                       "  background-color: #ef4444; color: white; font-weight: bold; "
                       "  border: none; border-radius: 4px; padding: 6px 14px; margin-top: 14px; "
                       "} "
                       "QPushButton#deleteBlockButton:hover { background-color: #dc2626; } "
                       "QPushButton#deleteBlockButton:pressed { background-color: #b91c1c; }"));
    connect(deleteButton, &QPushButton::clicked, this, [this] {
        const QString nodeId = m_nodeId;
        if (!nodeId.isEmpty()) {
            clear();
            Q_EMIT deleteBlockRequested(nodeId);
        }
    });
    m_form->addRow(QString{}, deleteButton);
}

void NodePropertiesEditor::addDeclaredRow(const NodeDescription& node,
                                          const ParameterDescriptor& parameter)
{
    const std::string name{parameter.name};
    const QString label = parameter.required ? tr("%1 *").arg(toQt(parameter.displayName))
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
        spin->setRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
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

            auto* browse = new QPushButton(m_formHost);
            browse->setFixedWidth(28);
            ThemeManager* themes = ThemeManager::instance();
            if (themes != nullptr) {
                browse->setIcon(themes->icon(QStringLiteral("open")));
                browse->setIconSize(QSize(14, 14));
            } else {
                browse->setText(tr("..."));
            }
            browse->setToolTip(choice.title);
            connect(browse, &QPushButton::clicked, this, [this, name, edit, choice] {
                const QString chosen =
                    QFileDialog::getOpenFileName(this, choice.title, edit->text(), choice.filter);

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
        edit->setToolTip(
            tr("Read by the script as parameters.%1").arg(QString::fromStdString(name)));

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

    auto* add = new QPushButton(tr("Add"), m_formHost);
    ThemeManager* themes = ThemeManager::instance();
    if (themes != nullptr) {
        add->setIcon(themes->icon(QStringLiteral("add")));
        add->setIconSize(QSize(14, 14));
    }
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
