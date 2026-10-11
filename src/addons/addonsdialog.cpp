// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "addonsdialog.h"

#include "addonform.h"
#include "addonprompt.h"
#include "addons.h"
#include "catalogdialog.h"
#include "dialogs.h"
#include "format.h"
#include "theme.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

AddonsDialog::AddonsDialog(Addons *addons, const QList<QKeySequence> &takenShortcuts, AddonMaker::Tester tester, QWidget *parent)
    : QDialog(parent)
    , m_addons(addons)
    , m_list(new QTreeWidget)
    , m_details(new QTextBrowser)
    , m_accept(new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok")), tr("I Added It")))
    , m_settings(new QPushButton(QIcon::fromTheme(QStringLiteral("configure")), tr("Settings…")))
    , m_edit(new QPushButton(QIcon::fromTheme(QStringLiteral("document-edit")), tr("Edit…")))
    , m_tester(std::move(tester))
    , m_actions(new QTableWidget)
    , m_taken(takenShortcuts)
{
    m_actions->setColumnCount(3);
    m_actions->setHorizontalHeaderLabels({tr("Action"), tr("On toolbar"), tr("Shortcut")});
    m_actions->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_actions->verticalHeader()->hide();
    m_actions->setSelectionMode(QAbstractItemView::NoSelection);
    connect(m_actions, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (!m_filling && item->column() == 1)
            Addons::setPinned(item->data(Qt::UserRole).toString(), item->checkState() == Qt::Checked);
    });
    connect(m_settings, &QPushButton::clicked, this, [this] {
        const int row = currentRow();
        if (row < 0)
            return;
        const Addon a = m_addons->all()[row];
        QMap<QString, QString> values;
        for (const AddonField &f : a.settings)
            values.insert(f.id, Addons::setting(a, f.id));
        AddonFormDialog form(tr("Settings for %1").arg(a.name), QString(), a.settings, values, tr("Save"), this);
        if (form.exec() != QDialog::Accepted)
            return;
        const QMap<QString, QString> answers = form.values();
        for (auto it = answers.constBegin(); it != answers.constEnd(); ++it) {
            if (hasHiddenCharacters(it.value())) {
                warnPlain(this, windowTitle(), tr("A value has hidden characters in it, so the settings weren't saved."));
                return;
            }
        }
        for (auto it = answers.constBegin(); it != answers.constEnd(); ++it)
            Addons::setSetting(a, it.key(), it.value());
    });
    setWindowTitle(tr("Add-ons"));
    m_list->setHeaderLabels({tr("Add-on"), tr("Version"), tr("Author"), tr("Status")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_details->setOpenExternalLinks(true);

    auto *catalog = new QPushButton(QIcon::fromTheme(QStringLiteral("get-hot-new-stuff"), QIcon::fromTheme(QStringLiteral("download"))), tr("Get Add-ons…"));
    auto *install = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Install from File…"));
    auto *remove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"));
    auto *folder = new QPushButton(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Open Folder"));
    auto *guide = new QPushButton(QIcon::fromTheme(QStringLiteral("help-contents")), tr("How It Works"));
    auto *make = new QPushButton(QIcon::fromTheme(QStringLiteral("document-new")), tr("Make an Add-on…"));
    connect(make, &QPushButton::clicked, this, [this] {
        AddonMaker(m_addons, nullptr, m_tester, this).exec();
        m_addons->load();
        fill();
    });
    connect(m_edit, &QPushButton::clicked, this, [this] {
        const int row = currentRow();
        if (row < 0)
            return;
        const Addon a = m_addons->all()[row];
        AddonMaker(m_addons, &a, m_tester, this).exec();
        m_addons->load();
        fill();
    });
    auto *close = new QPushButton(tr("Close"));

    connect(install, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Install Add-on"), QDir::homePath(), tr("Add-on manifest (addon.json *.json)"));
        if (file.isEmpty())
            return;
        // Read once, so what the question shows is exactly what gets installed.
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) {
            warnPlain(this, windowTitle(), tr("Couldn't open %1: %2").arg(file, f.errorString()));
            return;
        }
        const QByteArray data = f.read(Addons::kMaxDownload * 4);
        const Addon a = Addons::parseData(data, file);
        if (!a.error.isEmpty()) {
            warnPlain(this, windowTitle(), tr("This add-on is broken: %1").arg(a.error));
            return;
        }
        if (!askInstallAddon(this, a))
            return;
        QString error;
        if (!Addons::install(data, &error))
            warnPlain(this, windowTitle(), error);
        m_addons->load();
        fill();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = currentRow();
        if (row < 0)
            return;
        const Addon a = m_addons->all()[row];
        if (!askPlain(this, windowTitle(), tr("Remove \"%1\"?").arg(a.name)))
            return;
        QString error;
        if (!Addons::remove(a, &error))
            warnPlain(this, windowTitle(), error);
        m_addons->load();
        fill();
    });
    m_accept->setToolTip(tr("This add-on is yours: stop warning that it was added from outside DiskForge"));
    connect(m_accept, &QPushButton::clicked, this, [this] {
        const int row = currentRow();
        if (row < 0)
            return;
        const Addon a = m_addons->all()[row];
        if (!askPlain(this, windowTitle(), tr("Did you put \"%1\" in the add-on folder (or change it) yourself?\n\n"
                                               "Only say yes if you did. DiskForge stops warning about this version of it.").arg(a.name)))
            return;
        m_addons->accept(a.id, a.fileHash);
        fill();
        m_list->setCurrentItem(m_list->topLevelItem(row));
    });
    connect(folder, &QPushButton::clicked, this, [] {
        QDir().mkpath(Addons::userDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(Addons::userDir()));
    });
    connect(guide, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral(APP_HOMEPAGE "/blob/main/docs/ADDONS.md")));
    });
    connect(catalog, &QPushButton::clicked, this, [this] {
        CatalogDialog(m_addons, this).exec();
        m_addons->load();
        fill();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_list, &QTreeWidget::currentItemChanged, this, &AddonsDialog::showDetails);
    connect(m_list, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
        if (m_filling)
            return;
        const int row = m_list->indexOfTopLevelItem(item);
        if (row >= 0 && row < m_addons->all().size())
            m_addons->setEnabled(m_addons->all()[row].id, item->checkState(0) == Qt::Checked);
    });

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(make);
    buttons->addWidget(m_edit);
    buttons->addWidget(catalog);
    buttons->addWidget(install);
    buttons->addWidget(remove);
    buttons->addWidget(m_accept);
    buttons->addWidget(m_settings);
    buttons->addWidget(folder);
    buttons->addWidget(guide);
    buttons->addStretch();
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_list, 2);
    layout->addWidget(m_details, 3);
    auto *actionsLabel = new QLabel(tr("Its actions (pin one to the toolbar, or give it a shortcut):"));
    layout->addWidget(actionsLabel);
    layout->addWidget(m_actions, 2);
    layout->addLayout(buttons);
    fill();
    resize(780, 640);
}

void AddonsDialog::fill()
{
    m_filling = true;
    m_list->clear();
    m_accept->setEnabled(false);
    m_settings->setEnabled(false);
    m_edit->setEnabled(false);
    m_actions->setRowCount(0);
    for (const Addon &a : m_addons->all()) {
        const QString status = !a.error.isEmpty() ? tr("Broken") : a.outside ? tr("Added outside") : a.enabled ? tr("On") : tr("Off");
        auto *item = new QTreeWidgetItem(m_list, {a.name.isEmpty() ? a.file : a.name, a.version, a.author, status});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, a.enabled ? Qt::Checked : Qt::Unchecked);
        if (!a.error.isEmpty())
            item->setForeground(3, Theme::instance().color(Theme::Role::Danger));
        else if (a.outside)
            item->setForeground(3, Theme::instance().color(Theme::Role::Warning));
    }
    m_filling = false;
    if (m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));
    else
        m_details->setHtml(tr("<p>No add-ons installed.</p><p>Add-ons add actions to DiskForge's menus, like backing up a "
                              "drive or opening it in another tool. Make one with <b>Make an Add-on</b>, get one with "
                              "<b>Get Add-ons</b>, or install a file with <b>Install from File</b>.</p><p>They live in <code>%1</code>.</p>")
                               .arg(Addons::userDir().toHtmlEscaped()));
}

int AddonsDialog::currentRow() const
{
    const int row = m_list->indexOfTopLevelItem(m_list->currentItem());
    return row >= 0 && row < m_addons->all().size() ? row : -1;
}

void AddonsDialog::showDetails()
{
    const int row = currentRow();
    m_accept->setEnabled(row >= 0 && m_addons->all()[row].outside && m_addons->all()[row].error.isEmpty());
    m_settings->setEnabled(row >= 0 && !m_addons->all()[row].settings.isEmpty() && m_addons->all()[row].error.isEmpty());
    m_edit->setEnabled(row >= 0 && m_addons->all()[row].error.isEmpty());
    m_edit->setText(row >= 0 && m_addons->all()[row].systemWide ? tr("Edit a Copy…") : tr("Edit…"));
    showActions(row);
    if (row < 0)
        return;
    const Addon &a = m_addons->all()[row];
    QString html = QStringLiteral("<h3>%1</h3>").arg(a.name.toHtmlEscaped());
    if (!a.description.isEmpty())
        html += QStringLiteral("<p>%1</p>").arg(a.description.toHtmlEscaped());
    if (!a.error.isEmpty())
        html += QStringLiteral("<p style=\"color:%1\"><b>%2</b></p>")
                    .arg(Theme::instance().html(Theme::Role::Danger), tr("Broken: %1").arg(a.error).toHtmlEscaped());
    if (a.outside && a.error.isEmpty())
        html += outsideNote();
    for (const AddonAction &act : a.actions) {
        html += QStringLiteral("<p><b>%1</b> <small>(%2%3)</small><br><code>%4</code></p>")
                    .arg(act.label.toHtmlEscaped(), act.appliesTo.toHtmlEscaped(), act.terminal ? tr(", in a terminal") : act.window ? tr(", in a DiskForge window") : QString(),
                         act.command.join(QLatin1Char(' ')).toHtmlEscaped())
            + addonNotes(act);
    }
    html += QStringLiteral("<p><small>%1</small></p>").arg(a.file.toHtmlEscaped());
    m_details->setHtml(html);
}

void AddonsDialog::showActions(int row)
{
    m_filling = true;
    m_actions->setRowCount(0);
    if (row >= 0 && m_addons->all()[row].error.isEmpty()) {
        const Addon &a = m_addons->all()[row];
        for (const AddonAction &act : a.actions) {
            const QString key = Addons::actionKey(a, act);
            const int r = m_actions->rowCount();
            m_actions->insertRow(r);
            auto *label = new QTableWidgetItem(act.label);
            label->setFlags(Qt::ItemIsEnabled);
            m_actions->setItem(r, 0, label);
            auto *pin = new QTableWidgetItem;
            pin->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            pin->setCheckState(Addons::isPinned(key) ? Qt::Checked : Qt::Unchecked);
            pin->setData(Qt::UserRole, key);
            m_actions->setItem(r, 1, pin);
            auto *keys = new QKeySequenceEdit(QKeySequence(Addons::shortcut(key), QKeySequence::PortableText));
            keys->setMaximumSequenceLength(1);
            keys->setClearButtonEnabled(true);
            connect(keys, &QKeySequenceEdit::editingFinished, this, [this, keys, key] {
                const QString problem = shortcutProblem(keys->keySequence(), key);
                if (!problem.isEmpty()) {
                    warnPlain(this, windowTitle(), problem);
                    keys->setKeySequence(QKeySequence(Addons::shortcut(key), QKeySequence::PortableText));
                    return;
                }
                Addons::setShortcut(key, keys->keySequence().toString(QKeySequence::PortableText));
            });
            connect(keys, &QKeySequenceEdit::keySequenceChanged, this, [key](const QKeySequence &seq) {
                if (seq.isEmpty()) // the clear button
                    Addons::setShortcut(key, QString());
            });
            m_actions->setCellWidget(r, 2, keys);
        }
    }
    m_actions->resizeColumnToContents(1);
    m_actions->setColumnWidth(2, 180);
    m_filling = false;
}

QString AddonsDialog::shortcutProblem(const QKeySequence &keys, const QString &actionKey) const
{
    if (keys.isEmpty())
        return {};
    const QKeyCombination combo = keys[0];
    const int key = int(combo.key());
    const bool fKey = key >= Qt::Key_F1 && key <= Qt::Key_F35;
    if (!(combo.keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && !fKey)
        return tr("Use a key together with Ctrl, Alt or Meta (or an F key), so it can't go off while you type.");
    const QString shown = keys.toString(QKeySequence::NativeText);
    for (const QKeySequence &taken : m_taken) {
        if (taken.matches(keys) == QKeySequence::ExactMatch)
            return tr("%1 is already one of DiskForge's own shortcuts.").arg(shown);
    }
    for (const Addon &a : m_addons->all()) {
        for (const AddonAction &act : a.actions) {
            const QString other = Addons::actionKey(a, act);
            if (other != actionKey && QKeySequence(Addons::shortcut(other), QKeySequence::PortableText) == keys)
                return tr("%1 is already the shortcut for \"%2\".").arg(shown, act.label);
        }
    }
    return {};
}
