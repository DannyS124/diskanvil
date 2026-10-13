// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lookcloserdialog.h"

#include "filetypes.h"
#include "format.h"
#include "jobui.h"
#include "lostsource.h"
#include "placesmap.h"
#include "previewloader.h"
#include "viewer.h"
#include "viewerhost.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPushButton>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// What it's shown as: a picture or a film, by what it is (its type, or what the PC calls it).
// Anything else isn't for here.
viewer::Kind kindOf(const QString &path, int *type)
{
    *type = filetypes::typeForName(path);
    const viewer::Kind kind = viewer::kindFor(*type, QFileInfo(path).fileName());
    if (kind == viewer::Kind::Picture || kind == viewer::Kind::Film)
        return kind;
    const QString mime = QMimeDatabase().mimeTypeForFile(path).name();
    if (mime.startsWith(QLatin1String("video/")))
        return viewer::Kind::Film;
    if (mime.startsWith(QLatin1String("image/")) && !mime.contains(QLatin1String("svg")))
        return viewer::Kind::Picture;
    return viewer::Kind::Bytes;
}

} // namespace

LookCloserDialog::LookCloserDialog(QWidget *parent)
    : QDialog(parent)
    , m_list(new QListWidget)
    , m_note(new QLabel)
    , m_look(new QPushButton(tr("Look Closer")))
    , m_host(new ViewerHost(this))
{
    setWindowTitle(tr("Look Closer at Pictures and Films"));
    setAcceptDrops(true);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Any picture or film, to look closer at: Enhance and its profiles (Night camera, Number plate...), Pick a "
                                "Spot, Straighten, Stack Frames from a film, any speed, a frame at a time. Drop pictures, films or "
                                "folders here, or choose them. The files are only read: what's made is saved as new ones."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *choose = new QHBoxLayout;
    auto *files = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open")), tr("Choose Files…"));
    auto *folder = new QPushButton(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Choose a Folder…"));
    choose->addWidget(files);
    choose->addWidget(folder);
    choose->addStretch();
    layout->addLayout(choose);
    m_list->setObjectName(QStringLiteral("files"));
    m_list->setIconSize(QSize(24, 24));
    m_list->setUniformItemSizes(true);
    layout->addWidget(m_list, 1);
    m_note->setWordWrap(true);
    layout->addWidget(m_note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    m_look->setObjectName(QStringLiteral("look"));
    m_look->setEnabled(false);
    buttons->addButton(m_look, QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    resize(640, 520);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(files, &QPushButton::clicked, this, [this] {
        const QStringList picked = QFileDialog::getOpenFileNames(this, tr("Pictures and Films to Look Closer At"),
                                                                 QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
        const int before = int(m_files.size());
        if (!picked.isEmpty() && add(picked) > 0)
            look(before);
    });
    connect(folder, &QPushButton::clicked, this, [this] {
        const QString picked = QFileDialog::getExistingDirectory(this, tr("A Folder of Pictures or Films"),
                                                                 QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
        if (!picked.isEmpty())
            add({picked});
    });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) { m_look->setEnabled(row >= 0); });
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) { look(m_list->row(item)); });
    connect(m_look, &QPushButton::clicked, this, [this] { look(m_list->currentRow()); });
    connect(m_host, &ViewerHost::asked, this, &LookCloserDialog::asked);
    connect(m_host, &ViewerHost::status, m_note, &QLabel::setText);
    connect(m_host, &ViewerHost::closed, this, [this] { m_shown = -1; });
    m_note->setText(tr("Nothing yet: drop pictures or films here."));
}

LookCloserDialog::~LookCloserDialog()
{
    if (m_facts)
        m_facts->stop();
    if (m_factsThread) {
        m_factsThread->quit();
        m_factsThread->wait();
    }
    m_host->stop();
}

int LookCloserDialog::add(const QStringList &paths)
{
    QStringList found;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            const QFileInfoList inside = QDir(path).entryInfoList(QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
            for (const QFileInfo &file : inside)
                found << file.absoluteFilePath();
        } else if (info.isFile()) {
            found << info.absoluteFilePath();
        }
    }
    int added = 0, other = 0;
    const QFileIconProvider icons;
    for (const QString &path : std::as_const(found)) {
        int type = -1;
        const viewer::Kind kind = kindOf(path, &type);
        if (kind == viewer::Kind::Bytes) {
            ++other;
            continue;
        }
        if (m_files.contains(path))
            continue;
        m_files << path;
        auto *item = new QListWidgetItem(QIcon::fromTheme(kind == viewer::Kind::Film ? QStringLiteral("video-x-generic") : QStringLiteral("image-x-generic"),
                                                          icons.icon(QFileInfo(path))),
                                         QFileInfo(path).fileName());
        item->setToolTip(QDir::toNativeSeparators(path));
        m_list->addItem(item);
        ++added;
    }
    QString note = tr("%n picture(s) and film(s) to look at.", nullptr, int(m_files.size()));
    if (other > 0)
        note += QLatin1Char(' ') + tr("%n file(s) that aren't pictures or films left out.", nullptr, other);
    m_note->setText(note);
    if (m_list->currentRow() < 0 && m_list->count() > 0)
        m_list->setCurrentRow(0);
    return added;
}

QStringList LookCloserDialog::readableFolders() const
{
    QStringList folders;
    for (const QString &file : m_files) {
        const QString folder = QFileInfo(file).absolutePath();
        if (!folders.contains(folder))
            folders << folder;
    }
    return folders;
}

void LookCloserDialog::look(int row)
{
    if (row < 0 || row >= m_files.size())
        return;
    const QString path = m_files[row];
    const QFileInfo info(path);
    if (!info.isReadable()) {
        m_note->setText(tr("%1 can't be read any more.").arg(QDir::toNativeSeparators(path)));
        return;
    }
    viewer::Show show;
    int type = -1;
    show.kind = kindOf(path, &type);
    show.type = type;
    show.path = info.absoluteFilePath();
    show.name = info.fileName();
    show.where = tr("%1 of %2").arg(row + 1).arg(m_files.size());
    show.previous = row > 0;
    show.next = row + 1 < m_files.size();
    show.openable = true; // the person's own file, where it is
    show.from = tr("Look Closer");
    m_baseFacts = {tr("Size") + QLatin1Char('\t') + formatSize(quint64(info.size())), tr("In") + QLatin1Char('\t') + QDir::toNativeSeparators(info.absolutePath()),
                   tr("Changed") + QLatin1Char('\t') + QLocale().toString(info.lastModified(), QLocale::LongFormat)};
    if (info.birthTime().isValid())
        m_baseFacts << tr("Created") + QLatin1Char('\t') + QLocale().toString(info.birthTime(), QLocale::LongFormat);
    show.facts = m_baseFacts;
    m_shownFacts = m_baseFacts;
    m_shown = row;
    m_list->setCurrentRow(row);
    m_host->setSaveFolder(info.absolutePath());
    m_host->show(show, readableFolders());
    factsFor(row);
}

void LookCloserDialog::factsFor(int row)
{
    // What it says about itself (camera, where it was taken, its length...) from the preview
    // helper, in its sandbox like the viewer: the file is only read, and only there.
    if (m_facts)
        m_facts->stop();
    if (m_factsThread) {
        connect(m_factsThread, &QThread::finished, m_factsThread, &QObject::deleteLater);
        m_factsThread->quit();
    }
    QString error;
    const std::shared_ptr<lost::Source> source = lost::Source::fromImage(m_files[row], &error);
    if (!source)
        return;
    lost::Found file;
    file.name = QFileInfo(m_files[row]).fileName();
    file.type = filetypes::typeForName(file.name);
    file.size = quint64(QFileInfo(m_files[row]).size());
    file.extents = {{0, file.size}};
    auto *loader = new PreviewLoader(source);
    m_facts = loader;
    connect(loader, &PreviewLoader::ready, this, [this, row](int, int, const QImage &, const QString &, const QStringList &facts, bool) {
        if (row == m_shown) {
            m_shownFacts = m_baseFacts + facts;
            m_host->sendFacts(m_shownFacts);
        }
    });
    m_factsThread = startOnThread(this, loader);
    loader->want(row, file, 64);
}

void LookCloserDialog::asked(viewer::Ask ask)
{
    switch (ask) {
    case viewer::Ask::Previous:
        look(m_shown - 1);
        break;
    case viewer::Ask::Next:
        look(m_shown + 1);
        break;
    case viewer::Ask::Open:
        if (m_shown >= 0)
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_files[m_shown]));
        break;
    case viewer::Ask::Save:
        if (m_shown >= 0) {
            raise();
            activateWindow();
            const QString from = m_files[m_shown];
            const QString to = QFileDialog::getSaveFileName(this, tr("Save a Copy"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                                                                                         + QLatin1Char('/') + QFileInfo(from).fileName());
            if (!to.isEmpty() && QFileInfo(to).absoluteFilePath() != QFileInfo(from).absoluteFilePath()) {
                QFile::remove(to);
                m_note->setText(QFile::copy(from, to) ? tr("Saved a copy as %1.").arg(QDir::toNativeSeparators(to))
                                                      : tr("The copy couldn't be saved."));
            }
        }
        break;
    case viewer::Ask::Map:
        // Where it was taken, on OpenStreetMap (which asks first: it's going online).
        for (const QString &fact : std::as_const(m_shownFacts)) {
            double latitude = 0, longitude = 0;
            if (parseGeo(fact.section(QLatin1Char('\t'), 1), &latitude, &longitude)) {
                raise();
                activateWindow();
                openOnOpenStreetMap(this, latitude, longitude);
                break;
            }
        }
        break;
    default:
        break;
    }
}

void LookCloserDialog::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void LookCloserDialog::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths << url.toLocalFile();
    }
    event->acceptProposedAction();
    const int before = int(m_files.size());
    if (add(paths) > 0)
        look(before);
}
