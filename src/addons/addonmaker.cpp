// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "addonmaker.h"

#include "addonprompt.h"
#include "dialogs.h"
#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

const QStringList kTypes = {QStringLiteral("text"), QStringLiteral("number"), QStringLiteral("choice"),
                            QStringLiteral("check"), QStringLiteral("folder"), QStringLiteral("file")};
const QStringList kConditions = {QStringLiteral("mounted"), QStringLiteral("unmounted"), QStringLiteral("removable"),
                                 QStringLiteral("internal"), QStringLiteral("encrypted"), QStringLiteral("unlocked"),
                                 QStringLiteral("locked"), QStringLiteral("has-health")};

// "Quiet=-q, Chatty=-v" -> pairs. Commas separate; the first "=" splits name and value.
QVector<QPair<QString, QString>> splitOptions(const QString &text)
{
    QVector<QPair<QString, QString>> out;
    for (const QString &item : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString t = item.trimmed();
        const qsizetype eq = t.indexOf(QLatin1Char('='));
        if (eq < 0)
            out.push_back({t, t});
        else
            out.push_back({t.left(eq).trimmed(), t.mid(eq + 1).trimmed()});
    }
    return out;
}

// "My Backup" -> "my-backup"
QString slug(const QString &name)
{
    QString s = name.toLower();
    s.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    while (s.startsWith(QLatin1Char('-')))
        s.remove(0, 1);
    while (s.endsWith(QLatin1Char('-')))
        s.chop(1);
    return s.left(40);
}

QJsonObject parse(const char *json)
{
    return QJsonDocument::fromJson(json).object();
}

// Ready-made starting points. Their labels are translated where they're listed.
QJsonObject templateFor(int index)
{
    switch (index) {
    case 1:
        return parse(R"json({"name":"Folder Sizes","description":"Lists the folders on a mounted drive, biggest last.",
            "actions":[{"label":"Show Folder Sizes","icon":"view-statistics","when":["mounted"],"look_only":true,"system_disks":true,
            "output":"window","command":["sh","-c","du -xh --max-depth=1 \"$1\" 2>/dev/null | sort -h","sh","{mountpoint}"]}]})json");
    case 2:
        return parse(R"json({"name":"Open in App","description":"Opens a mounted drive in the app you use for folders.",
            "actions":[{"label":"Open in App","icon":"system-file-manager","when":["mounted"],"command":["xdg-open","{mountpoint}"]}]})json");
    case 3:
        return parse(R"json({"name":"Copy UUID","description":"Copies a partition's UUID, for /etc/fstab and the like. Needs wl-clipboard.",
            "actions":[{"label":"Copy UUID","icon":"edit-copy","command":["wl-copy","{uuid}"]}]})json");
    case 4:
        return parse(R"json({"name":"Back Up with rsync","description":"Copies everything on a mounted drive to a folder you pick. Needs rsync.",
            "actions":[{"label":"Back Up to a Folder","icon":"document-save","when":["mounted"],"output":"window",
            "confirm":"Copy everything on {label} to the folder you picked?",
            "command":["rsync","-a","--info=progress2","--mkpath","{ask:delete}","{mountpoint}/","{ask:folder}/{label}/"],
            "ask":[{"id":"folder","type":"folder","label":"Back up to","default":"{home}/Backups"},
                   {"id":"delete","type":"check","label":"Also delete files that are gone from the drive","on":"--delete","off":""}]}]})json");
    case 5:
        return parse(R"json({"name":"SMART Report","description":"Shows everything smartctl knows about a drive. Needs smartmontools.",
            "actions":[{"label":"Full SMART Report","icon":"dialog-information","applies_to":"disk","when":["has-health"],
            "output":"terminal","command":["sudo","smartctl","-x","{disk}"]}]})json");
    default:
        return parse(R"json({"name":"My Add-on","actions":[{"label":"My Action","command":[""]}]})json");
    }
}

} // namespace

FieldTable::FieldTable(QWidget *parent)
    : QWidget(parent)
    , m_table(new QTableWidget(0, 5))
{
    m_table->setHorizontalHeaderLabels({tr("Id"), tr("Type"), tr("Label"), tr("Default"), tr("Options")});
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeaderItem(4)->setToolTip(tr("For a choice: Name=value, Name=value\n"
                                                    "For a checkbox: on=value when ticked, off=value when not\n"
                                                    "For a number: min=0, max=100"));
    connect(m_table, &QTableWidget::itemChanged, this, &FieldTable::changed);
    auto *add = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add Field"));
    auto *remove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"));
    connect(add, &QPushButton::clicked, this, [this] {
        int n = m_table->rowCount() + 1;
        while (ids().contains(QStringLiteral("field%1").arg(n)))
            ++n;
        addRow(QJsonObject{{QStringLiteral("id"), QStringLiteral("field%1").arg(n)}, {QStringLiteral("type"), QStringLiteral("text")}});
        emit changed();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (m_table->currentRow() >= 0) {
            m_table->removeRow(m_table->currentRow());
            emit changed();
        }
    });
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch();
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_table);
    layout->addLayout(buttons);
}

void FieldTable::addRow(const QJsonObject &f)
{
    const QSignalBlocker block(m_table);
    const int r = m_table->rowCount();
    m_table->insertRow(r);
    m_table->setItem(r, 0, new QTableWidgetItem(f.value(QStringLiteral("id")).toString()));
    auto *type = new QComboBox;
    type->addItems(kTypes);
    type->setCurrentText(f.value(QStringLiteral("type")).toString(QStringLiteral("text")));
    connect(type, &QComboBox::currentIndexChanged, this, &FieldTable::changed);
    m_table->setCellWidget(r, 1, type);
    m_table->setItem(r, 2, new QTableWidgetItem(f.value(QStringLiteral("label")).toString()));
    m_table->setItem(r, 3, new QTableWidgetItem(f.value(QStringLiteral("default")).toVariant().toString()));
    QStringList options;
    for (const QJsonValue &c : f.value(QStringLiteral("choices")).toArray()) {
        const QJsonObject choice = c.toObject();
        options << (c.isString() ? c.toString()
                                 : choice.value(QStringLiteral("label")).toString() + QLatin1Char('=') + choice.value(QStringLiteral("value")).toString());
    }
    if (f.contains(QStringLiteral("on")) || f.contains(QStringLiteral("off")))
        options << QStringLiteral("on=") + f.value(QStringLiteral("on")).toString() << QStringLiteral("off=") + f.value(QStringLiteral("off")).toString();
    if (f.contains(QStringLiteral("min")) || f.contains(QStringLiteral("max")))
        options << QStringLiteral("min=%1").arg(f.value(QStringLiteral("min")).toInteger(0))
                << QStringLiteral("max=%1").arg(f.value(QStringLiteral("max")).toInteger(1000000));
    m_table->setItem(r, 4, new QTableWidgetItem(options.join(QStringLiteral(", "))));
}

void FieldTable::setFields(const QJsonArray &fields)
{
    m_table->setRowCount(0);
    for (const QJsonValue &f : fields)
        addRow(f.toObject());
}

QStringList FieldTable::ids() const
{
    QStringList out;
    for (int r = 0; r < m_table->rowCount(); ++r)
        out << (m_table->item(r, 0) ? m_table->item(r, 0)->text().trimmed() : QString());
    return out;
}

QJsonArray FieldTable::fields() const
{
    QJsonArray out;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        auto text = [this, r](int c) { return m_table->item(r, c) ? m_table->item(r, c)->text().trimmed() : QString(); };
        const QString type = qobject_cast<QComboBox *>(m_table->cellWidget(r, 1))->currentText();
        QJsonObject f{{QStringLiteral("id"), text(0)}, {QStringLiteral("type"), type}};
        if (!text(2).isEmpty())
            f.insert(QStringLiteral("label"), text(2));
        if (!text(3).isEmpty())
            f.insert(QStringLiteral("default"), text(3));
        const auto options = splitOptions(text(4));
        if (type == QLatin1String("choice")) {
            QJsonArray choices;
            for (const auto &[name, value] : options)
                choices.append(QJsonObject{{QStringLiteral("label"), name}, {QStringLiteral("value"), value}});
            f.insert(QStringLiteral("choices"), choices);
        } else if (type == QLatin1String("check")) {
            for (const auto &[name, value] : options) {
                if (name == QLatin1String("on") || name == QLatin1String("off"))
                    f.insert(name, value);
            }
        } else if (type == QLatin1String("number")) {
            for (const auto &[name, value] : options) {
                if (name == QLatin1String("min") || name == QLatin1String("max"))
                    f.insert(name, value.toLongLong());
            }
        }
        out.append(f);
    }
    return out;
}

AddonMaker::AddonMaker(Addons *addons, const Addon *existing, Tester tester, QWidget *parent)
    : QDialog(parent)
    , m_addons(addons)
    , m_tester(std::move(tester))
    , m_template(new QComboBox)
    , m_name(new QLineEdit)
    , m_author(new QLineEdit)
    , m_version(new QLineEdit(QStringLiteral("1.0")))
    , m_id(new QLineEdit)
    , m_description(new QLineEdit)
    , m_settings(new FieldTable)
    , m_actionList(new QListWidget)
    , m_label(new QLineEdit)
    , m_icon(new QComboBox)
    , m_command(new QTableWidget(0, 1))
    , m_appliesTo(new QComboBox)
    , m_filesystems(new QLineEdit)
    , m_systemDisks(new QCheckBox(tr("Also offer it on the disk the system runs from (look-only actions only)")))
    , m_output(new QComboBox)
    , m_lookOnly(new QCheckBox(tr("Look-only: run it in a read-only sandbox (no changing files, no network)")))
    , m_confirm(new QLineEdit)
    , m_ask(new FieldTable)
    , m_check(new QLabel)
    , m_saveButton(new QPushButton(tr("Save")))
    , m_testButton(new QPushButton(QIcon::fromTheme(QStringLiteral("media-playback-start")), tr("Test")))
{
    setWindowTitle(tr("Make an Add-on"));
    m_name->setObjectName(QStringLiteral("name"));
    m_label->setObjectName(QStringLiteral("label"));
    m_command->setObjectName(QStringLiteral("command"));
    m_check->setWordWrap(true);
    m_check->setTextFormat(Qt::RichText);

    m_template->addItems({tr("Start from…"), tr("Show folder sizes (look-only)"), tr("Open the drive in an app"),
                          tr("Copy something to the clipboard"), tr("Back up with rsync (asks for the folder)"),
                          tr("Run a check in a terminal")});
    connect(m_template, &QComboBox::activated, this, &AddonMaker::startFrom);

    // Add-on details.
    m_id->setToolTip(tr("Lowercase letters, digits and dashes; made from the name unless you change it"));
    connect(m_id, &QLineEdit::textEdited, this, [this] { m_idEdited = true; });
    connect(m_name, &QLineEdit::textChanged, this, [this](const QString &name) {
        if (!m_idEdited && !m_loading)
            m_id->setText(slug(name));
    });
    auto *details = new QFormLayout;
    details->addRow(tr("Name:"), m_name);
    details->addRow(tr("Made by:"), m_author);
    auto *idRow = new QHBoxLayout;
    idRow->addWidget(m_id, 1);
    idRow->addWidget(new QLabel(tr("Version:")));
    idRow->addWidget(m_version);
    details->addRow(tr("Id:"), idRow);
    details->addRow(tr("What it's for:"), m_description);

    // Actions on the left.
    auto *addAction = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add"));
    addAction->setToolTip(tr("Add an action"));
    auto *removeAction = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"));
    removeAction->setToolTip(tr("Remove this action"));
    auto *upAction = new QPushButton(QIcon::fromTheme(QStringLiteral("go-up")), tr("Up"));
    auto *downAction = new QPushButton(QIcon::fromTheme(QStringLiteral("go-down")), tr("Down"));
    connect(addAction, &QPushButton::clicked, this, [this] {
        storeAction();
        QString label = tr("New Action");
        for (int n = 2; std::any_of(m_actions.cbegin(), m_actions.cend(), [&label](const QJsonObject &a) {
                 return a.value(QStringLiteral("label")).toString() == label;
             });
             ++n)
            label = tr("New Action %1").arg(n);
        m_actions.append(QJsonObject{{QStringLiteral("label"), label}, {QStringLiteral("command"), QJsonArray{QString()}}});
        m_actionList->addItem(label);
        m_actionList->setCurrentRow(int(m_actions.size()) - 1);
    });
    connect(removeAction, &QPushButton::clicked, this, [this] {
        if (m_current < 0 || m_actions.size() <= 1)
            return;
        const int row = m_current;
        m_current = -1;
        m_actions.removeAt(row);
        delete m_actionList->takeItem(row);
        m_actionList->setCurrentRow(qMin(row, int(m_actions.size()) - 1));
        refresh();
    });
    auto move = [this](int by) {
        storeAction();
        const int from = m_current, to = m_current + by;
        if (from < 0 || to < 0 || to >= m_actions.size())
            return;
        m_actions.swapItemsAt(from, to);
        m_current = -1;
        m_actionList->insertItem(to, m_actionList->takeItem(from));
        m_actionList->setCurrentRow(to);
        refresh();
    };
    connect(upAction, &QPushButton::clicked, this, [move] { move(-1); });
    connect(downAction, &QPushButton::clicked, this, [move] { move(1); });
    connect(m_actionList, &QListWidget::currentRowChanged, this, [this](int row) {
        storeAction();
        showAction(row);
    });
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(new QLabel(tr("Actions:")));
    leftLayout->addWidget(m_actionList, 1);
    auto *actionButtons = new QHBoxLayout;
    for (QPushButton *b : {addAction, removeAction, upAction, downAction})
        actionButtons->addWidget(b);
    actionButtons->addStretch();
    leftLayout->addLayout(actionButtons);

    // "What it runs": the command, one part per row, like the run question shows it.
    m_icon->setEditable(true);
    for (const char *name : {"application-x-addon", "utilities-terminal", "system-file-manager", "document-save", "edit-copy",
                             "view-statistics", "dialog-information", "drive-harddisk", "media-flash", "folder", "edit-find",
                             "tools-report-bug", "system-run"})
        m_icon->addItem(QIcon::fromTheme(QLatin1String(name)), QLatin1String(name));
    m_command->setHorizontalHeaderLabels({tr("Command, one part per row (the first is the program)")});
    m_command->horizontalHeader()->setStretchLastSection(true);
    m_command->verticalHeader()->setVisible(true);
    auto *addPart = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add Part"));
    auto *removePart = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"));
    auto *insert = new QPushButton(tr("Insert"));
    auto *insertMenu = new QMenu(insert);
    insert->setMenu(insertMenu);
    connect(insertMenu, &QMenu::aboutToShow, this, [this, insertMenu] {
        insertMenu->clear();
        const QList<QPair<QString, QString>> drive = {
            {tr("The drive's folder"), QStringLiteral("{mountpoint}")}, {tr("Partition, like /dev/sdb1"), QStringLiteral("{device}")},
            {tr("Whole drive, like /dev/sdb"), QStringLiteral("{disk}")},   {tr("Name (label)"), QStringLiteral("{label}")},
            {tr("UUID"), QStringLiteral("{uuid}")},                         {tr("File system type"), QStringLiteral("{fstype}")},
            {tr("Size in bytes"), QStringLiteral("{size}")},                {tr("Drive model"), QStringLiteral("{model}")},
            {tr("Your home folder"), QStringLiteral("{home}")},
        };
        for (const auto &[label, text] : drive)
            insertMenu->addAction(QStringLiteral("%1   %2").arg(label, text), this, [this, text] { insertPlaceholder(text); });
        const QStringList asks = m_ask->ids(), settings = m_settings->ids();
        if (!asks.isEmpty() || !settings.isEmpty())
            insertMenu->addSeparator();
        for (const QString &id : asks)
            insertMenu->addAction(tr("Form field %1").arg(id), this, [this, id] { insertPlaceholder(QStringLiteral("{ask:%1}").arg(id)); });
        for (const QString &id : settings)
            insertMenu->addAction(tr("Setting %1").arg(id), this, [this, id] { insertPlaceholder(QStringLiteral("{setting:%1}").arg(id)); });
    });
    auto *paste = new QPushButton(tr("Type a Command Line…"));
    connect(addPart, &QPushButton::clicked, this, [this] {
        const int r = m_command->currentRow() >= 0 ? m_command->currentRow() + 1 : m_command->rowCount();
        m_command->insertRow(r);
        m_command->setItem(r, 0, new QTableWidgetItem);
        m_command->setCurrentCell(r, 0);
        m_command->editItem(m_command->item(r, 0));
    });
    connect(removePart, &QPushButton::clicked, this, [this] {
        if (m_command->currentRow() >= 0)
            m_command->removeRow(m_command->currentRow());
        storeAction();
        refresh();
    });
    connect(paste, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString line = QInputDialog::getText(this, tr("Type a Command Line"),
                                                   tr("Like in a terminal; it's split into parts the same way (quotes keep spaces together):"),
                                                   QLineEdit::Normal, QString(), &ok);
        if (!ok)
            return;
        const QStringList parts = QProcess::splitCommand(line);
        m_command->setRowCount(0);
        for (const QString &part : parts) {
            const int r = m_command->rowCount();
            m_command->insertRow(r);
            m_command->setItem(r, 0, new QTableWidgetItem(part));
        }
        storeAction();
        refresh();
    });
    auto *runs = new QWidget;
    auto *runsLayout = new QVBoxLayout(runs);
    auto *runsForm = new QFormLayout;
    runsForm->addRow(tr("Menu text:"), m_label);
    runsForm->addRow(tr("Icon:"), m_icon);
    runsLayout->addLayout(runsForm);
    runsLayout->addWidget(m_command, 1);
    auto *partButtons = new QHBoxLayout;
    for (QPushButton *b : {addPart, removePart, insert, paste})
        partButtons->addWidget(b);
    partButtons->addStretch();
    runsLayout->addLayout(partButtons);

    // "Where it shows".
    m_appliesTo->addItem(tr("A partition"), QStringLiteral("volume"));
    m_appliesTo->addItem(tr("A whole drive"), QStringLiteral("disk"));
    m_appliesTo->addItem(tr("Unallocated space"), QStringLiteral("free"));
    m_appliesTo->addItem(tr("Anything"), QStringLiteral("any"));
    const QMap<QString, QString> conditionText = {
        {QStringLiteral("mounted"), tr("Mounted")},         {QStringLiteral("unmounted"), tr("Not mounted")},
        {QStringLiteral("removable"), tr("USB or removable")}, {QStringLiteral("internal"), tr("Internal")},
        {QStringLiteral("encrypted"), tr("Encrypted")},     {QStringLiteral("unlocked"), tr("Unlocked")},
        {QStringLiteral("locked"), tr("Locked")},           {QStringLiteral("has-health"), tr("Reports its health")},
    };
    auto *whenGrid = new QGridLayout;
    for (int i = 0; i < kConditions.size(); ++i) {
        auto *box = new QCheckBox(conditionText.value(kConditions[i]));
        m_when.insert(kConditions[i], box);
        whenGrid->addWidget(box, i / 4, i % 4);
    }
    m_filesystems->setPlaceholderText(tr("Any file system; or a list like ext4, btrfs"));
    auto *shows = new QWidget;
    auto *showsForm = new QFormLayout(shows);
    showsForm->addRow(tr("Works on:"), m_appliesTo);
    showsForm->addRow(tr("Only when:"), whenGrid);
    showsForm->addRow(tr("File systems:"), m_filesystems);
    showsForm->addRow(m_systemDisks);

    // "How it runs".
    m_output->addItem(tr("Just start it (no output shown)"), QStringLiteral("none"));
    m_output->addItem(tr("In a terminal window"), QStringLiteral("terminal"));
    m_output->addItem(tr("In a DiskForge window"), QStringLiteral("window"));
    m_confirm->setPlaceholderText(tr("Optional question before it runs, like: Copy everything on {label}?"));
    auto *how = new QWidget;
    auto *howForm = new QFormLayout(how);
    howForm->addRow(tr("Output:"), m_output);
    howForm->addRow(m_lookOnly);
    howForm->addRow(tr("Ask first:"), m_confirm);

    auto *tabs = new QTabWidget;
    tabs->addTab(runs, tr("What It Runs"));
    tabs->addTab(shows, tr("Where It Shows"));
    tabs->addTab(how, tr("How It Runs"));
    tabs->addTab(m_ask, tr("Asks For"));
    auto *settingsPage = new QWidget;
    auto *settingsLayout = new QVBoxLayout(settingsPage);
    auto *settingsNote = new QLabel(tr("Settings belong to the whole add-on and are set once, in the Add-ons window. "
                                       "Use them in a command with Insert."));
    settingsNote->setWordWrap(true);
    settingsLayout->addWidget(settingsNote);
    settingsLayout->addWidget(m_settings, 1);
    tabs->addTab(settingsPage, tr("Settings"));

    auto *split = new QSplitter;
    split->addWidget(left);
    split->addWidget(tabs);
    split->setStretchFactor(1, 3);

    // Every change is stored and checked right away.
    for (QLineEdit *edit : {m_name, m_author, m_version, m_id, m_description})
        connect(edit, &QLineEdit::textChanged, this, &AddonMaker::refresh);
    for (QLineEdit *edit : {m_label, m_filesystems, m_confirm})
        connect(edit, &QLineEdit::textChanged, this, [this] {
            storeAction();
            refresh();
        });
    connect(m_label, &QLineEdit::textChanged, this, [this](const QString &label) {
        if (QListWidgetItem *item = m_actionList->item(m_current))
            item->setText(label);
    });
    connect(m_icon, &QComboBox::currentTextChanged, this, [this] {
        storeAction();
        refresh();
    });
    for (QComboBox *box : {m_appliesTo, m_output})
        connect(box, &QComboBox::currentIndexChanged, this, [this] {
            storeAction();
            refresh();
        });
    for (QCheckBox *box : m_when)
        connect(box, &QCheckBox::toggled, this, [this] {
            storeAction();
            refresh();
        });
    for (QCheckBox *box : {m_systemDisks, m_lookOnly})
        connect(box, &QCheckBox::toggled, this, [this] {
            storeAction();
            refresh();
        });
    connect(m_command, &QTableWidget::itemChanged, this, [this] {
        storeAction();
        refresh();
    });
    connect(m_ask, &FieldTable::changed, this, [this] {
        storeAction();
        refresh();
    });
    connect(m_settings, &FieldTable::changed, this, &AddonMaker::refresh);

    auto *saveCopyButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save-as")), tr("Save a Copy…"));
    m_testButton->setToolTip(tr("Runs this action on what's selected in the main window. It asks first, like any add-on."));
    connect(m_testButton, &QPushButton::clicked, this, [this] {
        storeAction();
        const Addon a = Addons::parseData(json(), QString());
        if (a.error.isEmpty() && m_current >= 0 && m_current < a.actions.size() && m_tester)
            m_tester(a, a.actions[m_current]);
    });
    connect(saveCopyButton, &QPushButton::clicked, this, &AddonMaker::saveCopy);
    connect(m_saveButton, &QPushButton::clicked, this, &AddonMaker::save);
    auto *cancel = new QPushButton(tr("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_testButton);
    bottom->addWidget(saveCopyButton);
    bottom->addStretch();
    bottom->addWidget(cancel);
    bottom->addWidget(m_saveButton);
    m_saveButton->setDefault(true);

    auto *top = new QHBoxLayout;
    top->addWidget(m_template);
    top->addStretch();
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addLayout(details);
    layout->addWidget(split, 1);
    layout->addWidget(m_check);
    layout->addLayout(bottom);
    resize(920, 680);

    QJsonObject start = templateFor(0);
    if (existing) {
        QFile f(existing->file);
        if (f.open(QIODevice::ReadOnly))
            start = QJsonDocument::fromJson(f.read(Addons::kMaxDownload * 4)).object();
        m_originalId = existing->id;
        m_keepOutsideFlag = existing->outside;
        setWindowTitle(existing->systemWide ? tr("Edit a Copy of %1").arg(existing->name) : tr("Edit %1").arg(existing->name));
        m_template->hide();
    }
    loadManifest(start);
}

void AddonMaker::loadManifest(const QJsonObject &o)
{
    m_loading = true;
    m_name->setText(o.value(QStringLiteral("name")).toString());
    m_author->setText(o.value(QStringLiteral("author")).toString());
    m_version->setText(o.value(QStringLiteral("version")).toString(QStringLiteral("1.0")));
    m_id->setText(o.value(QStringLiteral("id")).toString(slug(m_name->text())));
    m_idEdited = o.contains(QStringLiteral("id"));
    m_description->setText(o.value(QStringLiteral("description")).toString());
    m_settings->setFields(o.value(QStringLiteral("settings")).toArray());
    m_actions.clear();
    m_current = -1;
    m_actionList->clear();
    for (const QJsonValue &a : o.value(QStringLiteral("actions")).toArray()) {
        m_actions.append(a.toObject());
        m_actionList->addItem(a.toObject().value(QStringLiteral("label")).toString());
    }
    m_loading = false;
    m_actionList->setCurrentRow(m_actions.isEmpty() ? -1 : 0);
    refresh();
}

void AddonMaker::showAction(int row)
{
    m_current = row;
    if (row < 0 || row >= m_actions.size())
        return;
    const QJsonObject a = m_actions[row];
    m_loading = true;
    m_label->setText(a.value(QStringLiteral("label")).toString());
    m_icon->setCurrentText(a.value(QStringLiteral("icon")).toString(QStringLiteral("application-x-addon")));
    m_command->setRowCount(0);
    for (const QJsonValue &part : a.value(QStringLiteral("command")).toArray()) {
        const int r = m_command->rowCount();
        m_command->insertRow(r);
        m_command->setItem(r, 0, new QTableWidgetItem(part.toString()));
    }
    m_appliesTo->setCurrentIndex(qMax(0, m_appliesTo->findData(a.value(QStringLiteral("applies_to")).toString(QStringLiteral("volume")))));
    const QJsonArray when = a.value(QStringLiteral("when")).toArray();
    QString filesystems;
    for (auto it = m_when.constBegin(); it != m_when.constEnd(); ++it)
        it.value()->setChecked(when.contains(it.key()));
    for (const QJsonValue &w : when) {
        if (w.toString().startsWith(QLatin1String("filesystem:")))
            filesystems = w.toString().mid(11).split(QLatin1Char('|')).join(QStringLiteral(", "));
    }
    m_filesystems->setText(filesystems);
    m_lookOnly->setChecked(a.value(QStringLiteral("look_only")).toBool());
    m_systemDisks->setChecked(a.value(QStringLiteral("system_disks")).toBool());
    QString output = a.value(QStringLiteral("output")).toString();
    if (output.isEmpty())
        output = a.value(QStringLiteral("terminal")).toBool() ? QStringLiteral("terminal") : QStringLiteral("none");
    m_output->setCurrentIndex(qMax(0, m_output->findData(output)));
    m_confirm->setText(a.value(QStringLiteral("confirm")).toString());
    m_ask->setFields(a.value(QStringLiteral("ask")).toArray());
    m_loading = false;
    refresh();
}

void AddonMaker::storeAction()
{
    if (m_loading || m_current < 0 || m_current >= m_actions.size())
        return;
    QJsonObject a{{QStringLiteral("label"), m_label->text().trimmed()}};
    if (!m_icon->currentText().isEmpty() && m_icon->currentText() != QLatin1String("application-x-addon"))
        a.insert(QStringLiteral("icon"), m_icon->currentText().trimmed());
    if (m_appliesTo->currentData().toString() != QLatin1String("volume"))
        a.insert(QStringLiteral("applies_to"), m_appliesTo->currentData().toString());
    QJsonArray when;
    for (auto it = m_when.constBegin(); it != m_when.constEnd(); ++it) {
        if (it.value()->isChecked())
            when.append(it.key());
    }
    QStringList filesystems;
    for (const QString &fs : m_filesystems->text().split(QRegularExpression(QStringLiteral("[,|\\s]+")), Qt::SkipEmptyParts))
        filesystems << fs;
    if (!filesystems.isEmpty())
        when.append(QStringLiteral("filesystem:") + filesystems.join(QLatin1Char('|')));
    if (!when.isEmpty())
        a.insert(QStringLiteral("when"), when);
    QJsonArray command;
    for (int r = 0; r < m_command->rowCount(); ++r)
        command.append(m_command->item(r, 0) ? m_command->item(r, 0)->text() : QString());
    a.insert(QStringLiteral("command"), command);
    if (m_output->currentData().toString() != QLatin1String("none"))
        a.insert(QStringLiteral("output"), m_output->currentData().toString());
    if (m_lookOnly->isChecked())
        a.insert(QStringLiteral("look_only"), true);
    if (m_systemDisks->isChecked())
        a.insert(QStringLiteral("system_disks"), true);
    if (!m_confirm->text().trimmed().isEmpty())
        a.insert(QStringLiteral("confirm"), m_confirm->text().trimmed());
    const QJsonArray ask = m_ask->fields();
    if (!ask.isEmpty())
        a.insert(QStringLiteral("ask"), ask);
    m_actions[m_current] = a;
}

QJsonObject AddonMaker::manifest() const
{
    QJsonObject o{
        {QStringLiteral("id"), m_id->text().trimmed()},
        {QStringLiteral("name"), m_name->text().trimmed()},
        {QStringLiteral("version"), m_version->text().trimmed()},
    };
    if (!m_author->text().trimmed().isEmpty())
        o.insert(QStringLiteral("author"), m_author->text().trimmed());
    if (!m_description->text().trimmed().isEmpty())
        o.insert(QStringLiteral("description"), m_description->text().trimmed());
    const QJsonArray settings = m_settings->fields();
    if (!settings.isEmpty())
        o.insert(QStringLiteral("settings"), settings);
    QJsonArray actions;
    for (const QJsonObject &a : m_actions)
        actions.append(a);
    o.insert(QStringLiteral("actions"), actions);
    return o;
}

QByteArray AddonMaker::json() const
{
    return QJsonDocument(manifest()).toJson(QJsonDocument::Indented);
}

void AddonMaker::refresh()
{
    if (m_loading)
        return;
    m_systemDisks->setEnabled(m_lookOnly->isChecked());
    const Addon a = Addons::parseData(json(), QString());
    const QString danger = Theme::instance().html(Theme::Role::Danger);
    QString html;
    if (!a.error.isEmpty()) {
        html = QStringLiteral("<p style=\"color:%1\"><b>%2</b></p>").arg(danger, tr("Not ready yet: %1").arg(a.error).toHtmlEscaped());
    } else if (m_current >= 0 && m_current < a.actions.size()) {
        const AddonAction &act = a.actions[m_current];
        html = addonNotes(act, Addons::risks(act, a.settings));
        // There's no shell: these reach the program as plain text.
        static const QStringList shellBits = {QStringLiteral("|"), QStringLiteral("||"), QStringLiteral(">"), QStringLiteral(">>"),
                                              QStringLiteral("<"), QStringLiteral("&&"), QStringLiteral(";"), QStringLiteral("&"), QStringLiteral("2>")};
        for (const QString &part : act.command) {
            if (shellBits.contains(part)) {
                html += QStringLiteral("<p>%1</p>").arg(tr("There's no shell, so \"%1\" reaches the program as plain text. For pipes and "
                                                           "redirections, tick Look-only and use: sh -c '…' sh {mountpoint}, or put a "
                                                           "script in your home folder.").arg(part).toHtmlEscaped());
                break;
            }
        }
        if (!m_originalId.isEmpty())
            html += QStringLiteral("<p><small>%1</small></p>").arg(tr("Saving changes the add-on, so each action asks once more before it runs.").toHtmlEscaped());
    }
    m_check->setText(html);
    m_check->setVisible(!html.isEmpty());
    m_saveButton->setEnabled(a.error.isEmpty());
    m_testButton->setEnabled(a.error.isEmpty() && m_tester != nullptr);
}

void AddonMaker::startFrom(int index)
{
    if (index <= 0)
        return;
    if (!askPlain(this, windowTitle(), tr("Start over from \"%1\"? What's in the form now is replaced.").arg(m_template->itemText(index)))) {
        m_template->setCurrentIndex(0);
        return;
    }
    QJsonObject t = templateFor(index);
    t.insert(QStringLiteral("author"), m_author->text());
    m_idEdited = false;
    loadManifest(t);
    m_id->setText(slug(m_name->text()));
    m_idEdited = false;
    m_template->setCurrentIndex(0);
}

void AddonMaker::insertPlaceholder(const QString &text)
{
    int r = m_command->currentRow();
    if (r < 0) {
        r = m_command->rowCount();
        m_command->insertRow(r);
        m_command->setItem(r, 0, new QTableWidgetItem);
    }
    QTableWidgetItem *item = m_command->item(r, 0);
    if (!item) {
        item = new QTableWidgetItem;
        m_command->setItem(r, 0, item);
    }
    item->setText(item->text() + text);
    m_command->setCurrentCell(r, 0);
}

void AddonMaker::save()
{
    storeAction();
    const QByteArray data = json();
    const Addon a = Addons::parseData(data, QString());
    if (!a.error.isEmpty())
        return;
    for (const Addon &existing : m_addons->all()) {
        if (existing.id == a.id && a.id != m_originalId
            && !askPlain(this, windowTitle(), tr("There's already an add-on called \"%1\". Replace it?").arg(existing.name)))
            return;
    }
    QString error;
    // An add-on that turned up from outside keeps that mark until the user says it's theirs.
    if (!Addons::install(data, &error, !m_keepOutsideFlag)) {
        warnPlain(this, windowTitle(), error);
        return;
    }
    if (m_keepOutsideFlag)
        warnPlain(this, windowTitle(), tr("Saved. It's still marked as added outside DiskForge until you press I Added It in the Add-ons window."));
    m_addons->load();
    accept();
}

void AddonMaker::saveCopy()
{
    storeAction();
    const QString file = QFileDialog::getSaveFileName(this, tr("Save a Copy"), QDir::homePath() + QStringLiteral("/addon.json"),
                                                      tr("Add-on (*.json)"));
    if (file.isEmpty())
        return;
    QSaveFile f(file);
    const QByteArray data = json();
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        warnPlain(this, windowTitle(), tr("Couldn't save %1").arg(file));
}
