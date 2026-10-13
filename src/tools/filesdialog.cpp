// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filesdialog.h"

#include "format.h"
#include "lookcloserdialog.h"
#include "udisks.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QProcess>
#include <QProgressDialog>
#include <QSplitter>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QToolBar>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <atomic>

namespace {

QIcon icon(const char *name, const char *fallback)
{
    return QIcon::fromTheme(QLatin1String(name), QIcon::fromTheme(QLatin1String(fallback)));
}

// Copies (or moves) files and folders into a folder, a piece at a time, so it can show how far it
// is and stop. A name that's already there gets " (2)" and so on: nothing is overwritten.
class Paster : public QThread
{
public:
    Paster(QStringList from, QString into, bool move) : m_from(std::move(from)), m_into(std::move(into)), m_move(move) {}
    std::atomic<bool> stop{false};
    std::atomic<qint64> done{0};
    qint64 total = 0;
    QString problem;
    QStringList renamed;

    void measure()
    {
        for (const QString &p : std::as_const(m_from)) {
            const QFileInfo info(p);
            if (info.isDir() && !info.isSymLink()) {
                QDirIterator it(p, QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    it.next();
                    total += it.fileInfo().size();
                }
            } else {
                total += info.size();
            }
        }
    }

protected:
    void run() override
    {
        for (const QString &p : std::as_const(m_from)) {
            if (stop)
                return;
            const QFileInfo info(p);
            QString target = free(m_into + QLatin1Char('/') + info.fileName());
            if (target != m_into + QLatin1Char('/') + info.fileName())
                renamed << QFileInfo(target).fileName();
            // On the same drive a move is just a new name.
            if (m_move && QStorageInfo(p).device() == QStorageInfo(m_into).device() && QFile::rename(p, target))
                continue;
            if (!copyItem(p, target))
                return;
            if (m_move && !stop) {
                if (info.isDir() && !info.isSymLink() ? !QDir(p).removeRecursively() : !QFile::remove(p))
                    problem = QObject::tr("%1 was copied, but couldn't be taken away from where it was.").arg(info.fileName());
            }
        }
    }

private:
    static QString free(const QString &path)
    {
        if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink())
            return path;
        const QFileInfo info(path);
        const QString base = info.isDir() ? info.fileName() : info.completeBaseName();
        const QString suffix = info.isDir() || info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
        for (int n = 2;; ++n) {
            const QString next = info.absolutePath() + QLatin1Char('/') + QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(suffix);
            if (!QFileInfo::exists(next))
                return next;
        }
    }

    bool copyItem(const QString &from, const QString &to)
    {
        const QFileInfo info(from);
        if (info.isSymLink())
            return QFile::link(info.symLinkTarget(), to) || fail(info.fileName());
        if (info.isDir()) {
            if (!QDir().mkpath(to))
                return fail(info.fileName());
            for (const QFileInfo &child : QDir(from).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
                if (stop || !copyItem(child.absoluteFilePath(), to + QLatin1Char('/') + child.fileName()))
                    return false;
            }
            return true;
        }
        QFile in(from), out(to);
        if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly))
            return fail(info.fileName(), in.isOpen() ? out.errorString() : in.errorString());
        QByteArray buffer(4 << 20, Qt::Uninitialized);
        while (!stop) {
            const qint64 n = in.read(buffer.data(), buffer.size());
            if (n < 0)
                return fail(info.fileName(), in.errorString());
            if (n == 0)
                break;
            if (out.write(buffer.constData(), n) != n)
                return fail(info.fileName(), out.errorString());
            done += n;
        }
        if (stop) {
            out.close();
            out.remove(); // half a file is no use
            return false;
        }
        out.flush();
        out.close();
        out.setPermissions(in.permissions());
        return true;
    }

    bool fail(const QString &name, const QString &why = {})
    {
        problem = why.isEmpty() ? QObject::tr("%1 couldn't be copied.").arg(name) : QObject::tr("%1 couldn't be copied: %2").arg(name, why);
        return false;
    }

    QStringList m_from;
    QString m_into;
    bool m_move;
};

bool isPictureOrFilm(const QString &path)
{
    const QString mime = QMimeDatabase().mimeTypeForFile(path).name();
    return (mime.startsWith(QLatin1String("image/")) && !mime.contains(QLatin1String("svg"))) || mime.startsWith(QLatin1String("video/"));
}

} // namespace

FilesDialog::FilesDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_places(new QListWidget)
    , m_path(new QLineEdit)
    , m_view(new QTreeView)
    , m_model(new QFileSystemModel(this))
    , m_note(new QLabel)
{
    setWindowTitle(tr("Files"));
    resize(1000, 620);
    auto *layout = new QVBoxLayout(this);
    auto *bar = new QToolBar;
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_up = bar->addAction(icon("go-up", "go-previous"), tr("Up"), this, [this] { go(QFileInfo(m_folder).absolutePath()); });
    m_up->setShortcut(QKeySequence(Qt::Key_Backspace));
    bar->addSeparator();
    m_newFolder = bar->addAction(icon("folder-new", "list-add"), tr("New Folder"), this, &FilesDialog::newFolder);
    m_copy = bar->addAction(icon("edit-copy", "edit-copy"), tr("Copy"), this, [this] { copyPaths(picked(), false); updateActions(); });
    m_copy->setShortcut(QKeySequence::Copy);
    m_cut = bar->addAction(icon("edit-cut", "edit-cut"), tr("Cut"), this, [this] { copyPaths(picked(), true); updateActions(); });
    m_cut->setShortcut(QKeySequence::Cut);
    m_paste = bar->addAction(icon("edit-paste", "edit-paste"), tr("Paste"), this, [this] { paste(); });
    m_paste->setShortcut(QKeySequence::Paste);
    m_rename = bar->addAction(icon("edit-rename", "document-edit"), tr("Rename"), this, &FilesDialog::rename);
    m_rename->setShortcut(QKeySequence(Qt::Key_F2));
    m_delete = bar->addAction(icon("edit-delete", "edit-delete"), tr("Delete"), this, &FilesDialog::remove);
    m_delete->setShortcut(QKeySequence::Delete);
    bar->addSeparator();
    m_look = bar->addAction(icon("zoom-in", "view-preview"), tr("Look Closer"), this, &FilesDialog::openPicked);
    m_terminal = bar->addAction(icon("utilities-terminal", "utilities-terminal"), tr("Terminal Here"), this, [this] {
        QProcess::startDetached(QStringLiteral("qterminal"), {QStringLiteral("--workdir"), m_folder});
    });
    m_terminal->setVisible(!QStandardPaths::findExecutable(QStringLiteral("qterminal")).isEmpty());
    for (QAction *a : bar->actions())
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    layout->addWidget(bar);
    m_path->setObjectName(QStringLiteral("path"));
    layout->addWidget(m_path);

    auto *split = new QSplitter;
    m_places->setObjectName(QStringLiteral("places"));
    m_places->setIconSize(QSize(22, 22));
    split->addWidget(m_places);
    m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    m_model->setReadOnly(true);
    m_view->setModel(m_model);
    m_view->setObjectName(QStringLiteral("files"));
    m_view->setRootIsDecorated(false);
    m_view->setItemsExpandable(false);
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(0, Qt::AscendingOrder);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    split->addWidget(m_view);
    split->setStretchFactor(1, 1);
    split->setSizes({240, 760});
    layout->addWidget(split, 1);
    m_note->setWordWrap(true);
    layout->addWidget(m_note);

    connect(m_path, &QLineEdit::returnPressed, this, [this] { go(m_path->text()); });
    connect(m_view, &QTreeView::activated, this, [this](const QModelIndex &index) {
        const QString path = m_model->filePath(index);
        if (QFileInfo(path).isDir())
            go(path);
        else
            openPicked();
    });
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &FilesDialog::updateActions);
    connect(m_view, &QTreeView::customContextMenuRequested, this, [this](const QPoint &at) {
        QMenu menu(this);
        menu.addActions({m_look, m_copy, m_cut, m_paste, m_rename, m_delete});
        menu.addSeparator();
        menu.addActions({m_newFolder, m_terminal});
        menu.exec(m_view->viewport()->mapToGlobal(at));
    });
    connect(m_places, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const QString path = item->data(Qt::UserRole).toString();
        const QString objectPath = item->data(Qt::UserRole + 1).toString();
        if (!path.isEmpty()) {
            go(path);
            return;
        }
        // A drive that isn't open yet: opened (mounted), then shown.
        for (const Disk &d : m_udisks->disks()) {
            for (const Volume &v : d.volumes) {
                if (v.objectPath == objectPath) {
                    m_note->setText(tr("Opening %1…").arg(v.label.isEmpty() ? v.device : v.label));
                    m_opening = objectPath;
                    m_udisks->mount(v);
                }
            }
        }
    });
    connect(m_places, &QListWidget::itemClicked, m_places, &QListWidget::itemActivated);
    connect(m_udisks, &UDisks::changed, this, [this] {
        fillPlaces();
        openedNow();
    });
    connect(m_udisks, &UDisks::operationFinished, this, [this](bool ok, const QString &message) {
        if (!ok)
            m_note->setText(message);
        fillPlaces();
        openedNow();
    });
    fillPlaces();
    go(QDir::homePath());
}

void FilesDialog::fillPlaces()
{
    const QString selected = m_places->currentItem() ? m_places->currentItem()->data(Qt::UserRole + 1).toString() + m_places->currentItem()->text() : QString();
    m_places->clear();
    auto *home = new QListWidgetItem(icon("user-home", "folder"), tr("Home"));
    home->setData(Qt::UserRole, QDir::homePath());
    m_places->addItem(home);
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (v.effectiveFsType().isEmpty() || v.effectiveFsType() == QLatin1String("swap") || v.effectiveFsType() == QLatin1String("crypto_LUKS"))
                continue;
            const QStringList mounts = v.mounts();
            const QString name = (v.label.isEmpty() ? QFileInfo(v.device).fileName() : v.label) + QStringLiteral("  ·  ") + formatSize(v.size);
            auto *item = new QListWidgetItem(icon(d.removable ? "drive-removable-media-usb" : "drive-harddisk", "drive-harddisk"),
                                             mounts.isEmpty() ? tr("%1 (not open)").arg(name) : name);
            item->setData(Qt::UserRole, mounts.isEmpty() ? QString() : mounts.first());
            item->setData(Qt::UserRole + 1, v.objectPath);
            item->setToolTip(v.device + (mounts.isEmpty() ? tr(": click to open it") : QStringLiteral(": ") + mounts.first()));
            m_places->addItem(item);
            if (item->data(Qt::UserRole + 1).toString() + item->text() == selected)
                m_places->setCurrentItem(item);
        }
    }
}

void FilesDialog::openedNow()
{
    // The drive asked to be opened: shown once it is.
    if (m_opening.isEmpty())
        return;
    for (int i = 0; i < m_places->count(); ++i) {
        QListWidgetItem *item = m_places->item(i);
        if (item->data(Qt::UserRole + 1).toString() == m_opening && !item->data(Qt::UserRole).toString().isEmpty()) {
            m_opening.clear();
            m_places->setCurrentItem(item);
            go(item->data(Qt::UserRole).toString());
            return;
        }
    }
}

void FilesDialog::go(const QString &folder)
{
    const QFileInfo info(folder);
    if (!info.isDir()) {
        m_note->setText(tr("%1 isn't a folder here.").arg(folder));
        m_path->setText(m_folder);
        return;
    }
    m_folder = info.absoluteFilePath();
    m_path->setText(m_folder);
    m_view->setRootIndex(m_model->setRootPath(m_folder));
    m_up->setEnabled(m_folder != QLatin1String("/"));
    const bool writable = QFileInfo(m_folder).isWritable() && !QStorageInfo(m_folder).isReadOnly();
    m_note->setText(writable ? QString() : tr("This folder can't be changed here (the drive is read-only or write-protected): files can be copied from it, not into it."));
    updateActions();
}

QStringList FilesDialog::picked() const
{
    QStringList out;
    for (const QModelIndex &i : m_view->selectionModel()->selectedRows())
        out << m_model->filePath(i);
    return out;
}

void FilesDialog::updateActions()
{
    const bool writable = QFileInfo(m_folder).isWritable() && !QStorageInfo(m_folder).isReadOnly();
    const QStringList sel = picked();
    m_newFolder->setEnabled(writable);
    m_paste->setEnabled(writable && !m_clipboard.isEmpty());
    m_cut->setEnabled(writable && !sel.isEmpty());
    m_rename->setEnabled(writable && sel.size() == 1);
    m_delete->setEnabled(writable && !sel.isEmpty());
    m_copy->setEnabled(!sel.isEmpty());
    m_look->setEnabled(std::any_of(sel.cbegin(), sel.cend(), isPictureOrFilm));
}

bool FilesDialog::ask(const QString &question)
{
    if (m_answerYes)
        return true;
    return QMessageBox::question(this, windowTitle(), question) == QMessageBox::Yes;
}

void FilesDialog::newFolder()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New Folder"), tr("Its name:"), QLineEdit::Normal, tr("New folder"), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    if (name.contains(QLatin1Char('/')) || !QDir(m_folder).mkdir(name))
        m_note->setText(tr("The folder couldn't be made."));
}

void FilesDialog::rename()
{
    const QStringList sel = picked();
    if (sel.size() != 1)
        return;
    const QFileInfo info(sel.first());
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename"), tr("New name:"), QLineEdit::Normal, info.fileName(), &ok).trimmed();
    if (!ok || name.isEmpty() || name == info.fileName())
        return;
    if (name.contains(QLatin1Char('/')) || QFileInfo::exists(info.absolutePath() + QLatin1Char('/') + name)
        || !QFile::rename(info.absoluteFilePath(), info.absolutePath() + QLatin1Char('/') + name))
        m_note->setText(tr("It couldn't be renamed (is there one of that name already?)."));
}

void FilesDialog::remove()
{
    const QStringList sel = picked();
    if (sel.isEmpty())
        return;
    if (!ask(tr("Delete %n item(s) for good? There's no recycle bin here.", nullptr, int(sel.size()))))
        return;
    int failed = 0;
    for (const QString &p : sel) {
        const QFileInfo info(p);
        if (info.isDir() && !info.isSymLink() ? !QDir(p).removeRecursively() : !QFile::remove(p))
            ++failed;
    }
    m_note->setText(failed ? tr("%n item(s) couldn't be deleted.", nullptr, failed) : QString());
}

bool FilesDialog::paste()
{
    if (m_clipboard.isEmpty())
        return false;
    // Not into itself.
    for (const QString &p : std::as_const(m_clipboard)) {
        if (m_folder == p || m_folder.startsWith(p + QLatin1Char('/'))) {
            m_note->setText(tr("A folder can't be copied into itself."));
            return false;
        }
    }
    auto *paster = new Paster(m_clipboard, m_folder, m_move);
    paster->measure();
    if (QStorageInfo(m_folder).bytesAvailable() < paster->total && !(m_move && QStorageInfo(m_folder).device() == QStorageInfo(m_clipboard.first()).device())) {
        m_note->setText(tr("There isn't room here: it needs %1 and there's %2 free.")
                            .arg(formatSize(quint64(paster->total)), formatSize(quint64(QStorageInfo(m_folder).bytesAvailable()))));
        delete paster;
        return false;
    }
    QProgressDialog wait(m_move ? tr("Moving…") : tr("Copying…"), tr("Stop"), 0, 1000, this);
    wait.setWindowModality(Qt::WindowModal);
    wait.setMinimumDuration(400);
    connect(&wait, &QProgressDialog::canceled, this, [paster] { paster->stop = true; });
    paster->start();
    while (!paster->wait(100)) {
        if (paster->total > 0)
            wait.setValue(int(paster->done * 1000 / paster->total));
        QApplication::processEvents();
    }
    wait.reset();
    const bool ok = paster->problem.isEmpty() && !paster->stop;
    m_note->setText(paster->stop ? tr("Stopped. What was copied before then is there.")
                    : !paster->problem.isEmpty() ? paster->problem
                    : paster->renamed.isEmpty() ? (m_move ? tr("Moved.") : tr("Copied."))
                    : tr("Copied. Some had the same names as files already here, so they're called %1.").arg(paster->renamed.join(QStringLiteral(", "))));
    if (m_move && ok)
        m_clipboard.clear();
    delete paster;
    updateActions();
    return ok;
}

void FilesDialog::openPicked()
{
    QStringList looks;
    for (const QString &p : picked()) {
        if (isPictureOrFilm(p))
            looks << p;
    }
    if (!looks.isEmpty()) {
        auto *look = new LookCloserDialog(this);
        look->setAttribute(Qt::WA_DeleteOnClose);
        look->add(looks);
        look->show();
        look->look(0);
        return;
    }
    const QStringList sel = picked();
    if (sel.size() == 1 && !QDesktopServices::openUrl(QUrl::fromLocalFile(sel.first())))
        m_note->setText(tr("Nothing here opens %1 files: copy it to a PC to open it.").arg(QFileInfo(sel.first()).suffix()));
}
