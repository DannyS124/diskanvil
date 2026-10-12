// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jobui.h"
#include "lostscan.h"

#include <QAbstractListModel>
#include <QDialog>
#include <QHash>
#include <QPixmap>
#include <QSet>

#include <memory>
#include <optional>

class QCheckBox;
class QComboBox;
class QToolButton;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QProcess;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QThread;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class PreviewLoader;
namespace lost {
class LookCopies;
class ReadThrough;
}
class UDisks;

// Which of the found files are shown.
struct LostFilter {
    enum class Order { Found, Name, Date, Size, Kind };
    int category = -1;       // a filetypes::Category, -1: every kind
    bool hideDamaged = false;
    bool deletedOnly = false;
    bool hideSmallPictures = false; // icons and thumbnails: pictures smaller than kSmallPicture
    bool hideCopies = false;        // files found to be copies of another (Find Copies)
    bool hideBroken = false;        // files found not to open (Check They Open)
    Order order = Order::Found;
    QString folder;          // and what's in it, or kFoundByContents or kDeletedRecently; empty: everywhere
    QString text;

    static inline const QString kFoundByContents = QStringLiteral("\x01contents");
    static inline const QString kDeletedRecently = QStringLiteral("\x01recent"); // what's known to have been deleted when, newest first
    static constexpr quint64 kSmallPicture = 32 * 1024;
};

// What Find Lost Files found, filtered by kind, folder and what's typed, with a tick box each and
// a thumbnail for pictures (asked for only when a picture is on screen).
class LostFilesModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit LostFilesModel(QObject *parent = nullptr);

    void append(const QVector<lost::Found> &files);
    void clear();
    void setFilter(const LostFilter &filter);
    const lost::Found &file(int index) const { return m_files[index]; }
    const QVector<lost::Found> &files() const { return m_files; }
    int fileCount() const { return int(m_files.size()); }
    int fileIndex(int row) const { return m_shown.value(row, -1); }
    int rowOf(int index) const; // -1 when it isn't shown
    int count(int category) const; // how many of a kind were found
    void setThumbnail(int index, const QPixmap &thumbnail);
    // Its thumbnail was never made (the request was let go): ask again when it's shown.
    void forgetAsked(int index) { m_asked.remove(index); }
    QVector<lost::Found> ticked() const;
    QVector<int> tickedIndexes() const; // in the order ticked() has them
    int tickedCount() const { return int(m_ticked.size()); }
    // Where a file was saved to, once it has been (it stays in the list, marked).
    void markSaved(int index, const QString &path);
    QString savedTo(int index) const { return m_saved.value(index); }
    quint64 tickedSize() const;
    void tickShown(bool tick);
    // Copies (lost::CopyFinder): file index → the one it's a copy of.
    void setCopies(const QHash<int, int> &copyOf);
    int copyOf(int index) const { return m_copyOf.value(index, -1); }
    int copyCount() const { return int(m_copyOf.size()); }
    // Whether a file opened when it was checked (Check They Open).
    void setOpens(int index, bool opens);
    bool checkedOpen(int index) const { return m_opens.contains(index); }
    bool opens(int index) const { return m_opens.value(index, true); }
    int brokenCount() const;
    // The date a file is put in order by: the file system's, else the one it carries itself.
    static qint64 dateOf(const lost::Found &f) { return f.modified > 0 ? f.modified : f.inside; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

Q_SIGNALS:
    void wantThumbnail(int index) const;
    void tickedChanged();

private:
    bool passes(int index) const;
    bool before(int a, int b) const; // in the order the filter asks for
    bool inFoundOrder() const { return m_filter.order == LostFilter::Order::Found && m_filter.folder != LostFilter::kDeletedRecently; }
    void indexRows();
    // Takes rows out of the list, keeping what's picked.
    void removeShown(int index);

    QVector<lost::Found> m_files;
    void sortShown();

    QVector<int> m_shown; // file indexes in the list, in order (newest deleted first for kDeletedRecently)
    QSet<int> m_ticked;
    QHash<int, QString> m_saved;
    QHash<int, QPixmap> m_thumbnails;
    mutable QSet<int> m_asked;
    LostFilter m_filter;
    QHash<int, int> m_copyOf;
    QHash<int, bool> m_opens;
    QHash<int, int> m_rowOf; // file index → row, while the list isn't in the order found
};

// Find Lost Files: pick a drive, a partition or a disk image, look through it for files that
// were deleted or that its file system lost, look at them, and save the ones you want onto
// another drive. Only ever reads the drive it looks through.
class LostFilesDialog : public QDialog
{
    Q_OBJECT
public:
    // preferredPath: a drive or partition (UDisks object path) to have picked already.
    LostFilesDialog(UDisks *udisks, const QString &preferredPath, QWidget *parent = nullptr);
    ~LostFilesDialog() override;

    // Looks through a disk image file straight away.
    void scanImage(const QString &path);
    // What was found (for the timeline, and the tests).
    const QVector<lost::Found> &files() const { return m_model->files(); }
    // Looks through a source that's already open (and the tests' way in). diskDevice is the
    // drive it's on (/dev/sdb), so saving there can be refused; empty for an image.
    void scanSource(std::shared_ptr<lost::Source> source, const QString &title, const QString &diskDevice);
    // Saves the ticked files into `folder`, after the checks (the tests call this directly).
    void saveTo(const QString &folder);

protected:
    void reject() override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void fillDrives(const QString &preferredPath);
    void driveChosen();
    void start();
    void chooseImage();
    void filterChanged();
    void refreshCategories();
    // Adds the folders of new finds to the tree, with how many files are in each.
    void addFolders(const QVector<lost::Found> &files);
    void refreshFolderCounts();
    int chosenCategory() const;
    void startDeepScan(const QVector<lost::Extent> &knownRanges, const QVector<quint64> &knownStarts);
    // Windows' earlier copies of the drive (shadow copies): offered once its file systems are read.
    void offerCopies();
    void lookInCopies();
    void scanDone(bool stopped);
    void showPreview();
    void showPreviewResult(const QImage &image, const QString &text, const QStringList &facts, bool failed);
    // Rows of "label: value", the labels quieter, for the details.
    QString detailsTable(const QVector<QPair<QString, QString>> &rows) const;
    void updateTicked();
    void saveTicked();
    void saveSelected();
    // Saves files (indexes into the model) into `folder`, after the checks; the results stay.
    void saveFiles(const QVector<int> &indexes, const QString &folder);
    QVector<int> selectedIndexes() const;
    void showFilesMenu(const QPoint &at);
    // Organising: copies of the same file (lost::CopyFinder), and which files actually open
    // (each one through the preview helper, in its sandbox).
    void findCopies();
    // The small pictures in Windows' thumbnail caches among the files found, listed as files of their own.
    void findThumbnails();
    // Signs of ransomware among the files found (ransomware.h): a bar saying so, and what can be done.
    void lookForRansomware();
    void explainRansomware();
    // btrfs file systems in what's searched: their older file trees, through btrfs-progs (btrfsdialog.h).
    void offerBtrfs();
    void openBtrfs();
    // Copies part of a file to a file of its own (a partition out of a disk image), with progress.
    bool copyRange(const QString &from, quint64 offset, quint64 length, const QString &to, QString *why);
    // Finds text by words remembered from it (lost::WordSearch), all through the drive.
    void findByWords(const QString &words);
    void checkTheyOpen();
    void checkNext();
    void stopChecking();
    // Whether a preview says the file opened: true or false, or nothing when it can't be told.
    static std::optional<bool> opened(int type, const QImage &image, const QString &text, const QStringList &facts, bool failed);
    // A copy of a found file to look at, kept off the drive it's on (a progress window for a
    // big one). Empty when there's none; it's said why.
    QString lookCopy(int index);
    void openInProgram(int index);
    void dragOut();
    bool sourceMounted() const;
    // The viewer (viewer.h), a program of its own: started for the first file to view, then
    // handed the next ones.
    void openViewer(int index);
    void viewerShow(int index);
    void startViewer();
    void stopViewer();
    void sendToViewer(const QByteArray &message);
    void viewerSaid(const QByteArray &said);
    // The viewer's changed copy of a file, saved where it's asked to be (never on the drive searched).
    void saveChangedCopy(int index, const QMap<quint64, quint8> &changes);
    // Repairs a copy of a found file (repair.h); the repaired copy joins the list. `reference`: a
    // working file from the same device, when the repair asked for one.
    void repairCopy(int index, const QString &reference = {});
    // A PDF's earlier versions, still inside it, listed as files of their own.
    void earlierVersions(int index);
    // A report of what was found (foundreport.h), saved where the person says.
    void saveReport();
    // The fingerprints of the files picked, to copy.
    void showFingerprints(const QVector<int> &indexes);
    // The next or previous file in the list that has something to show, -1 when there's none.
    int neighbour(int index, int step) const;
    // The times the file system kept, as "label", "value" rows.
    QVector<QPair<QString, QString>> driveTimes(const lost::Found &f) const;
    QStringList factsFor(int index) const;

public:
    // For the tests: whether the viewer is running and has said it's ready, and what it shows.
    bool viewerReady() const { return m_viewerReady; }
    // For the tests: where the changed copy goes, instead of asking; and a message as if the
    // viewer had sent it.
    void setChangedCopyTarget(const QString &path) { m_changedCopyTarget = path; }
    // For the tests: a source read through into a Rescue Copy image, as for a drive with Keep a Copy ticked.
    void scanReadThrough(std::shared_ptr<lost::Source> drive, const QString &title, const QString &image);
    void hearFromViewer(const QByteArray &said) { viewerSaid(said); }
    void repairForTests(int index, const QString &reference = {}) { repairCopy(index, reference); }
    void findByWordsForTests(const QString &words) { findByWords(words); }
    bool searchingWords() const { return m_wordSearch != nullptr; }
    bool repairing() const { return m_repairing; }
    int viewerIndex() const { return m_viewerIndex; }
    int viewerShown() const { return m_viewerShown; } // the file it said it has on the screen, -1 for none

private:
    // Sized for the screen it opens on, or as it was left last time.
    void placeWindow();
    // How big a preview to ask for: as big as the room for it, within limits.
    int previewPixels() const;
    void fitPreview();
    void stopWorkers();

    UDisks *m_udisks;
    QStackedWidget *m_pages;
    // Picking
    QTreeWidget *m_drives;
    QLabel *m_driveNote;
    QCheckBox *m_keepCopy = nullptr;  // a Rescue Copy image of what's read (readthrough.h)
    QString m_copyImage;              // where it is, while it's being made
    lost::ReadThrough *m_through = nullptr; // the source's own layer, while there's one
    QPushButton *m_look;
    // Results
    QLabel *m_status;
    QProgressBar *m_bar;
    QLabel *m_phase;
    PhaseProgress m_meter;
    QPushButton *m_stop;
    QListWidget *m_categories;
    QTreeWidget *m_folders;
    QLineEdit *m_search;
    QCheckBox *m_hideDamaged;
    QCheckBox *m_deletedOnly;
    QComboBox *m_sort = nullptr;
    QCheckBox *m_hideSmall = nullptr;
    QCheckBox *m_hideCopies = nullptr;
    QCheckBox *m_hideBroken = nullptr;
    QToolButton *m_organise = nullptr; // a menu: Find Copies, Check They Open; says how they're doing
    QAction *m_findCopies = nullptr;
    QAction *m_checkOpen = nullptr;
    QAction *m_findWords = nullptr;
    lost::WordSearch *m_wordSearch = nullptr;
    QThread *m_wordThread = nullptr;
    QThread *m_thumbnailThread = nullptr;
    lost::CopyFinder *m_copyFinder = nullptr;
    QThread *m_copyFinderThread = nullptr;
    PreviewLoader *m_checker = nullptr;
    QThread *m_checkerThread = nullptr;
    QList<int> m_checkQueue;
    int m_checkWaiting = -1; // the file the helper is checking now
    int m_checkDone = 0, m_checkTotal = 0;
    QPushButton *m_grid;
    QListView *m_view;
    QLabel *m_previewImage;
    QLabel *m_details;
    QPlainTextEdit *m_hex;
    QSplitter *m_split = nullptr;
    QImage m_previewPicture; // the last one shown, fitted again when the room changes
    QString m_detailsHtml;   // what's known without the file's own facts, which come with its preview
    int m_previewAsked = 0;  // how big it was asked for
    QTimer *m_previewResize = nullptr;
    bool m_placed = false;
    std::unique_ptr<lost::LookCopies> m_looks;
    QPoint m_dragFrom;       // where a press on a picked file was, to drag it out from
    QPushButton *m_viewButton = nullptr;
    QProcess *m_viewer = nullptr;
    QByteArray m_viewerHeard;        // what it said that isn't read yet
    QList<QByteArray> m_viewerWaiting; // for it, until it's ready
    QStringList m_viewerReadable;    // the folders its sandbox may read
    bool m_viewerReady = false;
    bool m_viewerSandboxed = true;   // false once the sandbox turned out not to work here
    int m_viewerIndex = -1;
    int m_viewerShown = -1;
    QHash<int, QStringList> m_facts; // what each file said about itself, once its preview did
    bool m_warnedDrop = false;
    QString m_changedCopyTarget;
    bool m_repairing = false;
    QWidget *m_copiesBar = nullptr;
    QLabel *m_copiesText = nullptr;
    QPushButton *m_copiesLook = nullptr;
    QWidget *m_ransomBar = nullptr;
    QWidget *m_btrfsBar = nullptr;
    QString m_rawImage;     // the image file searched, when it's a plain one (not a VM's disk)
    QString m_sourceObject; // the drive or partition searched (UDisks object path)
    QLabel *m_ransomText = nullptr;
    QLabel *m_tickedInfo;
    QPushButton *m_save;
    // Saving, in a strip under the files: the results stay there for more
    QWidget *m_saveStrip = nullptr;
    QLabel *m_saveStatus;
    QProgressBar *m_saveBar;
    QLabel *m_savePhase;
    PhaseProgress m_saveMeter;
    QPushButton *m_openFolder;
    QPushButton *m_hideSaved;
    QString m_savedTo;

    LostFilesModel *m_model;
    std::shared_ptr<lost::Source> m_source;
    QString m_diskDevice;
    QString m_title;
    QString m_sourceWarning; // what's worth saying about where the files are (a disk file not closed properly)
    lost::QuickScan *m_quick = nullptr;
    QThread *m_quickThread = nullptr;
    lost::DeepScan *m_scan = nullptr;
    QThread *m_scanThread = nullptr;
    lost::CopyScan *m_copyScan = nullptr;
    QThread *m_copyThread = nullptr;
    QVector<lost::Volume> m_volumes;
    // Folder tree: an item per folder path, and how many files (and deleted ones) are in each.
    QHash<QString, QTreeWidgetItem *> m_folderItems;
    QHash<QString, int> m_inFolder, m_deletedInFolder;
    QTreeWidgetItem *m_contentsItem = nullptr;
    QTreeWidgetItem *m_recentItem = nullptr;
    lost::Saver *m_saver = nullptr;
    QThread *m_saveThread = nullptr;
    PreviewLoader *m_thumbs = nullptr;
    QThread *m_thumbThread = nullptr;
    int m_previewIndex = -1;
    bool m_scanning = false;
    bool m_saving = false;
};
