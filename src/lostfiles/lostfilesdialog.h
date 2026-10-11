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

class QCheckBox;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QThread;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class PreviewLoader;
class UDisks;

// Which of the found files are shown.
struct LostFilter {
    int category = -1;       // a filetypes::Category, -1: every kind
    bool hideDamaged = false;
    bool deletedOnly = false;
    QString folder;          // and what's in it, or kFoundByContents or kDeletedRecently; empty: everywhere
    QString text;

    static inline const QString kFoundByContents = QStringLiteral("\x01contents");
    static inline const QString kDeletedRecently = QStringLiteral("\x01recent"); // what's known to have been deleted when, newest first
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
    int fileCount() const { return int(m_files.size()); }
    int fileIndex(int row) const { return m_shown.value(row, -1); }
    int count(int category) const; // how many of a kind were found
    void setThumbnail(int index, const QPixmap &thumbnail);
    QVector<lost::Found> ticked() const;
    QVector<int> tickedIndexes() const; // in the order ticked() has them
    int tickedCount() const { return int(m_ticked.size()); }
    // Where a file was saved to, once it has been (it stays in the list, marked).
    void markSaved(int index, const QString &path);
    QString savedTo(int index) const { return m_saved.value(index); }
    quint64 tickedSize() const;
    void tickShown(bool tick);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

Q_SIGNALS:
    void wantThumbnail(int index) const;
    void tickedChanged();

private:
    bool passes(const lost::Found &f) const;

    QVector<lost::Found> m_files;
    void sortShown();

    QVector<int> m_shown; // file indexes in the list, in order (newest deleted first for kDeletedRecently)
    QSet<int> m_ticked;
    QHash<int, QString> m_saved;
    QHash<int, QPixmap> m_thumbnails;
    mutable QSet<int> m_asked;
    LostFilter m_filter;
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
    void showPreviewResult(const QImage &image, const QString &text, bool failed);
    void updateTicked();
    void saveTicked();
    void saveSelected();
    // Saves files (indexes into the model) into `folder`, after the checks; the results stay.
    void saveFiles(const QVector<int> &indexes, const QString &folder);
    QVector<int> selectedIndexes() const;
    void showFilesMenu(const QPoint &at);
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
    QPushButton *m_grid;
    QListView *m_view;
    QLabel *m_previewImage;
    QLabel *m_details;
    QPlainTextEdit *m_hex;
    QSplitter *m_split = nullptr;
    QImage m_previewPicture; // the last one shown, fitted again when the room changes
    int m_previewAsked = 0;  // how big it was asked for
    QTimer *m_previewResize = nullptr;
    bool m_placed = false;
    QWidget *m_copiesBar = nullptr;
    QLabel *m_copiesText = nullptr;
    QPushButton *m_copiesLook = nullptr;
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
