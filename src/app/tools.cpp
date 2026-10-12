// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tools.h"

#include "applog.h"
#include "blockio.h"
#include "blockmapwidget.h"
#include "jobui.h"

#include "benchmark.h"
#include "dialogs.h"
#include "checksums.h"
#include "filecopy.h"
#include "format.h"
#include "imagesource.h"
#include "isocopy.h"
#include "imagewriter.h"
#include "rescuestick.h"
#include "sdcard.h"
#include "surfacescan.h"
#include "drivefacts.h"
#include "farm.h"
#include "health.h"
#include "hostprocess.h"
#include "systemd.h"
#include "btrfscheck.h"
#include "theme.h"
#include "windowsusbdialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QThread>
#include <QProcess>
#include <QSpinBox>
#include <QStandardPaths>
#include <QGroupBox>
#include <QLocale>
#include <QDateTime>
#include <QTextDocument>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QString healthColor(Health::State s)
{
    const Theme &theme = Theme::instance();
    switch (s) {
    case Health::State::Healthy: return theme.html(Theme::Role::Good);
    case Health::State::Warning: return theme.html(Theme::Role::Warning);
    case Health::State::Failing: return theme.html(Theme::Role::Danger);
    case Health::State::Unknown: break;
    }
    return theme.html(Theme::Role::Muted);
}

// The first firmware updater app that's installed, as a command line. None in a Flatpak:
// those would have to be started on the host.
QStringList firmwareUpdater()
{
    if (QFileInfo::exists(QStringLiteral("/.flatpak-info")))
        return {};
    const QList<QStringList> apps = {{QStringLiteral("plasma-discover"), QStringLiteral("--mode"), QStringLiteral("update")},
                                     {QStringLiteral("gnome-firmware")},
                                     {QStringLiteral("gnome-software"), QStringLiteral("--mode"), QStringLiteral("updates")}};
    for (const QStringList &app : apps) {
        if (!QStandardPaths::findExecutable(app.first()).isEmpty())
            return app;
    }
    return {};
}

QString selftestText(const Health &h)
{
    const QString &s = h.selftestStatus;
    if (s.isEmpty())
        return QObject::tr("Never run");
    if (s == QLatin1String("success"))
        return QObject::tr("Last one passed");
    if (s == QLatin1String("inprogress"))
        return h.selftestPercentRemaining >= 0 ? QObject::tr("Running, %1% left").arg(h.selftestPercentRemaining)
                                               : QObject::tr("Running");
    if (s.startsWith(QLatin1String("abort")) || s == QLatin1String("interrupted"))
        return QObject::tr("Last one was stopped before it finished");
    return QObject::tr("Last one failed (%1)").arg(s);
}

QString sectors(qint64 n)
{
    return QObject::tr("%n sector(s)", nullptr, int(n));
}

} // namespace

// --- Encryption fields --------------------------------------------------------

EncryptionFields::EncryptionFields(QWidget *parent)
    : QWidget(parent)
    , m_box(new QCheckBox(tr("Encrypt with a passphrase (LUKS2)")))
    , m_fields(new QWidget)
    , m_pass(new QLineEdit)
    , m_confirm(new QLineEdit)
    , m_note(new QLabel)
{
    m_pass->setEchoMode(QLineEdit::Password);
    m_confirm->setEchoMode(QLineEdit::Password);
    m_pass->setPlaceholderText(tr("Passphrase"));
    m_confirm->setPlaceholderText(tr("Type it again"));
    m_note->setWordWrap(true);

    auto *fields = new QVBoxLayout(m_fields);
    fields->setContentsMargins(0, 0, 0, 0);
    fields->addWidget(m_pass);
    fields->addWidget(m_confirm);
    fields->addWidget(m_note);
    m_fields->setVisible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_box);
    layout->addWidget(m_fields);
#ifdef Q_OS_WIN
    setVisible(false); // LUKS is Linux's; Windows can't make it (BitLocker is set up in Windows itself)
#endif

    auto update = [this] {
        const bool mismatch = !m_confirm->text().isEmpty() && m_pass->text() != m_confirm->text();
        m_note->setText(mismatch ? redText(tr("The passphrases don't match."))
                                 : tr("If you forget it, nothing on the drive can be recovered."));
        emit changed();
    };
    connect(m_box, &QCheckBox::toggled, this, [this, update](bool on) {
        m_fields->setVisible(on);
        update();
        // Grow the dialog to fit, keeping its width (adjustSize would shrink it).
        QWidget *w = window();
        w->layout()->activate();
        const int h = w->layout()->hasHeightForWidth() ? w->layout()->totalHeightForWidth(w->width()) : w->sizeHint().height();
        w->resize(w->width(), std::max(h, w->minimumSizeHint().height()));
    });
    connect(m_pass, &QLineEdit::textChanged, this, update);
    connect(m_confirm, &QLineEdit::textChanged, this, update);
    update();
}

QString EncryptionFields::passphrase() const
{
    return m_box->isChecked() ? m_pass->text() : QString();
}

bool EncryptionFields::isValid() const
{
    return !m_box->isChecked() || (!m_pass->text().isEmpty() && m_pass->text() == m_confirm->text());
}

// --- Change passphrase --------------------------------------------------------

ChangePassphraseDialog::ChangePassphraseDialog(const QString &device, QWidget *parent)
    : QDialog(parent)
    , m_old(new QLineEdit)
    , m_new(new QLineEdit)
    , m_confirm(new QLineEdit)
{
    for (QLineEdit *e : {m_old, m_new, m_confirm})
        e->setEchoMode(QLineEdit::Password);
    auto *form = new QFormLayout;
#ifdef Q_OS_WIN
    // BitLocker: Windows doesn't ask for the old password while the drive is unlocked, and wants
    // at least 8 characters.
    setWindowTitle(tr("Change BitLocker Password of %1").arg(device));
    form->addRow(tr("New password:"), m_new);
    form->addRow(tr("New password again:"), m_confirm);
    constexpr int least = 8;
#else
    setWindowTitle(tr("Change Passphrase of %1").arg(device));
    form->addRow(tr("Current passphrase:"), m_old);
    form->addRow(tr("New passphrase:"), m_new);
    form->addRow(tr("New passphrase again:"), m_confirm);
    constexpr int least = 1;
#endif

    QPushButton *ok;
    QDialogButtonBox *box = dialogButtons(this, tr("Change"), &ok);
    auto update = [this, ok, least] {
#ifdef Q_OS_WIN
        const bool old = true;
#else
        const bool old = !m_old->text().isEmpty();
#endif
        ok->setEnabled(old && m_new->text().size() >= least && m_new->text() == m_confirm->text());
    };
    for (QLineEdit *e : {m_old, m_new, m_confirm})
        connect(e, &QLineEdit::textChanged, this, update);
    update();

    setMinimumWidth(440);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
#ifdef Q_OS_WIN
    m_old->hide();
    layout->addWidget(wrappingLabel(tr("<small>At least 8 characters. While the drive is unlocked, Windows doesn't need the old one. "
                                       "Its recovery key keeps opening it either way.</small>")));
#endif
    layout->addWidget(box);
}

QString ChangePassphraseDialog::oldPassphrase() const { return m_old->text(); }
QString ChangePassphraseDialog::newPassphrase() const { return m_new->text(); }

// --- Wipe ---------------------------------------------------------------------

WipeDialog::WipeDialog(const Disk &disk, int diskNumber, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Wipe Disk %1").arg(diskNumber));
    const bool slow = disk.rotationRate > 0 || disk.bus == QLatin1String("usb") || disk.removable;
    // Measured: a 5400 rpm laptop drive zero-fills at about 50 MB/s.
    const double seconds = double(disk.size) / (slow ? 50e6 : 400e6);

    QStringList lost;
    for (const Volume &v : disk.volumes) {
        if (!v.isContainer)
            lost << QStringLiteral("• ") + describeVolume(v).toHtmlEscaped();
    }
    QString text = tr("<p><b>Disk %1: %2</b></p>"
                      "<p>Every byte on the disk is overwritten with zeros, so nothing on it can be recovered. "
                      "Afterwards it has no partitions; use New Partition Table to use it again.</p>"
                      "<p>This takes %3.</p>")
                       .arg(diskNumber).arg(diskTitle(disk).toHtmlEscaped(), durationText(seconds));
    if (!lost.isEmpty())
        text += redText(tr("This erases:")) + QStringLiteral("<br>") + lost.join(QStringLiteral("<br>"));
    if (!diskWarning(disk).isEmpty())
        text += QStringLiteral("<br>") + redText(diskWarning(disk));

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(text));
    const QString name = shortDevice(disk.device);
    layout->addWidget(new QLabel(tr("Type <b>%1</b> to confirm:").arg(name)));
    auto *confirm = new QLineEdit;
    confirm->setPlaceholderText(name);
    layout->addWidget(confirm);
    QPushButton *ok;
    layout->addWidget(dialogButtons(this, tr("Wipe Disk"), &ok));
    ok->setEnabled(false);
    connect(confirm, &QLineEdit::textChanged, this, [ok, name](const QString &t) { ok->setEnabled(t.trimmed() == name); });
    resize(520, sizeHint().height());
}

// --- Health -------------------------------------------------------------------

// --- VeraCrypt -------------------------------------------------------------------

VeraCryptDialog::VeraCryptDialog(const QString &volumeName, QWidget *parent)
    : QDialog(parent)
    , m_password(new QLineEdit)
    , m_pim(new QSpinBox)
    , m_hidden(new QCheckBox(tr("A hidden volume inside it")))
    , m_system(new QCheckBox(tr("Windows' own drive, encrypted by VeraCrypt (system encryption)")))
    , m_keyfiles(new QListWidget)
{
    setWindowTitle(tr("Unlock %1 as VeraCrypt").arg(volumeName));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setObjectName(QStringLiteral("password"));
    m_pim->setRange(0, 2147468);
    m_pim->setSpecialValueText(tr("None (VeraCrypt's own)"));
    m_pim->setObjectName(QStringLiteral("pim"));
    m_pim->setToolTip(tr("Only if one was set when the volume was made"));
    m_keyfiles->setMaximumHeight(90);
    auto *add = new QPushButton(tr("Add Keyfile…"));
    auto *remove = new QPushButton(tr("Remove"));
    connect(add, &QPushButton::clicked, this, [this] {
        for (const QString &file : QFileDialog::getOpenFileNames(this, tr("Keyfiles"), QDir::homePath()))
            m_keyfiles->addItem(QDir::toNativeSeparators(file));
    });
    connect(remove, &QPushButton::clicked, this, [this] { delete m_keyfiles->currentItem(); });
    auto *keyButtons = new QVBoxLayout;
    keyButtons->addWidget(add);
    keyButtons->addWidget(remove);
    keyButtons->addStretch();
    auto *keys = new QHBoxLayout;
    keys->addWidget(m_keyfiles, 1);
    keys->addLayout(keyButtons);

    auto *form = new QFormLayout;
    form->addRow(tr("Password:"), m_password);
    form->addRow(tr("PIM:"), m_pim);
    form->addRow(tr("Keyfiles:"), keys);
    form->addRow(QString(), m_hidden);
    form->addRow(QString(), m_system);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("VeraCrypt volumes look like random data, so nothing on %1 says it is one: if it is, its password "
                                       "(and whatever else was set when it was made) opens it. TrueCrypt volumes open the same way.")
                                        .arg(volumeName.toHtmlEscaped())));
    layout->addLayout(form);
    QPushButton *ok;
    layout->addWidget(dialogButtons(this, tr("Unlock"), &ok));
    auto update = [this, ok] { ok->setEnabled(!m_password->text().isEmpty() || m_keyfiles->count() > 0); };
    connect(m_password, &QLineEdit::textChanged, this, update);
    connect(m_keyfiles->model(), &QAbstractItemModel::rowsInserted, this, update);
    connect(m_keyfiles->model(), &QAbstractItemModel::rowsRemoved, this, update);
    update();
    resize(520, sizeHint().height());
}

QString VeraCryptDialog::password() const
{
    return m_password->text();
}

QVariantMap VeraCryptDialog::options() const
{
    QVariantMap o;
    if (m_pim->value() > 0)
        o.insert(QStringLiteral("pim"), QVariant::fromValue(quint32(m_pim->value())));
    if (m_hidden->isChecked())
        o.insert(QStringLiteral("hidden"), true);
    if (m_system->isChecked())
        o.insert(QStringLiteral("system"), true);
    QStringList files;
    for (int i = 0; i < m_keyfiles->count(); ++i)
        files << QDir::fromNativeSeparators(m_keyfiles->item(i)->text());
    if (!files.isEmpty())
        o.insert(QStringLiteral("keyfiles"), files);
    return o;
}

// --- Sector size ----------------------------------------------------------------

SectorSizeDialog::SectorSizeDialog(UDisks *udisks, const Disk &disk, const drivepower::SectorSizes &sizes, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_disk(disk)
    , m_size(new QComboBox)
    , m_confirm(new QLineEdit)
    , m_change(new QPushButton(tr("Change")))
{
    setWindowTitle(tr("Sector Size of %1").arg(disk.model));
    for (int size : sizes.supported) {
        if (size != sizes.logical)
            m_size->addItem(tr("%1 bytes").arg(size), size);
    }
    m_size->setObjectName(QStringLiteral("sectorSize"));
    QString text = tr("<p><b>%1</b></p><p>It uses %2-byte sectors now (%3-byte inside). This drive can be switched to another "
                      "size: 4096 suits a drive that only holds data (a NAS, ZFS, a backup drive), and 512 is what older systems "
                      "and every Windows can start from.</p>")
                       .arg(diskTitle(disk).toHtmlEscaped())
                       .arg(sizes.logical)
                       .arg(sizes.physical);
    text += redText(tr("Everything on the drive is lost, and its partitions have to be made again. Many drives only change after "
                       "being switched off and on. Older PCs, and Windows 7 and earlier, can't start from a drive with 4096-byte sectors."));
    if (!diskWarning(disk).isEmpty())
        text += QStringLiteral("<br>") + redText(diskWarning(disk));
    const QString name = shortDevice(disk.device);
    m_confirm->setPlaceholderText(name);
    m_confirm->setObjectName(QStringLiteral("confirm"));
    m_change->setEnabled(false);
    connect(m_confirm, &QLineEdit::textChanged, this, [this, name](const QString &t) {
        m_change->setEnabled(t.trimmed() == name && m_size->count() > 0 && !m_disk.isSystem);
    });
    connect(m_change, &QPushButton::clicked, this, &SectorSizeDialog::change);
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_change);
    buttons->addWidget(close);
    auto *form = new QFormLayout;
    form->addRow(tr("New size:"), m_size);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(text));
    layout->addLayout(form);
    layout->addWidget(new QLabel(tr("Type <b>%1</b> to confirm:").arg(name)));
    layout->addWidget(m_confirm);
    layout->addLayout(buttons);
    resize(540, sizeHint().height());
}

void SectorSizeDialog::change()
{
    const int size = m_size->currentData().toInt();
    if (size <= 0 || m_disk.isSystem)
        return;
    const QStringList command = QStringList{QStringLiteral("sudo"), QStringLiteral("-n")} + drivepower::setSectorSizeCommand(m_disk.device, size);
    QString said;
    bool ok = false;
    if (runForTest) {
        ok = runForTest(command, &said);
    } else {
        // Its own drive's sectors: a few seconds, though some drives take a minute.
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QProcess hdparm;
        hdparm.setProcessChannelMode(QProcess::MergedChannels);
        hdparm.start(command.first(), command.mid(1));
        ok = hdparm.waitForFinished(180000) && hdparm.exitStatus() == QProcess::NormalExit && hdparm.exitCode() == 0;
        said = QString::fromLocal8Bit(hdparm.readAll()).trimmed();
        QApplication::restoreOverrideCursor();
    }
    qCInfo(lcOps).noquote() << "Sector size of" << m_disk.device << "to" << size << (ok ? "done" : "failed:") << said;
    m_udisks->rescan(m_disk.blockPath);
    if (ok) {
        QMessageBox::information(this, windowTitle(),
                                 tr("Done: %1 now uses %2-byte sectors. If it still shows the old size, switch the PC off fully and on "
                                    "again. Then make a new partition table on it.")
                                     .arg(shortDevice(m_disk.device))
                                     .arg(size));
        accept();
    } else {
        QMessageBox::warning(this, windowTitle(), tr("The drive wouldn't: %1").arg(said));
    }
}

// --- SD cards -----------------------------------------------------------------

SdCardDialog::SdCardDialog(UDisks *udisks, const Disk &disk, int diskNumber, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_disk(disk)
    , m_label(new QLineEdit)
    , m_confirm(new QLineEdit)
    , m_start(new QPushButton(tr("Format")))
    , m_progress(new QProgressBar)
    , m_status(new QLabel)
{
    setWindowTitle(tr("Format Disk %1 as an SD Card").arg(diskNumber));
    const sdcard::Plan plan = sdcard::plan(disk.size, disk.sectorSize);
    // Exact powers of two, said as such: "32 KiB", "4 MiB".
    const auto exact = [](quint64 bytes) {
        return bytes >= 1024 * 1024 ? tr("%1 MiB").arg(bytes / (1024 * 1024)) : tr("%1 KiB").arg(bytes / 1024);
    };
    QString text = tr("<p><b>Disk %1: %2</b></p>").arg(diskNumber).arg(diskTitle(disk).toHtmlEscaped());
    if (!plan.problem.isEmpty()) {
        text += QStringLiteral("<p>") + tr("This one can't be: %1.").arg(plan.problem.toHtmlEscaped()) + QStringLiteral("</p>");
    } else {
        text += tr("<p>Formats it the way the SD Card Association specifies, as cameras, phones and consoles expect: "
                   "<b>%1</b> with %2 clusters, and its partition and data placed on the card's %3 erase blocks, so it "
                   "writes faster and wears less than with a computer's usual format.</p>")
                    .arg(plan.fsName(), exact(quint64(plan.cluster)), exact(plan.boundary));
        if (!disk.sdCard)
            text += tr("<p>This drive doesn't say it's a memory card. The layout works on any drive, but it's made for cards.</p>");
    }
    QStringList lost;
    for (const Volume &v : disk.volumes) {
        if (!v.isContainer)
            lost << QStringLiteral("• ") + describeVolume(v).toHtmlEscaped();
    }
    if (!lost.isEmpty())
        text += redText(tr("This erases:")) + QStringLiteral("<br>") + lost.join(QStringLiteral("<br>"));
    if (!diskWarning(disk).isEmpty())
        text += QStringLiteral("<br>") + redText(diskWarning(disk));

    m_label->setMaxLength(11);
    m_label->setPlaceholderText(tr("Name (up to 11 characters)"));
    m_label->setObjectName(QStringLiteral("sdLabel"));
    m_confirm->setObjectName(QStringLiteral("sdConfirm"));
    const QString name = shortDevice(disk.device);
    m_confirm->setPlaceholderText(name);
    m_progress->setRange(0, 1000);
    m_progress->setVisible(false);
    m_status->setWordWrap(true);
    m_start->setEnabled(false);
    connect(m_confirm, &QLineEdit::textChanged, this, [this, name, plan](const QString &t) {
        m_start->setEnabled(t.trimmed() == name && plan.problem.isEmpty() && !m_running);
    });
    connect(m_start, &QPushButton::clicked, this, &SdCardDialog::start);
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &SdCardDialog::reject);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_start);
    buttons->addWidget(close);

    auto *form = new QFormLayout;
    form->addRow(tr("Name:"), m_label);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(text));
    layout->addLayout(form);
    layout->addWidget(new QLabel(tr("Type <b>%1</b> to confirm:").arg(name)));
    layout->addWidget(m_confirm);
    layout->addWidget(m_progress);
    layout->addWidget(m_status);
    layout->addLayout(buttons);
    m_confirm->setEnabled(plan.problem.isEmpty());
    resize(540, sizeHint().height());
}

SdCardDialog::~SdCardDialog()
{
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
}

void SdCardDialog::reject()
{
    if (!m_running)
        QDialog::reject();
}

void SdCardDialog::start()
{
    m_running = true;
    m_start->setEnabled(false);
    m_confirm->setEnabled(false);
    m_label->setEnabled(false);
    m_status->setText(tr("Waiting for permission…"));
    const QString label = m_label->text().trimmed();
    m_openConn = connect(m_udisks, &UDisks::deviceOpened, this, [this, label](const QString &path, int fd) {
        if (path != m_disk.blockPath)
            return;
        disconnect(m_openConn);
        if (fd < 0) {
            m_running = false;
            m_status->setText(tr("Couldn't open the drive for writing."));
            m_confirm->setEnabled(true);
            m_label->setEnabled(true);
            return;
        }
        m_progress->setValue(0);
        m_progress->setVisible(true);
        m_status->setText(tr("Formatting…"));
        const quint64 size = m_disk.size;
        const int sectorSize = m_disk.sectorSize;
        m_thread = QThread::create([this, fd, size, sectorSize, label] {
            QString error;
            const bool ok = sdcard::write(fd, size, sectorSize, label, &error, [this](quint64 done, quint64 total) {
                QMetaObject::invokeMethod(this, [this, done, total] { m_progress->setValue(total ? int(done * 1000 / total) : 0); });
            });
            blockio::close(fd);
            qCInfo(lcOps).noquote() << "SD card format of" << m_disk.device << (ok ? "done" : "failed: " + error);
            QMetaObject::invokeMethod(this, [this, ok, error] {
                m_thread->wait();
                delete m_thread;
                m_thread = nullptr;
                m_running = false;
                m_progress->setVisible(false);
                m_udisks->rescan(m_disk.blockPath);
                m_udisks->refresh();
                if (ok) {
                    QMessageBox::information(this, windowTitle(), tr("Done: the card is ready."));
                    QDialog::accept();
                } else {
                    m_status->setText(redText(tr("It couldn't be formatted: %1").arg(error.toHtmlEscaped())));
                    m_status->setTextFormat(Qt::RichText);
                    m_confirm->setEnabled(true);
                    m_label->setEnabled(true);
                }
            });
        });
        m_thread->start();
    });
    m_udisks->openDevice(m_disk, true, false);
}

HealthDialog::HealthDialog(UDisks *udisks, const QString &blockPath, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_blockPath(blockPath)
    , m_state(new QLabel)
    , m_explain(new QLabel)
    , m_form(new QFormLayout)
    , m_attributes(new QTreeWidget)
    , m_meaning(new QLabel)
    , m_selftest(new QPushButton(tr("Run Short Self-Test")))
{
    m_explain->setWordWrap(true);
    m_explain->setTextFormat(Qt::RichText);
    m_meaning->setWordWrap(true);
    m_meaning->setTextFormat(Qt::PlainText);
    m_attributes->setRootIsDecorated(false);
    m_attributes->setAlternatingRowColors(true);
    connect(m_attributes, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        m_meaning->setText(item ? item->data(0, Qt::UserRole).toString() : QString());
    });

    auto *refresh = new QPushButton(tr("Refresh"));
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(refresh, &QPushButton::clicked, this, [this] {
        if (const Disk *d = m_udisks->diskByPath(m_blockPath))
            m_udisks->smartUpdate(*d);
    });
    auto test = [this](const QString &kind) {
        if (const Disk *d = m_udisks->diskByPath(m_blockPath)) {
            health::noteSelftestStarted(d->health.key, kind);
            m_udisks->smartSelftest(*d, kind);
        }
    };
    connect(m_selftest, &QPushButton::clicked, this, [test] { test(QStringLiteral("short")); });
    auto *longTest = m_longTest = new QPushButton(tr("Run Long Self-Test"));
    connect(longTest, &QPushButton::clicked, this, [test] { test(QStringLiteral("extended")); });
    // A test made for after a hard drive was moved or posted: it looks for damage from knocks.
    m_conveyance = new QPushButton(tr("Run Conveyance Test"));
    m_conveyance->setToolTip(tr("A short test made for after a drive was moved or shipped: it looks for damage from knocks. "
                                "Not every drive has it."));
    connect(m_conveyance, &QPushButton::clicked, this, [test] { test(QStringLiteral("conveyance")); });
    m_smartOn = new QPushButton(tr("Turn SMART On"));
    m_smartOn->setToolTip(tr("SMART is the drive's own health record. It's off on this drive, so its health can't be told."));
    connect(m_smartOn, &QPushButton::clicked, this, [this] {
        if (const Disk *d = m_udisks->diskByPath(m_blockPath))
            m_udisks->smartSetEnabled(*d, true);
    });
    m_noise = new QPushButton(tr("What's That Noise?"));
    m_noise->setObjectName(QStringLiteral("noise"));
    connect(m_noise, &QPushButton::clicked, this, [this] {
        QMessageBox box(QMessageBox::Information, tr("What's That Noise?"),
                        tr("<p>Hard drives make some sounds that are normal, and some that mean trouble.</p>"
                           "<p><b>Normal</b></p><ul>"
                           "<li>A soft whir as it spins up, then a steady hum.</li>"
                           "<li>Quick ticks or chatter while it reads and writes: its heads moving.</li>"
                           "<li>One click when it parks its heads, after a while idle or when the computer sleeps.</li></ul>"
                           "<p><b>Trouble</b></p><ul>"
                           "<li>A click every second or two, over and over: its heads can't read. Turn it off.</li>"
                           "<li>Grinding or scraping: something is touching the disks inside. Turn it off at once: every second "
                           "scratches them.</li>"
                           "<li>Beeping or buzzing without spinning: its motor can't start (stuck heads, or a motor fault).</li>"
                           "<li>Spinning up, stopping and spinning up again: it can't get ready.</li></ul>"
                           "<p>For any of these, turn it off and don't keep trying. If the files matter a great deal, a data recovery lab "
                           "(with a clean room) can often still get them back, and every try at home makes that harder. If it still "
                           "works now and then, make a Rescue Copy the next time it does, before anything else.</p>"),
                        QMessageBox::Close, this);
        box.setTextFormat(Qt::RichText);
        box.exec();
    });
    m_farm = new QPushButton(tr("Check Its History"));
    m_farm->setObjectName(QStringLiteral("farm"));
    m_farm->setToolTip(tr("Seagate drives keep a second record of their use (FARM) that a SMART reset doesn't touch: a drive sold as "
                          "new with its SMART hours wiped shows up here"));
    connect(m_farm, &QPushButton::clicked, this, &HealthDialog::checkFarm);
    auto *scan = new QPushButton(tr("Scan for Bad Sectors…"));
    connect(scan, &QPushButton::clicked, this, [this] {
        if (const Disk *d = m_udisks->diskByPath(m_blockPath))
            BadSectorsDialog(m_udisks, *d, this).exec();
    });
    m_stopTest = new QPushButton(tr("Stop Test"));
    m_stopTest->setVisible(false);
    connect(m_stopTest, &QPushButton::clicked, this, [this] {
        if (const Disk *d = m_udisks->diskByPath(m_blockPath))
            m_udisks->smartSelftestAbort(*d);
    });
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_smartOn);
    buttons->addWidget(m_selftest);
    buttons->addWidget(m_stopTest);
    buttons->addWidget(longTest);
    buttons->addWidget(m_conveyance);
    buttons->addWidget(scan);
    buttons->addWidget(m_noise);
    buttons->addWidget(m_farm);
    buttons->addWidget(refresh);
    buttons->addStretch();
    buttons->addWidget(close);

    m_btrfs = new QGroupBox(tr("Btrfs"));
    new QVBoxLayout(m_btrfs);
    m_btrfs->setVisible(false);
    m_scrubPoll = new QTimer(this);
    m_scrubPoll->setInterval(2000);
    connect(m_scrubPoll, &QTimer::timeout, this, &HealthDialog::reloadBtrfs);
    m_systemd = new Systemd(this);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_state);
    layout->addWidget(m_explain);
    layout->addLayout(m_form);
    layout->addWidget(m_btrfs);
    layout->addWidget(m_attributes, 1);
    layout->addWidget(m_meaning);
    layout->addLayout(buttons);

    connect(m_udisks, &UDisks::changed, this, &HealthDialog::reload);
    // Firmware: only asked by itself when fwupd is already running (asking starts it).
    if (!firmware::installed())
        m_firmware.state = firmware::Result::State::NotInstalled;
    else if (firmware::running())
        checkFirmware();
    reload();
    resize(680, 660);
}

void HealthDialog::checkFarm()
{
    const Disk *disk = m_udisks->diskByPath(m_blockPath);
    if (!disk || m_farmBusy)
        return;
#ifdef Q_OS_WIN
    // smartctl names Windows' drives /dev/pd0, /dev/pd1... (PhysicalDrive0, 1...).
    const QString device = QStringLiteral("/dev/pd") + disk->device.section(QLatin1String("PhysicalDrive"), -1);
    const QString program = QCoreApplication::applicationDirPath() + QStringLiteral("/smartctl.exe");
    QStringList command = farm::command(device);
    command[0] = QFileInfo::exists(program) ? program : QStringLiteral("smartctl");
#else
    const QString device = disk->device;
    // smartctl talks to the drive itself, which takes admin rights: DiskForge Live has them.
    QStringList command = farm::command(device);
    if (rescue::runningInRescue())
        command = QStringList{QStringLiteral("sudo"), QStringLiteral("-n")} + command;
    else
        command = host::command(command);
#endif
    m_farmBusy = true;
    m_farmText = tr("Asking smartctl…");
    reload();
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process, device](int, QProcess::ExitStatus) {
        process->deleteLater();
        m_farmBusy = false;
        const QByteArray out = process->readAllStandardOutput();
        if (process->error() == QProcess::FailedToStart) {
            m_farmText = tr("This needs smartctl (smartmontools 7.4 or later), which isn't on this computer.");
        } else {
            const farm::Result r = farm::parse(out);
            m_farmText = farm::describe(r);
#ifndef Q_OS_WIN
            if (!r.read && !rescue::runningInRescue())
                m_farmText += QLatin1Char(' ') + tr("smartctl needs admin rights for this: in a terminal, run  sudo smartctl -l farm %1  (or "
                                                     "use DiskForge Live).")
                                                      .arg(device);
#endif
        }
        reload();
    });
    process->start(command.first(), command.mid(1));
}

void HealthDialog::reload()
{
    const Disk *disk = m_udisks->diskByPath(m_blockPath);
    if (!disk) {
        m_state->setText(tr("The disk is gone."));
        return;
    }
    const Health &h = disk->health;
    setWindowTitle(tr("Health of %1").arg(disk->model));
    m_state->setText(QStringLiteral("<span style=\"font-size:16pt; font-weight:bold; color:%1\">%2</span><br>%3")
                         .arg(healthColor(h.state), h.summary.toHtmlEscaped(), diskTitle(*disk).toHtmlEscaped()));

    // Every reason behind the verdict, worst first; notes in the normal text color.
    const Theme &theme = Theme::instance();
    QString explain;
    for (const HealthReason &r : h.reasons) {
        const QString color = r.level == HealthReason::Level::Failing ? theme.html(Theme::Role::Danger)
            : r.level == HealthReason::Level::Warning              ? theme.html(Theme::Role::Warning)
                                                                    : QString();
        explain += color.isEmpty() ? QStringLiteral("<p>%1</p>").arg(r.text.toHtmlEscaped())
                                   : QStringLiteral("<p style=\"color:%1\"><b>%2</b></p>").arg(color, r.text.toHtmlEscaped());
    }
    if (h.state == Health::State::Healthy && h.reasons.isEmpty())
        explain = QStringLiteral("<p>%1</p>").arg(tr("No problems reported.").toHtmlEscaped());
    if (h.usbNoData)
        explain = QStringLiteral("<p>%1</p>").arg(tr("This USB adapter doesn't pass the drive's health data through. Connected "
                                                     "directly (SATA or NVMe), DiskForge can read it.").toHtmlEscaped());
    m_explain->setText(explain);
    m_explain->setVisible(!explain.isEmpty());

    while (m_form->rowCount() > 0)
        m_form->removeRow(0);
    auto row = [this](const QString &name, const QString &value) {
        auto *label = new QLabel(value);
        label->setTextFormat(Qt::PlainText);
        m_form->addRow(name, label);
    };
    if (h.temperatureC > 0) {
        QString temp = QStringLiteral("%1 °C").arg(qRound(h.temperatureC));
        if (h.warningTempC > 0)
            temp += tr(" (its own limits: warning at %1 °C, critical at %2 °C)").arg(qRound(h.warningTempC)).arg(qRound(h.criticalTempC));
        row(tr("Temperature:"), temp);
    }
    if (h.powerOnHours > 0)
        row(tr("Powered on:"), tr("%L1 hours (%L2 days)").arg(h.powerOnHours).arg(h.powerOnHours / 24));
    if (h.lifeLeft >= 0)
        row(tr("Life left:"), QStringLiteral("%1%").arg(h.lifeLeft));
    if (h.reallocatedSectors >= 0)
        row(tr("Replaced sectors:"), QString::number(h.reallocatedSectors));
    if (h.pendingSectors >= 0)
        row(tr("Unreadable sectors:"), QString::number(h.pendingSectors));
    if (!h.criticalWarnings.isEmpty())
        row(tr("Warnings:"), h.criticalWarnings.join(QStringLiteral(", ")));
    if (!m_farmText.isEmpty()) {
        auto *label = new QLabel(m_farmText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse); // the command, when it has to be run by hand
        label->setObjectName(QStringLiteral("farmText"));
        m_form->addRow(tr("History (FARM):"), label);
    }
    if (const drivefacts::EmmcLife life = drivefacts::emmcLife(disk->device); life.known)
        row(tr("Wear (eMMC):"), drivefacts::describe(life));
    if (const QStringList off = drivefacts::misaligned(*disk); !off.isEmpty())
        row(tr("Alignment:"), off.join(QLatin1Char('\n')) + QLatin1Char('\n')
                                  + tr("That makes writes slower, and on an SSD wears it more. Moving the partition to a boundary fixes it "
                                       "(a backup, a new partition, then restoring)."));
    QStringList facts;
    for (const drivefacts::Fact &f : drivefacts::aboutModel(disk->model, disk->revision))
        facts << f.text;
    if (!facts.isEmpty())
        row(tr("About this model:"), facts.join(QLatin1Char('\n')));
    row(tr("Self-test:"), selftestText(h));
    // The ones DiskForge saw finish, newest first.
    QStringList earlier;
    for (const health::SelftestRecord &r : health::selftestHistory(h.key).mid(0, 5))
        earlier << tr("%1, %2: %3").arg(QLocale().toString(r.finished.toLocalTime(), QLocale::ShortFormat), health::selftestKind(r.kind),
                                        health::selftestResult(r.status));
    if (!earlier.isEmpty())
        row(tr("Earlier tests:"), earlier.join(QLatin1Char('\n')));
    if (!disk->isLoop && !disk->revision.isEmpty()) {
        // The version from the drive, then what fwupd says about it, with what can be done.
        auto *firmwareBox = new QWidget;
        auto *firmwareLayout = new QHBoxLayout(firmwareBox);
        firmwareLayout->setContentsMargins(0, 0, 0, 0);
        const QString said = m_firmwareBusy ? tr("Asking fwupd…") : firmware::describe(m_firmware);
        auto *firmwareText = new QLabel(said.isEmpty() ? disk->revision : tr("%1. %2").arg(disk->revision, said));
        firmwareText->setTextFormat(Qt::PlainText);
        firmwareText->setWordWrap(true);
        firmwareText->setTextInteractionFlags(Qt::TextSelectableByMouse); // the fwupdmgr commands
        firmwareLayout->addWidget(firmwareText, 1);
        using State = firmware::Result::State;
        if (!m_firmwareBusy && m_firmware.state != State::NotInstalled && m_firmware.state != State::NotUpdatable
            && m_firmware.state != State::Unknown) {
            auto *check = new QPushButton(m_firmware.state == State::NotRunning ? tr("Check for Updates") : tr("Check Again"));
            connect(check, &QPushButton::clicked, this, &HealthDialog::checkFirmware);
            firmwareLayout->addWidget(check, 0, Qt::AlignTop);
        }
        const QStringList updater = firmwareUpdater();
        if (m_firmware.state == State::Available && !updater.isEmpty()) {
            auto *open = new QPushButton(tr("Open Updater"));
            connect(open, &QPushButton::clicked, this, [updater] { QProcess::startDetached(updater.first(), updater.mid(1)); });
            firmwareLayout->addWidget(open, 0, Qt::AlignTop);
        }
        auto *firmwareName = new QLabel(tr("Firmware:"));
        m_form->addRow(firmwareName, firmwareBox);
    }
    const bool running = h.selftestStatus == QLatin1String("inprogress");
#ifdef Q_OS_WIN
    // On Windows the drive's own Identify data says whether it does self-tests.
    m_selftest->setVisible(!h.nvme || h.nvmeSelftest);
    m_longTest->setVisible(!h.nvme || h.nvmeSelftest);
#endif
    m_selftest->setEnabled(!running && !h.smartOff);
    m_stopTest->setVisible(running);
    m_conveyance->setVisible(!h.nvme && !disk->isLoop && disk->rotationRate != 0);
    m_noise->setVisible(!h.nvme && !disk->isLoop && disk->rotationRate != 0 && !disk->removable);
    m_farm->setVisible(!h.nvme && !disk->isLoop && farm::keeps(disk->model));
    m_farm->setEnabled(!m_farmBusy);
    m_conveyance->setEnabled(!running && !h.smartOff);
    m_smartOn->setVisible(h.smartOff);

    const QVector<SmartAttribute> attrs = m_udisks->smartAttributes(*disk);
    m_attributes->clear();
    const bool ata = !h.nvme;
    m_attributes->setHeaderLabels(ata ? QStringList{tr("ID"), tr("Attribute"), tr("What it is"), tr("Value"), tr("Worst"), tr("Threshold"), tr("Raw")}
                                      : QStringList{tr("Attribute"), tr("Value")});
    for (const SmartAttribute &a : attrs) {
        const health::AttributeInfo info = ata ? health::describe(a.id) : health::AttributeInfo();
        auto *item = ata ? new QTreeWidgetItem(m_attributes, {QString::number(a.id), a.name, info.name, QString::number(a.value),
                                                               QString::number(a.worst), QString::number(a.threshold), a.raw})
                         : new QTreeWidgetItem(m_attributes, {a.name, a.raw});
        if (!info.meaning.isEmpty())
            item->setData(0, Qt::UserRole, tr("%1: %2").arg(info.name, info.meaning)
                                               + (info.counts ? QLatin1Char(' ') + tr("This one counts toward the verdict.") : QString()));
        if (!info.meaning.isEmpty())
            for (int c = 0; c < item->columnCount(); ++c)
                item->setToolTip(c, Qt::convertFromPlainText(info.meaning, Qt::WhiteSpaceNormal));
        if (info.counts) {
            // The ones that go into the verdict.
            QFont bold = item->font(0);
            bold.setBold(true);
            for (int c = 0; c < item->columnCount(); ++c)
                item->setFont(c, bold);
        }
        if (a.failing) {
            for (int c = 0; c < item->columnCount(); ++c)
                item->setForeground(c, theme.color(Theme::Role::Danger));
        }
    }
    for (int c = 0; c < m_attributes->columnCount(); ++c)
        m_attributes->resizeColumnToContents(c);
    m_meaning->setText(ata ? tr("Bold ones count toward the verdict. Select one to see what it means.") : QString());
    reloadBtrfs();
}

// One block per Btrfs partition on the drive: its error counts, the last scrub and what it
// found, and buttons to scrub now or every month. Rebuilt every time, from systemd and sysfs,
// so a scrub that runs while the window is closed shows up when it opens again.
void HealthDialog::reloadBtrfs()
{
    const Disk *disk = m_udisks->diskByPath(m_blockPath);
    QLayout *layout = m_btrfs->layout();
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    bool any = false, scrubbing = false;
    const bool haveUnits = m_systemd->unitExists(btrfscheck::scrubUnit(QStringLiteral("/")));
    for (const Volume &v : disk ? disk->volumes : QVector<Volume>()) {
        if (v.effectiveFsType() != QLatin1String("btrfs"))
            continue;
        any = true;
        const QString name = QFileInfo(v.device).fileName();
        auto *box = new QWidget;
        auto *rows = new QVBoxLayout(box);
        rows->setContentsMargins(0, 0, 0, 0);
        auto *title = new QLabel(v.label.isEmpty() ? name : tr("%1 (%2)").arg(name, v.label));
        title->setTextFormat(Qt::PlainText);
        QFont bold = title->font();
        bold.setBold(true);
        title->setFont(bold);
        rows->addWidget(title);

        if (v.mounts().isEmpty()) {
            auto *note = new QLabel(tr("Mount it to see its error counts and to scrub it."));
            note->setWordWrap(true);
            rows->addWidget(note);
            layout->addWidget(box);
            continue;
        }
        const btrfscheck::Counts counts = btrfscheck::read(kernelName(v.encrypted ? v.cleartextDevice : v.device));
        auto *countsText = new QLabel(btrfscheck::describe(counts)
                                      + (counts.total() > 0 ? QLatin1Char(' ') + tr("They stay until reset with: sudo btrfs device stats -z %1")
                                                                                     .arg(btrfscheck::scrubMountPoint(v.mounts()))
                                                            : QString()));
        countsText->setTextFormat(Qt::PlainText);
        countsText->setWordWrap(true);
        countsText->setTextInteractionFlags(Qt::TextSelectableByMouse);
        if (counts.total() > 0) {
            QPalette p = countsText->palette();
            p.setColor(QPalette::WindowText, Theme::instance().color(Theme::Role::Danger));
            countsText->setPalette(p);
        }
        rows->addWidget(countsText);

        if (!haveUnits) {
            auto *note = new QLabel(tr("Scrubbing from here needs btrfs-progs' btrfs-scrub@ units, which aren't installed."));
            note->setWordWrap(true);
            rows->addWidget(note);
            layout->addWidget(box);
            continue;
        }
        const QString mountPoint = btrfscheck::scrubMountPoint(v.mounts());
        const QString unit = btrfscheck::scrubUnit(mountPoint), timer = btrfscheck::scrubTimer(mountPoint);
        const Systemd::ServiceState state = m_systemd->serviceState(unit);
        QString last;
        if (state.running()) {
            scrubbing = true;
            last = tr("Scrubbing since %1. It reads everything at low priority, so the PC stays usable; it can take hours on a big drive.")
                       .arg(QLocale().toString(QDateTime::fromMSecsSinceEpoch(qint64(state.started / 1000)).time(), QLocale::ShortFormat));
        } else if (state.exited > 0) {
            const QString when = QLocale().toString(QDateTime::fromMSecsSinceEpoch(qint64(state.exited / 1000)), QLocale::ShortFormat);
            const QString found = m_stopped.contains(unit) ? tr("Stopped.")
                : btrfscheck::scrubOutcome(state.conditionMet, state.exitStatus, state.result, m_errorsBefore.value(unit, counts.total()), counts.total());
            last = tr("Last scrub %1: %2").arg(when, found);
        } else {
            last = tr("Not scrubbed from here yet. A scrub reads everything back and checks it against its checksums.");
        }
        auto *lastText = new QLabel(last);
        lastText->setTextFormat(Qt::PlainText);
        lastText->setWordWrap(true);
        rows->addWidget(lastText);

        auto *actions = new QHBoxLayout;
        // Queued: these rows are rebuilt while the buttons' dialogs are open.
        const QString device = v.device;
        if (state.running()) {
            auto *stop = new QPushButton(tr("Stop Scrub"));
            connect(stop, &QPushButton::clicked, this, [this, unit] { QTimer::singleShot(0, this, [this, unit] { stopScrub(unit); }); });
            actions->addWidget(stop);
        } else {
            auto *start = new QPushButton(tr("Scrub Now…"));
            connect(start, &QPushButton::clicked, this, [this, device, mountPoint] {
                QTimer::singleShot(0, this, [this, device, mountPoint] { startScrub(device, mountPoint); });
            });
            actions->addWidget(start);
        }
        auto *monthly = new QCheckBox(tr("Scrub every month"));
        monthly->setChecked(m_systemd->unitFileState(timer) == QLatin1String("enabled"));
        if (rescue::runningInRescue()) {
            monthly->setEnabled(false);
            monthly->setToolTip(rescue::notInRescueReason());
        }
        connect(monthly, &QCheckBox::toggled, this, [this, timer](bool on) {
            QTimer::singleShot(0, this, [this, timer, on] { setMonthlyScrub(timer, on); });
        });
        actions->addWidget(monthly);
        actions->addStretch();
        rows->addLayout(actions);
        layout->addWidget(box);
    }
    m_btrfs->setVisible(any);
    if (scrubbing && !m_scrubPoll->isActive())
        m_scrubPoll->start();
    else if (!scrubbing)
        m_scrubPoll->stop();
}

void HealthDialog::startScrub(const QString &device, const QString &mountPoint)
{
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("Scrub %1").arg(QFileInfo(device).fileName()),
        tr("Scrub the Btrfs file system on %1?\n\nA scrub reads everything on it and checks it against its checksums, "
           "repairing what it can from a second copy. It runs in the background at low priority, so the PC stays "
           "usable, but it can take hours on a big drive. It can be stopped any time.")
            .arg(QFileInfo(device).fileName()),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
    if (answer != QMessageBox::Yes)
        return;
    const QString unit = btrfscheck::scrubUnit(mountPoint);
    const Disk *disk = m_udisks->diskByPath(m_blockPath);
    qint64 before = 0;
    for (const Volume &v : disk ? disk->volumes : QVector<Volume>()) {
        if (v.device == device)
            before = btrfscheck::read(kernelName(v.encrypted ? v.cleartextDevice : v.device)).total();
    }
    m_errorsBefore.insert(unit, before);
    m_stopped.remove(unit);
    m_systemd->ref(unit); // so its result can still be read when it's done
    m_systemd->startUnit(unit, [this](bool ok, const QString &message) {
        if (!ok && message != QLatin1String("canceled"))
            QMessageBox::warning(this, windowTitle(), tr("The scrub didn't start: %1").arg(message));
        reloadBtrfs();
    });
}

void HealthDialog::stopScrub(const QString &unit)
{
    m_stopped.insert(unit);
    m_systemd->stopUnit(unit, [this](bool, const QString &) { reloadBtrfs(); });
}

void HealthDialog::setMonthlyScrub(const QString &timer, bool on)
{
    m_systemd->setTimerEnabled(timer, on, [this](bool ok, const QString &message) {
        if (!ok)
            QMessageBox::warning(this, windowTitle(), message);
        reloadBtrfs();
    });
}

void HealthDialog::checkFirmware()
{
    const Disk *disk = m_udisks->diskByPath(m_blockPath);
    if (!disk || m_firmwareBusy)
        return;
    m_firmwareBusy = true;
    auto *check = new firmware::Check(this);
    connect(check, &firmware::Check::finished, this, [this, check](const firmware::Result &result) {
        check->deleteLater();
        m_firmware = result;
        m_firmwareBusy = false;
        reload();
    });
    check->start(disk->serial, disk->model, disk->revision);
    // Shown on the next pass: the Check button is in the rows reload() rebuilds.
    QTimer::singleShot(0, this, &HealthDialog::reload);
}

// --- Benchmark ----------------------------------------------------------------

BenchmarkDialog::BenchmarkDialog(UDisks *udisks, const Disk &disk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_disk(disk)
    , m_writeTest(new QCheckBox)
    , m_start(new QPushButton(tr("Start")))
    , m_progress(new QProgressBar)
    , m_phase(new QLabel)
    , m_results(new QLabel)
{
    setWindowTitle(tr("Benchmark %1").arg(disk.model));
    // A writable mount on this disk, or your home folder if it lives on this disk.
    const QString home = QDir::homePath();
    QString homeMount;
    for (const Volume &v : disk.volumes) {
        for (const QString &mp : v.mounts()) {
            if (m_writeDir.isEmpty() && QFileInfo(mp).isWritable())
                m_writeDir = mp;
            if ((home == mp || home.startsWith(mp == QLatin1String("/") ? mp : mp + QLatin1Char('/'))) && mp.size() > homeMount.size())
                homeMount = mp;
        }
    }
    if (m_writeDir.isEmpty() && !homeMount.isEmpty())
        m_writeDir = home;
    m_writeTest->setText(m_writeDir.isEmpty() ? tr("Write speed (mount a partition on this disk to test it)")
                                              : tr("Also test write speed (a 256 MB file in %1, deleted afterwards)").arg(m_writeDir));
    m_writeTest->setEnabled(!m_writeDir.isEmpty());
    m_progress->setRange(0, 100);
    m_progress->setVisible(false);
    m_results->setTextFormat(Qt::RichText);

    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &BenchmarkDialog::reject);
    connect(m_start, &QPushButton::clicked, this, &BenchmarkDialog::start);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_start);
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("<b>%1</b><br>Measures how fast the drive reads and how quickly it finds data. "
                                       "Nothing on the drive is changed.").arg(diskTitle(disk).toHtmlEscaped())));
    layout->addWidget(m_writeTest);
    layout->addWidget(m_phase);
    layout->addWidget(m_progress);
    layout->addWidget(m_results);
    layout->addStretch();
    layout->addLayout(buttons);
    resize(520, 320);
}

BenchmarkDialog::~BenchmarkDialog()
{
    stop();
}

void BenchmarkDialog::stop()
{
    disconnect(m_openConn);
    if (m_thread) {
        if (auto *b = qobject_cast<Benchmark *>(m_worker))
            b->cancel();
        m_thread->quit();
        m_thread->wait();
        m_thread = nullptr;
    }
}

void BenchmarkDialog::reject()
{
    stop();
    QDialog::reject();
}

void BenchmarkDialog::start()
{
    m_start->setEnabled(false);
    m_writeTest->setEnabled(false);
    m_results->clear();
    m_phase->setText(tr("Waiting for permission…"));
    m_openConn = connect(m_udisks, &UDisks::deviceOpened, this, [this](const QString &path, int fd) {
        if (path != m_disk.blockPath)
            return;
        disconnect(m_openConn);
        if (fd < 0) {
            m_phase->setText(tr("Couldn't open the drive."));
            m_start->setEnabled(true);
            return;
        }
        auto *bench = new Benchmark(fd, m_disk.size, m_writeTest->isChecked() ? m_writeDir : QString());
        m_worker = bench;
        m_thread = new QThread(this);
        bench->moveToThread(m_thread);
        connect(m_thread, &QThread::started, bench, &Benchmark::run);
        connect(m_thread, &QThread::finished, bench, &QObject::deleteLater);
        connect(bench, &Benchmark::progress, this, [this](const QString &phase, int percent) {
            m_phase->setText(phase);
            m_progress->setValue(percent);
        });
        connect(bench, &Benchmark::finished, this, [this](bool ok, const BenchmarkResult &r, const QString &error) {
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_worker = nullptr;
            m_progress->setVisible(false);
            m_start->setEnabled(true);
            m_start->setText(tr("Run Again"));
            m_writeTest->setEnabled(!m_writeDir.isEmpty());
            if (!ok) {
                m_phase->setText(error);
                return;
            }
            m_phase->clear();
            QString text = tr("<p><b>Read speed:</b> %1 MB/s<br><small>slowest part %2 MB/s, fastest %3 MB/s</small></p>"
                              "<p><b>Access time:</b> %4 ms</p>")
                               .arg(qRound(r.readMBps)).arg(qRound(r.minReadMBps)).arg(qRound(r.maxReadMBps))
                               .arg(r.accessMs, 0, 'f', 2);
            if (r.writeMBps > 0)
                text += tr("<p><b>Write speed:</b> %1 MB/s</p>").arg(qRound(r.writeMBps));
            m_results->setText(text);
        });
        m_progress->setValue(0);
        m_progress->setVisible(true);
        m_thread->start();
    });
    m_udisks->openDevice(m_disk, false, true);
}

// --- Write image --------------------------------------------------------------

// The four checksums of a file, worked out in one pass, each with Copy: to compare with what
// a download page says by eye, or to note down.
void showChecksums(QWidget *parent, const QString &path)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Checksums of %1").arg(QFileInfo(path).fileName()));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    QList<QLineEdit *> fields;
    for (QCryptographicHash::Algorithm a : checksums::algorithms()) {
        auto *field = new QLineEdit;
        field->setReadOnly(true);
        field->setPlaceholderText(QObject::tr("Working it out…"));
        field->setMinimumWidth(field->fontMetrics().horizontalAdvance(QLatin1Char('0')) * 66);
        auto *copy = new QPushButton(QObject::tr("Copy"));
        QObject::connect(copy, &QPushButton::clicked, field, [field] { QGuiApplication::clipboard()->setText(field->text()); });
        auto *row = new QHBoxLayout;
        row->addWidget(field, 1);
        row->addWidget(copy);
        form->addRow(checksums::name(a) + QLatin1Char(':'), row);
        fields.append(field);
    }
    auto *progress = new QProgressBar;
    progress->setRange(0, 1000);
    layout->addLayout(form);
    layout->addWidget(progress);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(box);

    auto *hasher = new checksums::Hasher(path);
    QObject::connect(hasher, &checksums::Hasher::progress, progress, [progress](quint64 done, quint64 total) {
        progress->setValue(total ? int(done * 1000 / total) : 0);
    });
    QThread *thread = nullptr;
    QObject::connect(hasher, &checksums::Hasher::finished, &dialog, [&](bool ok, const QStringList &hex, const QString &error) {
        progress->setVisible(false);
        for (int i = 0; i < fields.size(); ++i)
            fields[i]->setText(ok ? hex.value(i) : QString());
        if (!ok)
            fields.first()->setPlaceholderText(error);
        thread->quit();
    });
    thread = startOnThread(&dialog, hasher);
    dialog.exec();
    hasher->cancel();
    thread->quit();
    thread->wait();
}

bool WriteImageDialog::allowLoopDevicesForTest = false;

WriteImageDialog::WriteImageDialog(UDisks *udisks, const QString &preferredDisk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_image(new QLineEdit)
    , m_imageInfo(new QLabel)
    , m_targets(new QComboBox)
    , m_sha(new QLineEdit)
    , m_verify(new QCheckBox(tr("Check the drive after writing (recommended)")))
    , m_warning(new QLabel)
    , m_confirm(new QLineEdit)
    , m_progress(new QProgressBar)
    , m_phase(new QLabel)
    , m_modeBox(new QWidget)
    , m_asIs(new QRadioButton(tr("Write it as it is: works on old BIOS PCs too")))
    , m_copyMode(new QRadioButton(tr("Copy the files: the stick stays usable for other files (UEFI PCs only)")))
    , m_persist(new QCheckBox(tr("Keep changes between starts (persistence):")))
    , m_persistGb(new QSpinBox)
    , m_modeNote(new QLabel)
{
    setWindowTitle(tr("Write Image to USB"));
    m_asIs->setObjectName(QStringLiteral("asIs"));
    m_copyMode->setObjectName(QStringLiteral("copyMode"));
    m_persist->setObjectName(QStringLiteral("persist"));
    m_persistGb->setObjectName(QStringLiteral("persistSize"));
    m_image->setObjectName(QStringLiteral("image"));
    m_imageInfo->setObjectName(QStringLiteral("imageInfo"));
    m_sha->setObjectName(QStringLiteral("checksum"));
    m_confirm->setObjectName(QStringLiteral("confirm"));
    auto *browse = new QPushButton(tr("Browse…"));
    browse->setAutoDefault(false);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Choose an Image"), QDir::homePath() + QStringLiteral("/Downloads"),
                                                          tr("Disk images (*.iso *.img *.raw *.xz *.gz *.bz2 *.zst *.lzma *.zip);;All files (*)"));
        if (!file.isEmpty())
            m_image->setText(file);
    });
    auto *imageRow = new QHBoxLayout;
    imageRow->addWidget(m_image, 1);
    imageRow->addWidget(browse);
    m_image->setPlaceholderText(tr("archlinux-x86_64.iso"));
    m_sha->setPlaceholderText(tr("Optional: MD5, SHA-1, SHA-256 or SHA-512 from the download page"));
    auto *fromFile = new QPushButton(tr("From a File…"));
    fromFile->setAutoDefault(false);
    fromFile->setToolTip(tr("Take it from a checksum file, like SHA256SUMS"));
    connect(fromFile, &QPushButton::clicked, this, [this] {
        const QString image = m_image->text().trimmed();
        const QString file = QFileDialog::getOpenFileName(this, tr("Choose the Checksum File"),
                                                          image.isEmpty() ? QDir::homePath() : QFileInfo(image).path(),
                                                          tr("Checksum files (*SUMS* *.sha256 *.sha512 *.sha1 *.md5 *.txt);;All files (*)"));
        if (file.isEmpty())
            return;
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, windowTitle(), f.errorString());
            return;
        }
        // The checksum is for the file as it was downloaded.
        checksums::Expected e = checksums::fromFile(f.read(1024 * 1024), QFileInfo(image).fileName());
        if (!e.isSet())
            return (void)QMessageBox::warning(this, windowTitle(), e.error);
        m_sha->setText(QString::fromLatin1(e.hex));
    });
    auto *showAll = new QPushButton(tr("Checksums…"));
    showAll->setAutoDefault(false);
    showAll->setToolTip(tr("Work out the MD5, SHA-1, SHA-256 and SHA-512 of the image"));
    connect(showAll, &QPushButton::clicked, this, [this] {
        if (QFileInfo(m_image->text().trimmed()).isFile())
            showChecksums(this, m_image->text().trimmed());
    });
    auto *shaRow = new QHBoxLayout;
    shaRow->addWidget(m_sha, 1);
    shaRow->addWidget(fromFile);
    shaRow->addWidget(showAll);
    m_verify->setChecked(true);
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::RichText);
    m_progress->setRange(0, 1000);
    m_progress->setVisible(false);

    auto *form = new QFormLayout;
    form->addRow(tr("Image:"), imageRow);
    form->addRow(QString(), m_imageInfo);
    form->addRow(tr("Drive:"), m_targets);
    form->addRow(tr("Checksum:"), shaRow);
    form->addRow(QString(), m_verify);
    // For ISOs: as they are, or their files copied (Rufus's "ISO mode").
    m_asIs->setChecked(true);
    m_persistGb->setSuffix(tr(" GB"));
    m_persistGb->setRange(1, 1);
    m_modeNote->setWordWrap(true);
    auto *modes = new QButtonGroup(this);
    modes->addButton(m_asIs);
    modes->addButton(m_copyMode);
    auto *persistRow = new QHBoxLayout;
    persistRow->setContentsMargins(24, 0, 0, 0);
    persistRow->addWidget(m_persist);
    persistRow->addWidget(m_persistGb);
    persistRow->addStretch();
    auto *modeLayout = new QVBoxLayout(m_modeBox);
    modeLayout->setContentsMargins(0, 0, 0, 0);
    modeLayout->addWidget(m_asIs);
    modeLayout->addWidget(m_copyMode);
    modeLayout->addLayout(persistRow);
    modeLayout->addWidget(m_modeNote);
    form->addRow(tr("How:"), m_modeBox);
    m_form = form;

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(wrappingLabel(tr("<small>Works for Linux ISOs (Arch, Ubuntu, Fedora...), disk and SD card images, "
                                       "compressed or not. For Windows ISOs there's Make a Windows USB in the File menu.</small>")));
    layout->addWidget(m_warning);
    m_toWindows = new QPushButton(tr("Make a Windows USB From It…"));
    m_toWindows->setVisible(false);
    m_toWindows->setAutoDefault(false);
    connect(m_toWindows, &QPushButton::clicked, this, [this] {
        const QString iso = m_image->text().trimmed();
        const QString disk = m_targets->currentData().toString();
        hide();
        WindowsUsbDialog(m_udisks, disk, parentWidget(), iso).exec();
        QDialog::reject();
    });
    layout->addWidget(m_toWindows, 0, Qt::AlignLeft);
    layout->addWidget(m_confirm);
    layout->addWidget(m_phase);
    layout->addWidget(m_progress);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_write = box->addButton(tr("Write"), QDialogButtonBox::ActionRole);
    m_write->setAutoDefault(false);
    box->button(QDialogButtonBox::Cancel)->setDefault(true);
    connect(m_write, &QPushButton::clicked, this, &WriteImageDialog::start);
    connect(box, &QDialogButtonBox::rejected, this, &WriteImageDialog::reject);
    layout->addWidget(box);

    fillTargets(preferredDisk);
    connect(m_image, &QLineEdit::textChanged, this, &WriteImageDialog::updateState);
    connect(m_targets, &QComboBox::currentIndexChanged, this, &WriteImageDialog::updateState);
    connect(m_confirm, &QLineEdit::textChanged, this, &WriteImageDialog::updateState);
    connect(m_sha, &QLineEdit::textChanged, this, &WriteImageDialog::updateState);
    connect(modes, &QButtonGroup::buttonToggled, this, &WriteImageDialog::updateState);
    connect(m_persist, &QCheckBox::toggled, this, &WriteImageDialog::updateState);
    // Sticks plugged in while the dialog is open.
    connect(m_udisks, &UDisks::changed, this, [this] {
        if (m_running)
            return;
        fillTargets(m_targets->currentData().toString());
        updateState();
    });
    updateState();
    resize(600, sizeHint().height());
}

WriteImageDialog::~WriteImageDialog()
{
    disconnect(m_openConn);
    if (m_thread) {
        if (auto *w = qobject_cast<ImageWriter *>(m_worker))
            w->cancel();
        m_thread->quit();
        m_thread->wait();
    }
}

void WriteImageDialog::fillTargets(const QString &preferred)
{
    const QSignalBlocker block(m_targets);
    m_targets->clear();
    const QVector<Disk> &disks = m_udisks->disks();
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        // USB and removable drives only: writing an image over an internal disk is almost always a mistake.
        if (d.isSystem || d.isRaid)
            continue;
        if (d.isLoop ? !allowLoopDevicesForTest : !(d.removable || d.bus == QLatin1String("usb")))
            continue;
        m_targets->addItem(tr("Disk %1: %2").arg(i).arg(diskTitle(d)), d.blockPath);
        if (d.blockPath == preferred)
            m_targets->setCurrentIndex(m_targets->count() - 1);
    }
    if (m_targets->count() == 0)
        m_targets->addItem(tr("No drive to write to (plug in a USB stick)"));
}

const Disk *WriteImageDialog::target() const
{
    return m_udisks->diskByPath(m_targets->currentData().toString());
}

void WriteImageDialog::updateState()
{
    if (m_running)
        return;
    const QString path = m_image->text().trimmed();
    const QFileInfo image(path);
    const Disk *d = target();
    bool ok = image.isFile() && d;

    // What the image is, looked at once per file: compressed or not, and its size once unpacked.
    if (path != m_described) {
        m_described = path;
        m_unpacked = 0;
        m_isIso = false;
        m_analysis = {};
        QString info;
        if (image.isFile()) {
            QString error;
            const std::unique_ptr<ImageSource> source = ImageSource::open(path, &error);
            if (!source) {
                info = redText(error);
            } else if (source->compression().isEmpty()) {
                m_unpacked = quint64(image.size());
                info = formatSize(m_unpacked).toHtmlEscaped();
                // An ISO can also have its files copied: see what this one allows.
                const std::unique_ptr<filecopy::Source> iso = filecopy::openIso(path);
                m_isIso = iso->error().isEmpty();
                if (m_isIso)
                    m_analysis = isomode::analyse(iso->entries(), iso->label());
            } else {
                m_unpacked = source->size();
                const QString inside = source->innerName().isEmpty() ? QString() : tr(" (%1 inside)").arg(source->innerName());
                info = (m_unpacked ? tr("Compressed (%1), %2. Unpacks to %3%4.").arg(source->compression(), formatSize(quint64(image.size())), formatSize(m_unpacked), inside)
                                   : tr("Compressed (%1), %2%3. It's unpacked on the way to the drive.").arg(source->compression(), formatSize(quint64(image.size())), inside))
                           .toHtmlEscaped();
            }
            if (source && !hasBootSignature(path))
                info += QStringLiteral("<br><small>")
                      + tr("It has no boot record, so a PC can't start from it as it is. That's fine for SD card images and "
                           "data, not for an installer.").toHtmlEscaped()
                      + QStringLiteral("</small>");
        } else if (!path.isEmpty()) {
            info = tr("File not found");
        }
        m_imageInfo->setText(info);
    }

    // As it is, or the files copied.
    m_form->setRowVisible(m_modeBox, m_isIso);
    m_copyMode->setEnabled(m_analysis.canCopy());
    if (!m_analysis.canCopy() && m_copyMode->isChecked())
        m_asIs->setChecked(true);
    const bool copy = m_isIso && m_copyMode->isChecked();
    m_verify->setEnabled(!copy); // copying always checks every file
    const quint64 room = d && d->size > m_analysis.bytes * 21 / 20 + 256 * 1024 * 1024 ? d->size - m_analysis.bytes * 21 / 20 - 256 * 1024 * 1024 : 0;
#ifdef Q_OS_WIN
    // The changes are kept on an ext4 partition, which Windows can't make.
    const bool canPersist = false;
#else
    const bool canPersist = copy && m_analysis.persistence != isomode::Persistence::None && room >= 1000000000ULL;
#endif
    m_persist->setEnabled(canPersist);
    m_persistGb->setMaximum(std::max<int>(1, int(room / 1000000000ULL)));
    m_persistGb->setEnabled(canPersist && m_persist->isChecked());
    QString note = m_isIso && !m_analysis.canCopy() && !m_analysis.windows ? m_analysis.whyNot() : QString();
    if (copy && m_analysis.persistence == isomode::Persistence::None)
        note = tr("This one doesn't keep changes between starts.");
#ifdef Q_OS_WIN
    else if (copy)
        note = tr("Keeping changes between starts needs an ext4 partition, which Windows can't make; DiskForge Live can.");
#endif
    else if (copy && !canPersist)
        note = tr("There's no room on this stick to keep changes between starts.");
    m_modeNote->setText(note);
    m_modeNote->setVisible(!note.isEmpty());

    const checksums::Expected checksum = checksums::parse(m_sha->text());
    QString warning;
    if (!checksum.error.isEmpty()) {
        warning = redText(checksum.error) + QStringLiteral("<br>");
        ok = false;
    }
    if (m_isIso && m_analysis.windows) {
        // Written as it is, a Windows ISO doesn't start from a USB stick.
        warning += redText(tr("This is a Windows ISO: written as it is, it won't start a PC from a USB stick. Make a Windows "
                              "USB does those.")) + QStringLiteral("<br>");
        ok = false;
    }
    m_toWindows->setVisible(m_isIso && m_analysis.windows);
    bool asked = false;
    if (d) {
        // Compressed and not saying its size: at least the file itself has to fit.
        quint64 needed = m_unpacked ? m_unpacked : quint64(image.size());
        if (copy)
            needed = m_analysis.bytes + 64 * 1024 * 1024 + (m_persist->isChecked() && canPersist ? quint64(m_persistGb->value()) * 1000000000ULL : 0);
        if (image.isFile() && blockio::pathIsOnDisk(image.absoluteFilePath(), d->device)) {
            warning += redText(tr("The image is on this drive, so writing it there would destroy it on the way. "
                                  "Pick another drive, or copy the image somewhere else first."));
            ok = false;
        } else if (image.isFile() && needed > d->size) {
            warning += redText(tr("The image (%1) is bigger than this drive.").arg(formatSize(needed)));
            ok = false;
        } else {
            warning += redText(tr("Everything on %1 will be erased.").arg(diskTitle(*d)));
            if (!diskWarning(*d).isEmpty())
                warning += QStringLiteral("<br>") + redText(diskWarning(*d));
            const QString name = shortDevice(d->device);
            warning += QStringLiteral("<br>") + tr("Type <b>%1</b> to confirm:").arg(name);
            m_confirm->setPlaceholderText(name);
            ok = ok && m_confirm->text().trimmed() == name;
            asked = true;
        }
    }
    m_warning->setText(warning);
    m_confirm->setVisible(asked);
    m_write->setEnabled(ok);
}

void WriteImageDialog::reject()
{
    if (m_isoCopy) {
        if (!m_isoCopy->canCancel()) {
            QMessageBox::information(this, windowTitle(), tr("One moment: DiskForge is in the middle of setting up the stick."));
            return;
        }
        const auto answer = QMessageBox::warning(this, windowTitle(), tr("Stop? The stick won't be usable until it's done again."),
                                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes && m_isoCopy)
            m_isoCopy->cancel(); // finished() says when it has stopped
        return;
    }
    if (m_running) {
        const auto answer = QMessageBox::warning(this, windowTitle(),
                                                 tr("Stop writing? The drive won't be usable until you format it again."),
                                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        if (auto *w = qobject_cast<ImageWriter *>(m_worker))
            w->cancel();
        return; // finished() closes the dialog
    }
    disconnect(m_openConn);
    QDialog::reject();
}

void WriteImageDialog::startCopy()
{
    const Disk *d = target();
    if (!d)
        return;
    // Read before the widgets are disabled (a disabled box disables what's in it).
    const quint64 persistence = m_persist->isEnabled() && m_persist->isChecked() ? quint64(m_persistGb->value()) * 1000000000ULL : 0;
    m_running = true;
    for (QWidget *w : std::initializer_list<QWidget *>{m_image, m_targets, m_sha, m_verify, m_confirm, m_write, m_modeBox})
        w->setEnabled(false);
    m_isoCopy = new IsoCopy(m_udisks, d->blockPath, m_image->text().trimmed(), m_analysis, persistence, checksums::parse(m_sha->text()), this);
    auto *meter = new PhaseProgress(m_progress, m_phase);
    connect(m_isoCopy, &IsoCopy::phase, this, [this](const QString &text) {
        m_progress->setRange(0, 0);
        m_phase->setText(text);
    });
    connect(m_isoCopy, &IsoCopy::progress, this, [this, meter](const QString &phase, quint64 done, quint64 total) {
        if (m_progress->maximum() == 0)
            m_progress->setRange(0, 1000);
        meter->update(phase, done, total);
    });
    connect(m_isoCopy, &IsoCopy::finished, this, [this, meter](bool ok, const QString &message, bool shownAlready) {
        delete meter;
        m_isoCopy->deleteLater();
        m_isoCopy = nullptr;
        m_running = false;
        m_udisks->refresh();
        if (ok) {
            QMessageBox::information(this, windowTitle(), message);
            QDialog::accept();
            return;
        }
        if (!shownAlready)
            QMessageBox::warning(this, windowTitle(), message);
        m_progress->setVisible(false);
        m_phase->setText(redText(message));
        m_phase->setTextFormat(Qt::RichText);
        m_confirm->clear();
        for (QWidget *w : std::initializer_list<QWidget *>{m_image, m_targets, m_sha, m_verify, m_confirm, m_modeBox})
            w->setEnabled(true);
        updateState();
    });
    m_progress->setRange(0, 0);
    m_progress->setVisible(true);
    m_isoCopy->start();
}

void WriteImageDialog::start()
{
    const Disk *d = target();
    if (!d)
        return;
    if (m_isIso && m_copyMode->isChecked())
        return startCopy();
    const Disk disk = *d;
    const QString image = m_image->text().trimmed();
    const checksums::Expected sha = checksums::parse(m_sha->text());
    const bool verify = m_verify->isChecked();

    m_running = true;
    for (QWidget *w : std::initializer_list<QWidget *>{m_image, m_targets, m_sha, m_verify, m_confirm, m_write})
        w->setEnabled(false);
    m_phase->setText(tr("Waiting for permission…"));

    m_openConn = connect(m_udisks, &UDisks::deviceOpened, this, [this, disk, image, sha, verify](const QString &path, int fd) {
        if (path != disk.blockPath)
            return;
        disconnect(m_openConn);
        if (fd < 0) {
            m_running = false;
            for (QWidget *w : std::initializer_list<QWidget *>{m_image, m_targets, m_sha, m_verify, m_confirm})
                w->setEnabled(true);
            m_phase->setText(tr("Couldn't open the drive."));
            updateState();
            return;
        }
        auto *writer = new ImageWriter(image, fd, verify, sha);
        m_worker = writer;
        m_thread = new QThread(this);
        writer->moveToThread(m_thread);
        connect(m_thread, &QThread::started, writer, &ImageWriter::run);
        connect(m_thread, &QThread::finished, writer, &QObject::deleteLater);

        // Speed and time left are per phase (checking, writing, verifying).
        auto *meter = new PhaseProgress(m_progress, m_phase);
        connect(writer, &ImageWriter::progress, this, [meter](const QString &phase, quint64 done, quint64 total) {
            meter->update(phase, done, total);
        });
        connect(writer, &ImageWriter::finished, this, [this, meter](bool ok, const QString &message) {
            qCInfo(lcOps).noquote() << "Write Image" << (ok ? "finished:" : "failed:") << message;
            delete meter;
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_worker = nullptr;
            m_running = false;
            m_udisks->refresh();
            if (ok) {
                QMessageBox::information(this, windowTitle(), message);
                QDialog::accept();
            } else {
                QMessageBox::warning(this, windowTitle(), message);
                QDialog::reject();
            }
        });
        m_progress->setValue(0);
        m_progress->setVisible(true);
        m_thread->start();
    });
    m_udisks->openDevice(disk, true, false);
}

// --- Bad sectors --------------------------------------------------------------

BadSectorsDialog::BadSectorsDialog(UDisks *udisks, const Disk &disk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_disk(disk)
    , m_status(new QLabel)
    , m_progress(new QProgressBar)
    , m_map(new BlockMapWidget)
    , m_found(new QTreeWidget)
    , m_repairNote(new QLabel)
    , m_scan(new QPushButton(tr("Start Scan")))
    , m_repair(new QPushButton(tr("Repair…")))
    , m_refresh(new QPushButton(tr("Refresh Slow Areas…")))
{
    setWindowTitle(tr("Bad Sectors on %1").arg(disk.model));
    m_refresh->setObjectName(QStringLiteral("refresh"));
    m_refresh->setToolTip(tr("Experimental: an SSD's old data can get slow to read as it fades. Writing the same data back makes it fresh."));
    m_refresh->setVisible(false);
    connect(m_refresh, &QPushButton::clicked, this, &BadSectorsDialog::startRefresh);
    const bool slow = disk.rotationRate > 0 || disk.bus == QLatin1String("usb") || disk.removable;
    QString intro = tr("<p><b>%1</b></p><p>Reads every sector of the drive to find ones that can't be read. "
                       "Scanning doesn't change anything and takes %2. You can keep using the PC meanwhile.</p>")
                        .arg(diskTitle(disk).toHtmlEscaped(), durationText(double(disk.size) / (slow ? 70e6 : 450e6)));
    if (disk.health.pendingSectors > 0)
        intro += tr("<p>The drive reports %1 that it can't read. This scan finds them so they can be repaired.</p>")
                     .arg(sectors(disk.health.pendingSectors));
    else if (disk.health.reallocatedSectors > 0)
        intro += tr("<p>The drive has already swapped %1 for spares; those are handled. It doesn't report any unreadable "
                    "sectors right now, but a scan checks every sector to be sure.</p>")
                     .arg(sectors(disk.health.reallocatedSectors));

    m_progress->setRange(0, 1000);
    m_progress->setVisible(false);
    m_status->setWordWrap(true);
    m_map->reset(disk.size, disk.rotationRate > 0);
    QVector<BlockMapWidget::Area> areas;
    for (const Volume &v : disk.volumes) {
        if (!v.isContainer)
            areas.append({v.offset, v.offset + v.size, volumeTitle(v)});
    }
    m_map->setAreas(areas);
    m_map->setToolTip(QString()); // per-cell tooltips come from the widget
    m_found->setHeaderLabels({tr("Sector"), tr("Position"), tr("Partition")});
    m_found->setRootIsDecorated(false);
    m_found->setVisible(false);
    m_repairNote->setWordWrap(true);
    m_repairNote->setText(tr("Repair rewrites only these sectors. Anything the drive can still read is kept; the rest was "
                             "already lost and becomes zeros. The drive then reuses each spot or swaps in a spare. "
                             "If new bad sectors keep showing up, the drive is wearing out: plan to replace it."));
    m_repairNote->setVisible(false);
    m_repair->setVisible(false);

    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &BadSectorsDialog::reject);
    connect(m_scan, &QPushButton::clicked, this, [this] {
        if (m_running)
            stop();
        else
            startScan();
    });
    connect(m_repair, &QPushButton::clicked, this, &BadSectorsDialog::startRepair);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_repair);
    buttons->addWidget(m_refresh);
    buttons->addStretch();
    buttons->addWidget(m_scan);
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(intro));
    layout->addWidget(m_progress);
    layout->addWidget(m_status);
    layout->addWidget(m_map, 2);
    layout->addWidget(m_found, 1);
    layout->addWidget(m_repairNote);
    layout->addLayout(buttons);
    resize(680, 620);
}

BadSectorsDialog::~BadSectorsDialog()
{
    stop();
}

void BadSectorsDialog::stop()
{
    disconnect(m_openConn);
    if (auto *s = qobject_cast<SurfaceScan *>(m_worker))
        s->cancel(); // finished() then reports how far it got
    if (auto *r = qobject_cast<SlowRefresh *>(m_worker))
        r->cancel(); // after the piece it's on: it's never left half written
}

void BadSectorsDialog::reject()
{
    if (m_running && qobject_cast<SectorRepair *>(m_worker))
        return; // a repair is a few writes; let it finish
    stop();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
    QDialog::reject();
}

void BadSectorsDialog::setRunning(bool running)
{
    m_running = running;
    m_scan->setText(running ? tr("Stop") : tr("Scan Again"));
    m_repair->setEnabled(!running && !m_bad.isEmpty() && !m_disk.isSystem);
    m_refresh->setEnabled(!running && !m_disk.isSystem);
    m_refresh->setVisible(isSsd() && !m_slow.isEmpty());
}

bool BadSectorsDialog::isSsd() const
{
    return (m_disk.rotationRate == 0 && !m_disk.isLoop) || (m_disk.isLoop && loopIsSsdForTest);
}

void BadSectorsDialog::showScanForTest(const QVector<ReadSample> &samples)
{
    m_map->reset(m_disk.size, m_disk.rotationRate > 0);
    m_map->addSamples(samples);
    m_slowReads.clear();
    for (const ReadSample &s : samples) {
        if (s.ms >= 50)
            m_slowReads << s;
    }
    scanFinished(true, {}, 512);
}

void BadSectorsDialog::showFound()
{
    m_found->clear();
    for (quint64 offset : m_bad) {
        QString where = tr("Outside any partition");
        for (const Volume &v : m_disk.volumes) {
            if (!v.isContainer && offset >= v.offset && offset < v.offset + v.size)
                where = volumeTitle(v);
        }
        new QTreeWidgetItem(m_found, {QString::number(offset / quint64(m_logical)), formatSize(offset), where});
    }
    for (int c = 0; c < m_found->columnCount(); ++c)
        m_found->resizeColumnToContents(c);
    const bool any = !m_bad.isEmpty();
    m_found->setVisible(any);
    m_repairNote->setVisible(any);
    m_repair->setVisible(any);
    if (any && m_disk.isSystem)
        m_repairNote->setText(m_repairNote->text() + QStringLiteral("\n\n")
                              + tr("This is your system drive, so DiskForge won't write to it. Repair it from a live USB instead."));
}

void BadSectorsDialog::startScan()
{
    m_bad.clear();
    showFound();
    setRunning(true);
    m_status->setText(tr("Waiting for permission…"));
    m_openConn = connect(m_udisks, &UDisks::deviceOpened, this, [this](const QString &path, int fd) {
        if (path != m_disk.blockPath)
            return;
        disconnect(m_openConn);
        if (fd < 0) {
            m_status->setText(tr("Couldn't open the drive."));
            setRunning(false);
            return;
        }
        auto *scan = new SurfaceScan(fd, m_disk.size);
        m_worker = scan;
        m_thread = new QThread(this);
        scan->moveToThread(m_thread);
        connect(m_thread, &QThread::started, scan, &SurfaceScan::run);
        connect(m_thread, &QThread::finished, scan, &QObject::deleteLater);
        auto *clock = new QElapsedTimer;
        clock->start();
        m_map->reset(m_disk.size, m_disk.rotationRate > 0);
        connect(scan, &SurfaceScan::samples, m_map, &BlockMapWidget::addSamples);
        // Kept: the reads slow enough that they might be slow for this drive (50 ms, an SSD's
        // floor); which really are is known once the whole drive's been read.
        m_slowReads.clear();
        m_slow.clear();
        connect(scan, &SurfaceScan::samples, this, [this](const QVector<ReadSample> &batch) {
            for (const ReadSample &s : batch) {
                if (s.ms >= 50 && s.readable && m_slowReads.size() < 100000)
                    m_slowReads << s;
            }
        });
        connect(scan, &SurfaceScan::progress, this, [this, clock](quint64 done, quint64 total, int bad) {
            m_progress->setValue(int(done * 1000 / std::max<quint64>(total, 1)));
            const double rate = done / std::max(clock->nsecsElapsed() / 1e9, 0.001);
            m_status->setText(tr("Scanned %1 of %2, %3/s, %4 left. Bad sectors so far: %5")
                                  .arg(formatSize(done), formatSize(total), formatSize(quint64(rate)),
                                       durationText((total - done) / std::max(rate, 1.0)))
                                  .arg(bad));
        });
        connect(scan, &SurfaceScan::finished, this, [this, clock](bool completed, const QVector<quint64> &bad, int logical) {
            qCInfo(lcOps).noquote() << "Bad sector scan" << (completed ? "finished," : "stopped,") << bad.size() << "bad";
            delete clock;
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_worker = nullptr;
            scanFinished(completed, bad, logical);
        });
        m_progress->setValue(0);
        m_progress->setVisible(true);
        m_thread->start();
    });
    m_udisks->openDevice(m_disk, false, true);
}

void BadSectorsDialog::scanFinished(bool completed, const QVector<quint64> &bad, int logical)
{
    m_bad = bad;
    m_logical = logical;
    m_progress->setVisible(false);
    // Only a whole drive's reads say what's slow for it.
    m_slow = completed ? slowRanges(m_slowReads, m_map->data()) : QVector<QPair<quint64, quint64>>();
    const int slow = m_map->data().slowAreas();
    QString slowNote = slow == 0 ? QString()
        : QStringLiteral(" ") + tr("%n area(s) read slowly. Slow spots often turn into bad sectors later, so keep a backup.", nullptr, slow);
    if (isSsd() && !m_slow.isEmpty())
        slowNote += QStringLiteral(" ") + tr("On an SSD, old data that has faded reads slowly too: Refresh Slow Areas writes it back fresh.");
    if (!completed)
        m_status->setText(tr("Stopped. Bad sectors found so far: %1").arg(bad.size()) + slowNote);
    else if (bad.isEmpty() && slow == 0)
        m_status->setText(tr("Done. Every sector reads fine."));
    else if (bad.isEmpty())
        m_status->setText(tr("Done. Every sector can be read.") + slowNote);
    else
        m_status->setText(tr("Done. %1 can't be read.").arg(sectors(bad.size())) + slowNote);
    showFound();
    setRunning(false);
}

void BadSectorsDialog::startRefresh()
{
    if (m_slow.isEmpty() || m_disk.isSystem || m_running)
        return;
    quint64 bytes = 0;
    for (const auto &range : std::as_const(m_slow))
        bytes += range.second;
    QStringList mounted;
    for (const Volume &v : m_disk.volumes) {
        if (!v.mounts().isEmpty())
            mounted << volumeTitle(v);
    }
    QString question = tr("Refresh %1 that read slowly on %2?").arg(formatSize(bytes), diskTitle(m_disk));
    if (!mounted.isEmpty())
        question += QStringLiteral("\n\n") + tr("These get unmounted first: %1").arg(mounted.join(QStringLiteral(", ")));
    QMessageBox confirm(QMessageBox::Warning, tr("Refresh Slow Areas"), question, QMessageBox::Cancel, this);
    confirm.setInformativeText(tr("This is experimental. Each slow piece is read and written back unchanged, which makes faded data on an "
                                  "SSD fresh again, and it uses a little of the drive's writing life. If the power goes in the middle of "
                                  "a piece, that piece can be damaged: keep a backup, and on a laptop, plug it in. Pieces that can't be "
                                  "read are left alone (that's what Repair is for)."));
    QAbstractButton *go = confirm.addButton(tr("Refresh"), QMessageBox::AcceptRole);
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != go)
        return;

    setRunning(true);
    m_status->setText(tr("Waiting for permission…"));
    const QVector<QPair<quint64, quint64>> ranges = m_slow;
    m_openConn = connect(m_udisks, &UDisks::deviceOpened, this, [this, ranges](const QString &path, int fd) {
        if (path != m_disk.blockPath)
            return;
        disconnect(m_openConn);
        if (fd < 0) {
            m_status->setText(tr("Couldn't open the drive for writing."));
            setRunning(false);
            return;
        }
        auto *refresh = new SlowRefresh(fd, ranges);
        m_worker = refresh;
        m_thread = new QThread(this);
        refresh->moveToThread(m_thread);
        connect(m_thread, &QThread::started, refresh, &SlowRefresh::run);
        connect(m_thread, &QThread::finished, refresh, &QObject::deleteLater);
        connect(refresh, &SlowRefresh::progress, this, [this](quint64 done, quint64 total) {
            m_progress->setValue(int(done * 1000 / std::max<quint64>(total, 1)));
            m_status->setText(tr("Refreshed %1 of %2…").arg(formatSize(done), formatSize(total)));
        });
        connect(refresh, &SlowRefresh::finished, this, [this](const RefreshResult &r) {
            qCInfo(lcOps).noquote() << "Slow area refresh:" << r.bytes << "bytes," << r.unreadable << "unreadable, before" << r.beforeMs << "ms after"
                                    << r.afterMs << "ms" << r.error;
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_worker = nullptr;
            m_progress->setVisible(false);
            QString text = r.error.isEmpty() ? tr("Refreshed %1.").arg(formatSize(r.bytes)) : tr("%1, after %2.").arg(r.error, formatSize(r.bytes));
            if (r.beforeMs > 0)
                text += QLatin1Char(' ') + tr("A MiB took %1 ms to read before, and %2 ms after.")
                                               .arg(QLocale().toString(r.beforeMs, 'f', 1), QLocale().toString(r.afterMs, 'f', 1));
            if (r.unreadable > 0)
                text += QLatin1Char(' ') + tr("Not read, so left alone: %n piece(s). Scan again, then Repair.", nullptr, r.unreadable);
            m_status->setText(text);
            m_slow.clear();
            setRunning(false);
            QMessageBox::information(this, tr("Refresh Slow Areas"), text);
        });
        m_progress->setValue(0);
        m_progress->setVisible(true);
        m_thread->start();
    });
    m_udisks->openDevice(m_disk, true, false, true);
}

void BadSectorsDialog::startRepair()
{
    if (m_bad.isEmpty() || m_disk.isSystem)
        return;
    QStringList mounted;
    for (const Volume &v : m_disk.volumes) {
        if (!v.mounts().isEmpty())
            mounted << volumeTitle(v);
    }
    QString question = tr("Repair %n bad sector(s) on %1?", nullptr, int(m_bad.size())).arg(diskTitle(m_disk));
    if (!mounted.isEmpty())
        question += QStringLiteral("\n\n") + tr("These get unmounted first: %1").arg(mounted.join(QStringLiteral(", ")));
    QMessageBox confirm(QMessageBox::Warning, tr("Repair Bad Sectors"), question, QMessageBox::Cancel, this);
    confirm.setInformativeText(m_repairNote->text());
    QAbstractButton *go = confirm.addButton(tr("Repair"), QMessageBox::AcceptRole);
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != go)
        return;

    setRunning(true);
    m_scan->setEnabled(false);
    m_status->setText(tr("Waiting for permission…"));
    const QVector<quint64> bad = m_bad;
    m_openConn = connect(m_udisks, &UDisks::deviceOpened, this, [this, bad](const QString &path, int fd) {
        if (path != m_disk.blockPath)
            return;
        disconnect(m_openConn);
        if (fd < 0) {
            m_status->setText(tr("Couldn't open the drive for writing."));
            m_scan->setEnabled(true);
            setRunning(false);
            return;
        }
        auto *repair = new SectorRepair(fd, bad);
        m_worker = repair;
        m_thread = new QThread(this);
        repair->moveToThread(m_thread);
        connect(m_thread, &QThread::started, repair, &SectorRepair::run);
        connect(m_thread, &QThread::finished, repair, &QObject::deleteLater);
        connect(repair, &SectorRepair::progress, this, [this](int done, int total) {
            m_status->setText(tr("Rewriting block %1 of %2…").arg(done).arg(total));
        });
        connect(repair, &SectorRepair::finished, this, [this](const RepairResult &r) {
            qCInfo(lcOps).noquote() << "Sector repair: rewrote" << r.blocks << "blocks," << r.recovered << "recovered,"
                                    << r.zeroed << "zeroed," << r.stillBad << "still bad" << r.error;
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_worker = nullptr;
            m_scan->setEnabled(true);
            QString text;
            if (!r.error.isEmpty()) {
                text = r.error;
            } else {
                text = tr("Rewrote %1. Still readable and kept: %2. Already lost, now zeros: %3.")
                           .arg(tr("%n block(s)", nullptr, r.blocks), sectors(r.recovered), sectors(r.zeroed));
                if (r.stillBad > 0)
                    text += QStringLiteral("\n\n") + tr("%1 still can't be read: the drive has run out of ways to fix them. "
                                                        "Copy off anything you need and replace it.").arg(sectors(r.stillBad));
                else
                    text += QStringLiteral("\n\n") + tr("Run Check for Errors on the partition so the file system catches up, then scan again to confirm.");
            }
            m_status->setText(text);
            m_bad.clear();
            showFound();
            setRunning(false);
            if (const Disk *d = m_udisks->diskByPath(m_disk.blockPath))
                m_udisks->smartUpdate(*d); // pending count should drop
            QMessageBox::information(this, tr("Repair Bad Sectors"), text);
        });
        m_thread->start();
    });
    m_udisks->openDevice(m_disk, true, false, true);
}
