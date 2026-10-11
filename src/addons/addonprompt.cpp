// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "addonprompt.h"

#include "dialogs.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace {

QLabel *plainLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

// Commands go in a plain text box: nothing in them can turn into formatting there.
QPlainTextEdit *commandBox(const QString &text, int lines)
{
    auto *box = new QPlainTextEdit(text);
    box->setReadOnly(true);
    box->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    box->setLineWrapMode(QPlainTextEdit::NoWrap);
    box->setFixedHeight(box->fontMetrics().lineSpacing() * qBound(2, lines, 10) + 2 * box->frameWidth() + 12);
    return box;
}

// One part of a command, quoted if it's empty or has spaces or quotes in it.
QString shown(const QString &part)
{
    static const QRegularExpression special(QStringLiteral("[\\s'\"\\\\]"));
    if (!part.isEmpty() && !part.contains(special))
        return part;
    QString quoted = part;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QString byAuthor(const Addon &addon)
{
    return addon.author.isEmpty() ? QString() : QObject::tr(" by %1").arg(addon.author);
}

QString note(const QString &color, const QString &text)
{
    if (color.isEmpty())
        return QStringLiteral("<p>%1</p>").arg(text.toHtmlEscaped());
    return QStringLiteral("<p style=\"color:%1\"><b>%2</b></p>").arg(color, text.toHtmlEscaped());
}

const QString kRed = QStringLiteral("#e05050");
const QString kOrange = QStringLiteral("#e08a1e");

} // namespace

QString addonNotes(const AddonAction &action)
{
    return addonNotes(action, Addons::risks(action));
}

QString addonNotes(const AddonAction &action, const AddonRisks &r)
{
    QString html;
    if (action.lookOnly) {
        html += note({}, QObject::tr("Look-only: runs in a read-only sandbox. It can look at your files and drives, but can't "
                                     "change anything, use the network or reach other programs."));
    } else {
        if (!r.admin.isEmpty())
            html += note(kRed, QObject::tr("Runs as admin (root) through %1: it can change anything on this PC, including the "
                                           "drive your system runs from.").arg(r.admin));
        if (!r.anything.isEmpty())
            html += note(kOrange, QObject::tr("Uses %1, which runs other commands: it can do anything you can.").arg(r.anything));
        if (!r.network.isEmpty())
            html += note({}, QObject::tr("Uses the network (%1): it can send things off this PC or download things.").arg(r.network));
        if (!r.deletes.isEmpty())
            html += note({}, QObject::tr("Can delete or overwrite files (%1).").arg(r.deletes));
        if (action.systemDisks)
            html += note({}, QObject::tr("Not offered on the system disk: only look-only actions are."));
    }
    return html;
}

QString outsideNote()
{
    return note(kOrange, QObject::tr("This add-on was put in your add-on folder, or changed, without going through DiskForge. "
                                     "If you didn't do that yourself, don't run it: remove it in Tools → Add-ons."));
}

bool askInstallAddon(QWidget *parent, const Addon &addon)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Install Add-on?"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(plainLabel(QObject::tr("Install \"%1\"%2? It adds these actions:").arg(addon.name, byAuthor(addon))));

    QStringList lines;
    QString notes;
    for (const AddonAction &act : addon.actions) {
        QStringList parts;
        for (const QString &part : act.command)
            parts << shown(part);
        if (!lines.isEmpty())
            lines << QString();
        lines << act.label + (act.terminal ? QObject::tr("  (in a terminal)") : act.window ? QObject::tr("  (in a DiskForge window)") : QString());
        lines << QStringLiteral("  ") + parts.join(QLatin1Char(' '));
        const QString actionNotes = addonNotes(act);
        if (!actionNotes.isEmpty())
            notes += QStringLiteral("<p><b>%1</b></p>").arg(act.label.toHtmlEscaped()) + actionNotes;
    }
    layout->addWidget(commandBox(lines.join(QLatin1Char('\n')), int(lines.size())));
    if (!notes.isEmpty())
        layout->addWidget(wrappingLabel(notes));
    layout->addWidget(plainLabel(QObject::tr("Only install add-ons you trust. Each action still asks before it first runs.")));
    QPushButton *install = nullptr;
    layout->addWidget(dialogButtons(&dialog, QObject::tr("Install"), &install));
    dialog.setMinimumWidth(560);
    return dialog.exec() == QDialog::Accepted;
}

bool askRunAddon(QWidget *parent, const Addon &addon, const AddonAction &action, const QStringList &argv, bool *remember)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Run Add-on?"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(plainLabel(QObject::tr("\"%1\" from the add-on \"%2\"%3 runs this command, one part per line:")
                                     .arg(action.label, addon.name, byAuthor(addon))));
    QStringList parts;
    for (const QString &part : argv)
        parts << shown(part);
    layout->addWidget(commandBox(parts.join(QLatin1Char('\n')), int(parts.size())));

    // What this run will really do, with the form's answers in.
    const AddonRisks risks = action.lookOnly ? AddonRisks() : Addons::risksOf(argv);
    const QString notes = (addon.outside ? outsideNote() : QString()) + addonNotes(action, risks);
    if (!notes.isEmpty())
        layout->addWidget(wrappingLabel(notes));
    layout->addWidget(plainLabel(risks.admin.isEmpty() ? QObject::tr("Only run add-ons you trust. It runs as you, not as root.")
                                                       : QObject::tr("Only run add-ons you trust.")));
    QCheckBox *again = nullptr;
    if (!remember) {
        // A test run from the Add-on Maker: nothing is remembered.
    } else if (risks.alwaysAsk() || Addons::risks(action, addon.settings).alwaysAsk()) {
        layout->addWidget(plainLabel(QObject::tr("Actions with admin power or that run other commands ask every time.")));
    } else {
        again = new QCheckBox(QObject::tr("Don't ask again for this action"));
        layout->addWidget(again);
    }
    QPushButton *run = nullptr;
    layout->addWidget(dialogButtons(&dialog, QObject::tr("Run"), &run));
    dialog.setMinimumWidth(560);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    if (remember)
        *remember = again && again->isChecked();
    return true;
}
