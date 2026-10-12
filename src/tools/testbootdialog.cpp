// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "testbootdialog.h"

#include "applog.h"
#include "blockio.h"
#include "dialogs.h"
#include "format.h"
#include "jobui.h"
#include "testboot.h"
#include "vdisk.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

bool TestBootDialog::looksUefi(const QByteArray &first)
{
    // GPT says "EFI PART" at sector 1 (512- or 4096-byte sectors); an MBR can have an EFI
    // partition (type 0xEF) too.
    if (first.mid(512, 8) == "EFI PART" || first.mid(4096, 8) == "EFI PART")
        return true;
    for (int i = 0; i < 4 && first.size() >= 512; ++i) {
        if (quint8(first[446 + i * 16 + 4]) == 0xEF)
            return true;
    }
    return false;
}

TestBootDialog::TestBootDialog(UDisks *udisks, const Disk *disk, const QString &image, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_image(image)
    , m_uefi(new QRadioButton(tr("UEFI (PCs from about 2012 on)")))
    , m_bios(new QRadioButton(tr("BIOS (the old way)")))
    , m_memory(new QSpinBox)
    , m_note(new QLabel)
    , m_start(new QPushButton(tr("Start")))
{
    setWindowTitle(tr("Try Starting It"));
    if (disk) {
        m_disk = *disk;
        m_isDisk = true;
    }
    const QString what = m_isDisk ? diskTitle(m_disk) : QFileInfo(image).fileName();
    // UEFI or BIOS, guessed from its partition table.
    bool uefi = false;
    if (m_isDisk) {
        uefi = m_disk.tableType == QLatin1String("gpt");
        for (const Volume &v : m_disk.volumes)
            uefi = uefi || v.isEfi;
    } else if (const auto source = vdisk::formatOf(image).isEmpty() ? lost::Source::fromImage(image, nullptr) : vdisk::open(image, nullptr)) {
        QByteArray first(8192, '\0');
        source->read(0, first.data(), quint64(first.size()));
        uefi = looksUefi(first);
    }
    (uefi ? m_uefi : m_bios)->setChecked(true);
    m_memory->setRange(512, 16384);
    m_memory->setSingleStep(512);
    m_memory->setValue(2048);
    m_memory->setSuffix(tr(" MB"));
    m_note->setWordWrap(true);
    m_note->setObjectName(QStringLiteral("note"));
    m_start->setObjectName(QStringLiteral("start"));

    QStringList notes;
    if (testboot::qemu().isEmpty()) {
        notes << redText(tr("It needs QEMU, which isn't on this computer: install it (qemu-system-x86_64, and OVMF for UEFI)."));
        m_start->setEnabled(false);
    } else if (!testboot::kvm()) {
        notes << tr("This PC's virtualization can't be used here, so it's emulated: expect it to be slow.");
    }
    bool mounted = false;
    for (const Volume &v : m_disk.volumes)
        mounted = mounted || !v.mounts().isEmpty();
    if (mounted)
        notes << tr("Some of its partitions are in use here: what starts in the window may look damaged. The drive itself isn't changed.");
    m_note->setText(notes.join(QStringLiteral("<br>")));

    auto *firmware = new QVBoxLayout;
    firmware->addWidget(m_uefi);
    firmware->addWidget(m_bios);
    auto *form = new QFormLayout;
    form->addRow(tr("Start it as:"), firmware);
    form->addRow(tr("Memory:"), m_memory);
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_start, &QPushButton::clicked, this, &TestBootDialog::start);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_start);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("<b>%1</b><br>Starts it in a window of its own (a virtual machine), to see that it starts: a "
                                       "clone or a backup before relying on it, say. Nothing it does is kept: what it writes goes to "
                                       "a temporary file, and the %2 is only read.")
                                        .arg(what.toHtmlEscaped(), m_isDisk ? tr("drive") : tr("image"))));
    layout->addLayout(form);
    layout->addWidget(m_note);
    layout->addLayout(buttons);
    resize(520, sizeHint().height());
}

void TestBootDialog::start()
{
    testboot::Options o;
    o.uefi = m_uefi->isChecked();
    o.memoryMiB = m_memory->value();
    const auto launch = [this](testboot::Options o, int fd) {
        o.fd = fd;
        if (!serialForTest.isEmpty()) {
            o.serialLog = serialForTest;
            o.window = false;
        }
        QString error;
        // Its own process, which outlives this window: the drive's handle closes when it ends.
        QProcess *qemu = testboot::start(o, m_udisks, &error);
        if (!qemu) {
            if (fd >= 0)
                blockio::close(fd);
            m_note->setText(redText(error.toHtmlEscaped()));
            return;
        }
        qCInfo(lcOps).noquote() << "Try Starting It:" << (fd >= 0 ? m_disk.device : m_image) << (o.uefi ? "UEFI" : "BIOS");
        // QEMU stopping straight away with an error says why here, while the window's open.
        QPointer<QLabel> note = m_note;
        auto *clock = new QElapsedTimer;
        clock->start();
        connect(qemu, &QProcess::finished, qemu, [qemu, fd, note, clock](int code) {
            if (fd >= 0)
                blockio::close(fd);
            const QString said = QString::fromLocal8Bit(qemu->readAllStandardError()).trimmed();
            if (note && code != 0 && clock->elapsed() < 15000 && !said.isEmpty())
                note->setText(redText(tr("It didn't start: %1").arg(said.right(400)).toHtmlEscaped()));
            qCInfo(lcOps).noquote() << "Try Starting It: QEMU ended" << code << said.right(400);
            delete clock;
            qemu->deleteLater();
        });
        m_note->setText(tr("Started: it's in its own window. Close that window when you're done."));
    };
    if (!m_isDisk) {
        o.file = m_image;
        o.format = testboot::formatOf(m_image);
        return launch(o, -1);
    }
    o.format = QStringLiteral("raw");
#ifdef Q_OS_WIN
    // QEMU, started from DiskAnvil, is an administrator too, and reads the drive by its name.
    o.file = m_disk.blockPath;
    launch(o, -1);
#else
    // Opened read-only as it is: nothing is unmounted.
    m_note->setText(tr("Waiting for permission…"));
    openBlockThen(m_udisks, this, m_disk.blockPath, UDisks::OpenMode::Peek, [this, launch, o](int fd) {
        if (fd >= 0)
            launch(o, fd);
        else
            m_note->setText(redText(tr("The drive couldn't be opened to read it.")));
    });
#endif
}
