// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "productkeydialog.h"

#include "applog.h"
#include "dialogs.h"
#include "hostprocess.h"
#include "productkey.h"
#include "rescuestick.h"
#include "windowspicker.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace {

QPushButton *copyButton(const QString &key)
{
    auto *copy = new QPushButton(QObject::tr("Copy"));
    QObject::connect(copy, &QPushButton::clicked, copy, [key] { QApplication::clipboard()->setText(key); });
    return copy;
}

QString genericNote()
{
    return ProductKeyDialog::tr("This is Microsoft's generic key for the edition, not a licence of its own: this Windows is activated "
                                "by a digital licence tied to the PC (or to a Microsoft account). Reinstall the same edition and it "
                                "activates by itself.");
}

} // namespace

ProductKeyDialog::ProductKeyDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_firmware(new QLabel)
    , m_installs(new QGridLayout)
{
    setWindowTitle(tr("Windows Product Key"));
    m_firmware->setWordWrap(true);
    m_firmware->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_firmware->setObjectName(QStringLiteral("firmwareKey"));

    auto *firmwareBox = new QGroupBox(tr("In this PC's firmware"));
    auto *firmwareRow = new QHBoxLayout(firmwareBox);
    firmwareRow->addWidget(m_firmware, 1);

    auto *installBox = new QGroupBox(tr("Windows installs here"));
    auto *installLayout = new QVBoxLayout(installBox);
    installLayout->addLayout(m_installs);
    auto *elsewhere = new QPushButton(tr("Look in Another Windows…"));
    connect(elsewhere, &QPushButton::clicked, this, &ProductKeyDialog::lookElsewhere);
    installLayout->addWidget(elsewhere, 0, Qt::AlignLeft);

    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("The keys to have at hand before reinstalling Windows. A PC that came with Windows 8 or later "
                                       "keeps its key in its firmware, and Windows' setup finds it there by itself.")));
    layout->addWidget(firmwareBox);
    layout->addWidget(installBox, 1);
    layout->addWidget(close, 0, Qt::AlignRight);

    readFirmware(false);
#ifdef Q_OS_WIN
    const productkey::Installed self = productkey::running();
    addWindows(tr("This PC's own Windows"), self.edition, self.key, self.problem);
#endif
    for (const QString &root : WindowsPicker::mounted(m_udisks)) {
        const productkey::Installed found = productkey::fromHive(root + QStringLiteral("/Windows/System32/config/SOFTWARE"));
        addWindows(tr("Windows on %1").arg(QDir::toNativeSeparators(root)), found.edition, found.key, found.problem);
        m_shown << root;
    }
    if (m_installs->count() == 0) {
        m_none = new QLabel(tr("None mounted: Look in Another Windows finds one on a drive."));
        m_installs->addWidget(m_none, 0, 0);
    }
    resize(620, sizeHint().height());
}

void ProductKeyDialog::showFirmware(const QString &key, const QString &note)
{
    delete m_firmwareCopy;
    m_firmwareCopy = nullptr;
    m_firmware->setText(key.isEmpty() ? note : (note.isEmpty() ? key : key + QLatin1Char('\n') + note));
    if (!key.isEmpty()) {
        m_firmwareCopy = copyButton(key);
        static_cast<QHBoxLayout *>(m_firmware->parentWidget()->layout())->addWidget(m_firmwareCopy, 0, Qt::AlignTop);
    }
}

void ProductKeyDialog::readFirmware(bool askForRights)
{
#ifdef Q_OS_WIN
    Q_UNUSED(askForRights)
    const QString key = productkey::firmwareKey();
    showFirmware(key, key.isEmpty() ? tr("None: this PC didn't come with Windows 8 or later, or its maker didn't put the key in its "
                                         "firmware.")
                                    : QString());
#else
    const QString path = QStringLiteral("/sys/firmware/acpi/tables/MSDM");
    if (!QFile::exists(path)) {
        showFirmware({}, tr("None: this PC didn't come with Windows 8 or later, or its maker didn't put the key in its firmware."));
        return;
    }
    QFile table(path);
    QByteArray data;
    if (table.open(QIODevice::ReadOnly)) {
        data = table.readAll();
    } else if (askForRights) {
        // Only root reads the firmware's tables: DiskForge Live's sudo, or the admin password.
        const QStringList command = rescue::runningInRescue() ? QStringList{QStringLiteral("sudo"), QStringLiteral("-n"), QStringLiteral("cat"), path}
                                                              : host::command({QStringLiteral("pkexec"), QStringLiteral("cat"), path});
        QProcess cat;
        cat.start(command.first(), command.mid(1));
        if (cat.waitForFinished(120000) && cat.exitCode() == 0)
            data = cat.readAllStandardOutput();
    }
    if (data.isEmpty()) {
        showFirmware({}, tr("This PC's firmware has a key; reading it takes admin rights."));
        if (!m_firmwareShow) {
            auto *show = new QPushButton(tr("Show It"));
            show->setObjectName(QStringLiteral("showFirmwareKey"));
            connect(show, &QPushButton::clicked, this, [this] { readFirmware(true); });
            static_cast<QHBoxLayout *>(m_firmware->parentWidget()->layout())->addWidget(show, 0, Qt::AlignTop);
            m_firmwareShow = show;
        }
        return;
    }
    delete m_firmwareShow;
    m_firmwareShow = nullptr;
    const QString key = productkey::fromMsdm(data);
    showFirmware(key, key.isEmpty() ? tr("Its firmware has a key table, but no key could be read from it.") : QString());
#endif
}

void ProductKeyDialog::addWindows(const QString &title, const QString &edition, const QString &key, const QString &problem)
{
    delete m_none;
    m_none = nullptr;
    const int row = m_installs->rowCount();
    auto *name = new QLabel(QStringLiteral("<b>%1</b>%2").arg(title.toHtmlEscaped(), edition.isEmpty() ? QString() : QStringLiteral("<br>") + edition.toHtmlEscaped()));
    QString text = key.isEmpty() ? tr("No key: %1").arg(problem) : key;
    if (productkey::isGeneric(key))
        text += QLatin1Char('\n') + genericNote();
    auto *value = new QLabel(text);
    value->setObjectName(QStringLiteral("installedKey"));
    value->setWordWrap(true);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_installs->addWidget(name, row, 0, Qt::AlignTop);
    m_installs->addWidget(value, row, 1);
    if (!key.isEmpty())
        m_installs->addWidget(copyButton(key), row, 2, Qt::AlignTop);
    m_installs->setColumnStretch(1, 1);
}

void ProductKeyDialog::lookElsewhere()
{
    WindowsPicker picker(m_udisks, false, tr("Which Windows' key? A Windows on another drive is mounted read-only to read it."), this);
    if (picker.exec() != QDialog::Accepted)
        return;
#ifdef Q_OS_WIN
    if (picker.running())
        return; // listed already
#endif
    const QString root = picker.root();
    if (m_shown.contains(root))
        return;
    m_shown << root;
    const productkey::Installed found = productkey::fromHive(root + QStringLiteral("/Windows/System32/config/SOFTWARE"));
    addWindows(tr("Windows on %1").arg(QDir::toNativeSeparators(root)), found.edition, found.key, found.problem);
}
