// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lostfilesdialog.h"

#include "applog.h"
#include "blockio.h"
#include "dialogs.h"
#include "filetypes.h"
#include "format.h"
#include "gpt.h"
#include "health.h"
#include "lostfs.h"
#include "previewloader.h"
#include "udisks.h"
#include "vdisk.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QKeyEvent>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyledItemDelegate>
#include <QThread>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kThumbnail = 128;
constexpr int kPreview = 480;
constexpr qint64 kMostToDecode = 64 * 1024 * 1024; // a picture bigger than this isn't shown

QString categoryIcon(filetypes::Category category)
{
    switch (category) {
    case filetypes::Category::Picture:
        return QStringLiteral("image-x-generic");
    case filetypes::Category::Document:
        return QStringLiteral("x-office-document");
    case filetypes::Category::Video:
        return QStringLiteral("video-x-generic");
    case filetypes::Category::Music:
        return QStringLiteral("audio-x-generic");
    case filetypes::Category::Archive:
        return QStringLiteral("package-x-generic");
    case filetypes::Category::Other:
        break;
    }
    return QStringLiteral("text-x-generic");
}

QString conditionWords(lost::Condition condition)
{
    switch (condition) {
    case lost::Condition::Good:
        return QObject::tr("Its structure checks out from start to end, so it should open fine.");
    case lost::Condition::MaybeDamaged:
        return QObject::tr("Its structure breaks off or doesn't add up: it may open with parts missing, or not at all. Worth "
                           "saving anyway.");
    case lost::Condition::Overwritten:
        return QObject::tr("Part of its space has been used for something else since it was deleted.");
    case lost::Condition::Unreadable:
        break;
    }
    return QObject::tr("Part of it is on spots the drive couldn't read. Those parts come back blank.");
}

// What's known about how whole a file is, in plain words.
QString conditionWords(const lost::Found &f)
{
    if (f.nameOnly)
        return QObject::tr("Only its name is left: there's nothing of it to save.");
    if (f.origin == lost::Origin::FileSystem && !f.deleted && f.condition == lost::Condition::Good)
        return QObject::tr("It wasn't deleted: the file system still lists it, so it should be whole.");
    if (f.origin == lost::Origin::FileSystem && f.condition == lost::Condition::Good && !f.checked) {
        return f.type >= 0 && f.size > lost::kFullCheckLimit
                   ? QObject::tr("It starts the way it should, and its space hasn't been used for anything else since it was "
                                 "deleted. It's too big to check all of here.")
                   : QObject::tr("Its space hasn't been used for anything else since it was deleted. What's in it can't be "
                                 "checked for this kind of file.");
    }
    return conditionWords(f.condition);
}

} // namespace

// Draws a found file in the picture grid the same way whatever the system's style: its thumbnail
// (or its icon, or a card with its extension when there's neither) above its name, with the
// tick box in the corner. Styles draw icon grids very differently (Windows 11's puts the name
// beside the picture and gets where to click wrong), and here they all have to work alike.
constexpr int kNameRole = Qt::UserRole + 1; // the file's own name, without what the list adds to it

class GridDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static bool grid(const QStyleOptionViewItem &option)
    {
        const auto *view = qobject_cast<const QListView *>(option.widget);
        return view && view->viewMode() == QListView::IconMode;
    }

    QRect checkRect(const QStyleOptionViewItem &option) const
    {
        const QStyle *style = option.widget ? option.widget->style() : QApplication::style();
        const int size = style->pixelMetric(QStyle::PM_IndicatorWidth, &option, option.widget);
        return QRect(option.rect.left() + 4, option.rect.top() + 4, size, size);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (!grid(option))
            return QStyledItemDelegate::paint(p, option, index);
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        p->save();
        // The selection is drawn here too: Windows 11's own is so light that the name, drawn in the
        // selected-text colour below, vanished on it.
        const bool selected = opt.state & QStyle::State_Selected;
        const QPalette::ColorGroup group = opt.state & QStyle::State_Active ? QPalette::Active : QPalette::Inactive;
        if (selected || (opt.state & QStyle::State_MouseOver)) {
            QColor fill = opt.palette.color(group, QPalette::Highlight);
            if (!selected)
                fill.setAlpha(40);
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(Qt::NoPen);
            p->setBrush(fill);
            p->drawRoundedRect(QRectF(opt.rect).adjusted(1.5, 1.5, -1.5, -1.5), 4, 4);
        }
        const int textHeight = 2 * opt.fontMetrics.height() + 4;
        const QRect area = opt.rect.adjusted(6, 6, -6, -6 - textHeight);

        const QVariant decoration = index.data(Qt::DecorationRole);
        QPixmap picture;
        if (decoration.canConvert<QPixmap>() && decoration.typeId() == QMetaType::QPixmap)
            picture = decoration.value<QPixmap>();
        else if (decoration.canConvert<QIcon>())
            picture = decoration.value<QIcon>().pixmap(64, 64);
        if (!picture.isNull()) {
            const QSize fit = picture.size().scaled(area.size().boundedTo(picture.size()), Qt::KeepAspectRatio);
            p->drawPixmap(QRect(area.center() - QPoint(fit.width() / 2, fit.height() / 2), fit), picture);
        } else {
            // No picture and no icon: a card with the file's extension on it.
            const QString name = index.data(kNameRole).toString(); // the shown text may have more after it
            const QString ext = name.contains(QLatin1Char('.')) ? name.section(QLatin1Char('.'), -1).toUpper().left(5) : QStringLiteral("?");
            const QRect card(area.center() - QPoint(26, 32), QSize(52, 64));
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(QPen(opt.palette.color(QPalette::Mid), 1.5));
            p->setBrush(opt.palette.color(QPalette::Base));
            p->drawRoundedRect(card, 5, 5);
            QFont f = opt.font;
            f.setBold(true);
            f.setPointSizeF(f.pointSizeF() * 0.95);
            p->setFont(f);
            p->setPen(opt.palette.color(QPalette::Highlight));
            p->drawText(card, Qt::AlignCenter, ext);
            p->setFont(opt.font);
        }

        QStyleOptionViewItem check(opt);
        if (!index.data(Qt::CheckStateRole).isValid())
            check.rect = QRect(); // nothing to save, so nothing to tick
        else
            check.rect = checkRect(opt);
        check.state = check.state & ~QStyle::State_HasFocus;
        check.state |= index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? QStyle::State_On : QStyle::State_Off;
        if (check.rect.isValid())
            style->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &check, p, opt.widget);

        p->setPen(opt.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text));
        const QRect text(opt.rect.left() + 4, opt.rect.bottom() - textHeight, opt.rect.width() - 8, textHeight);
        // Two lines at most: what fits on the first (up to a space, when there's one), the rest
        // on the second, cut short with "…" in the middle so the extension still shows.
        const QString name = opt.text;
        qsizetype cut = name.size();
        while (cut > 0 && opt.fontMetrics.horizontalAdvance(name.left(cut)) > text.width())
            --cut;
        const qsizetype space = name.lastIndexOf(QLatin1Char(' '), cut);
        if (cut < name.size() && space > cut / 2)
            cut = space + 1;
        QStringList lines = {name.left(cut).trimmed()};
        if (cut < name.size())
            lines << opt.fontMetrics.elidedText(name.mid(cut), Qt::ElideMiddle, text.width());
        p->drawText(text, Qt::AlignHCenter | Qt::AlignTop, lines.join(QLatin1Char('\n')));
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (!grid(option))
            return QStyledItemDelegate::sizeHint(option, index);
        const auto *view = qobject_cast<const QListView *>(option.widget);
        return view->gridSize();
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (!grid(option))
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        // A click on the tick box ticks, anywhere else it just picks the file.
        if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && checkRect(option).adjusted(-3, -3, 3, 3).contains(mouse->position().toPoint())) {
                const bool on = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
                return model->setData(index, on ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
            }
        } else if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Space) {
            const bool on = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
            return model->setData(index, on ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
        }
        return false;
    }
};

// --- The model

LostFilesModel::LostFilesModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

bool LostFilesModel::passes(const lost::Found &f) const
{
    const LostFilter &w = m_filter;
    if (w.category >= 0 && (f.type < 0 || int(filetypes::types()[f.type].category) != w.category))
        return false;
    if (w.hideDamaged && f.condition != lost::Condition::Good)
        return false;
    if (w.deletedOnly && !f.deleted)
        return false;
    if (w.folder == LostFilter::kFoundByContents) {
        if (f.origin != lost::Origin::Contents)
            return false;
    } else if (w.folder == LostFilter::kDeletedRecently) {
        if (f.deletedAt <= 0)
            return false;
    } else if (!w.folder.isEmpty()) {
        if (f.origin != lost::Origin::FileSystem || (f.folder != w.folder && !f.folder.startsWith(w.folder + QLatin1Char('/'))))
            return false;
    }
    return w.text.isEmpty() || f.name.contains(w.text, Qt::CaseInsensitive) || f.folder.contains(w.text, Qt::CaseInsensitive);
}

void LostFilesModel::append(const QVector<lost::Found> &files)
{
    QVector<int> add;
    for (int i = 0; i < files.size(); ++i) {
        if (passes(files[i]))
            add << int(m_files.size()) + i;
    }
    m_files += files;
    if (add.isEmpty())
        return;
    if (m_filter.folder == LostFilter::kDeletedRecently) {
        // In time order, so the list is made again.
        beginResetModel();
        m_shown += add;
        sortShown();
        endResetModel();
        return;
    }
    beginInsertRows({}, int(m_shown.size()), int(m_shown.size() + add.size() - 1));
    m_shown += add;
    endInsertRows();
}

void LostFilesModel::clear()
{
    beginResetModel();
    m_files.clear();
    m_shown.clear();
    m_ticked.clear();
    m_thumbnails.clear();
    m_asked.clear();
    endResetModel();
    Q_EMIT tickedChanged();
}

void LostFilesModel::setFilter(const LostFilter &filter)
{
    beginResetModel();
    m_filter = filter;
    m_filter.text = filter.text.trimmed();
    m_shown.clear();
    for (int i = 0; i < m_files.size(); ++i) {
        if (passes(m_files[i]))
            m_shown << i;
    }
    sortShown();
    endResetModel();
}

void LostFilesModel::sortShown()
{
    if (m_filter.folder == LostFilter::kDeletedRecently)
        std::stable_sort(m_shown.begin(), m_shown.end(), [this](int a, int b) { return m_files[a].deletedAt > m_files[b].deletedAt; });
}

int LostFilesModel::count(int category) const
{
    if (category < 0)
        return int(m_files.size());
    int n = 0;
    for (const lost::Found &f : m_files)
        n += f.type >= 0 && int(filetypes::types()[f.type].category) == category;
    return n;
}

void LostFilesModel::setThumbnail(int index, const QPixmap &thumbnail)
{
    m_thumbnails.insert(index, thumbnail);
    const auto it = m_filter.folder == LostFilter::kDeletedRecently ? std::find(m_shown.cbegin(), m_shown.cend(), index)
                                                                   : std::lower_bound(m_shown.cbegin(), m_shown.cend(), index);
    if (it != m_shown.cend() && *it == index) {
        const QModelIndex row = createIndex(int(it - m_shown.cbegin()), 0);
        Q_EMIT dataChanged(row, row, {Qt::DecorationRole});
    }
}

QVector<lost::Found> LostFilesModel::ticked() const
{
    QList<int> indexes = m_ticked.values();
    std::sort(indexes.begin(), indexes.end());
    QVector<lost::Found> out;
    for (int i : std::as_const(indexes))
        out << m_files[i];
    return out;
}

quint64 LostFilesModel::tickedSize() const
{
    quint64 size = 0;
    for (int i : m_ticked)
        size += m_files[i].size;
    return size;
}

void LostFilesModel::tickShown(bool tick)
{
    for (int i : std::as_const(m_shown)) {
        if (tick && !m_files[i].nameOnly)
            m_ticked.insert(i);
        else
            m_ticked.remove(i);
    }
    if (!m_shown.isEmpty())
        Q_EMIT dataChanged(index(0), index(int(m_shown.size()) - 1), {Qt::CheckStateRole});
    Q_EMIT tickedChanged();
}

int LostFilesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_shown.size());
}

QVariant LostFilesModel::data(const QModelIndex &index, int role) const
{
    const int i = fileIndex(index.row());
    if (i < 0)
        return {};
    const lost::Found &f = m_files[i];
    const QString deleted = f.deletedAt > 0 ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(f.deletedAt), QLocale::ShortFormat) : QString();
    switch (role) {
    case Qt::DisplayRole:
        return m_filter.folder == LostFilter::kDeletedRecently ? tr("%1 (deleted %2)").arg(f.name, deleted) : f.name;
    case Qt::DecorationRole: {
        const auto thumb = m_thumbnails.constFind(i);
        if (thumb != m_thumbnails.cend() && !thumb->isNull())
            return *thumb;
        const filetypes::Category category = f.type >= 0 ? filetypes::types()[f.type].category : filetypes::Category::Other;
        const bool shows = category == filetypes::Category::Picture || category == filetypes::Category::Video
                        || category == filetypes::Category::Music || (f.type >= 0 && filetypes::types()[f.type].id == QLatin1String("pdf"));
        if (shows && !m_asked.contains(i)) {
            m_asked.insert(i);
            Q_EMIT wantThumbnail(i);
        }
        return QIcon::fromTheme(categoryIcon(category));
    }
    case Qt::CheckStateRole:
        if (f.nameOnly)
            return {};
        return m_ticked.contains(i) ? Qt::Checked : Qt::Unchecked;
    case Qt::ToolTipRole: {
        QString tip = QStringLiteral("%1\n%2, %3").arg(f.name, f.type >= 0 ? filetypes::types()[f.type].name : tr("File"),
                                                       f.nameOnly ? tr("only the name is left") : formatSize(f.size));
        if (!deleted.isEmpty())
            tip += QLatin1Char('\n') + tr("Deleted %1").arg(deleted);
        return tip;
    }
    case Qt::UserRole:
        return i;
    case kNameRole:
        return f.name;
    default:
        return {};
    }
}

bool LostFilesModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    const int i = fileIndex(index.row());
    if (i < 0 || role != Qt::CheckStateRole || m_files[i].nameOnly)
        return false;
    if (value.toInt() == Qt::Checked)
        m_ticked.insert(i);
    else
        m_ticked.remove(i);
    Q_EMIT dataChanged(index, index, {Qt::CheckStateRole});
    Q_EMIT tickedChanged();
    return true;
}

Qt::ItemFlags LostFilesModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    const int i = fileIndex(index.row());
    if (i >= 0 && m_files[i].nameOnly)
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
}

// --- The window

LostFilesDialog::LostFilesDialog(UDisks *udisks, const QString &preferredPath, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_pages(new QStackedWidget)
    , m_drives(new QTreeWidget)
    , m_driveNote(new QLabel)
    , m_look(new QPushButton(tr("Look for Files")))
    , m_status(new QLabel)
    , m_bar(new QProgressBar)
    , m_phase(new QLabel)
    , m_meter(m_bar, m_phase)
    , m_stop(new QPushButton(tr("Stop")))
    , m_categories(new QListWidget)
    , m_folders(new QTreeWidget)
    , m_search(new QLineEdit)
    , m_hideDamaged(new QCheckBox(tr("Hide files that may be damaged")))
    , m_deletedOnly(new QCheckBox(tr("Only deleted files")))
    , m_grid(new QPushButton(tr("Show as a List")))
    , m_view(new QListView)
    , m_previewImage(new QLabel)
    , m_details(new QLabel)
    , m_hex(new QPlainTextEdit)
    , m_tickedInfo(new QLabel)
    , m_save(new QPushButton(tr("Save Ticked Files To…")))
    , m_saveStatus(new QLabel)
    , m_saveBar(new QProgressBar)
    , m_savePhase(new QLabel)
    , m_saveMeter(m_saveBar, m_savePhase)
    , m_openFolder(new QPushButton(tr("Open the Folder")))
    , m_backToFiles(new QPushButton(tr("Back to the Files")))
    , m_model(new LostFilesModel(this))
{
    setWindowTitle(tr("Find Lost Files"));
    setObjectName(QStringLiteral("lostFiles"));
    resize(1120, 720);
    QImageReader::setAllocationLimit(256);

    // 1. Where were the files?
    auto *pick = new QWidget;
    auto *pickLayout = new QVBoxLayout(pick);
    pickLayout->addWidget(wrappingLabel(tr("Find Lost Files looks through a drive for files that were deleted, or that are on a drive "
                                           "that was formatted or won't open. It only reads: nothing on the drive changes. Pick "
                                           "where the files were: the whole drive finds the most.")));
    m_drives->setObjectName(QStringLiteral("drives"));
    m_drives->setHeaderLabels({tr("Drive or partition"), tr("Size"), tr("File system"), tr("Label")});
    m_drives->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_drives->setRootIsDecorated(true);
    pickLayout->addWidget(m_drives, 1);
    m_driveNote->setWordWrap(true);
    m_driveNote->setTextFormat(Qt::RichText);
    m_driveNote->setObjectName(QStringLiteral("driveNote"));
    pickLayout->addWidget(m_driveNote);
    auto *pickButtons = new QHBoxLayout;
    auto *image = new QPushButton(tr("A Disk Image File…"));
    image->setAutoDefault(false);
    pickButtons->addWidget(image);
    pickButtons->addStretch();
    m_look->setAutoDefault(false);
    pickButtons->addWidget(m_look);
    pickLayout->addLayout(pickButtons);
    m_pages->addWidget(pick);

    // 2. What was found
    auto *results = new QWidget;
    auto *resultsLayout = new QVBoxLayout(results);
    auto *top = new QHBoxLayout;
    m_status->setObjectName(QStringLiteral("status"));
    top->addWidget(m_status, 1);
    m_stop->setAutoDefault(false);
    top->addWidget(m_stop);
    resultsLayout->addLayout(top);
    resultsLayout->addWidget(m_bar);
    resultsLayout->addWidget(m_phase);
    m_copiesBar = new QWidget;
    auto *copies = new QHBoxLayout(m_copiesBar);
    copies->setContentsMargins(0, 0, 0, 0);
    m_copiesText = new QLabel;
    m_copiesText->setObjectName(QStringLiteral("copies"));
    m_copiesText->setWordWrap(true);
    m_copiesLook = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open-recent")), tr("Look in Them"));
    m_copiesLook->setObjectName(QStringLiteral("lookInCopies"));
    copies->addWidget(m_copiesText, 1);
    copies->addWidget(m_copiesLook);
    m_copiesBar->hide();
    resultsLayout->addWidget(m_copiesBar);

    auto *split = new QSplitter;
    // On the left: kinds of file, and below them the folders the file systems still name.
    auto *left = new QSplitter(Qt::Vertical);
    m_categories->setObjectName(QStringLiteral("categories"));
    left->addWidget(m_categories);
    m_folders->setObjectName(QStringLiteral("folders"));
    m_folders->setHeaderLabel(tr("Folders"));
    m_folders->setRootIsDecorated(true);
    m_folders->setUniformRowHeights(true);
    // Long names lose their middle, not the end that tells them apart (a time, the count).
    m_folders->setTextElideMode(Qt::ElideMiddle);
    left->addWidget(m_folders);
    left->setMaximumWidth(260);
    split->addWidget(left);
    auto *middle = new QWidget;
    auto *middleLayout = new QVBoxLayout(middle);
    middleLayout->setContentsMargins(0, 0, 0, 0);
    auto *filters = new QHBoxLayout;
    m_search->setPlaceholderText(tr("Search the names"));
    m_search->setClearButtonEnabled(true);
    filters->addWidget(m_search, 1);
    m_deletedOnly->setObjectName(QStringLiteral("deletedOnly"));
    m_deletedOnly->setChecked(true);
    m_deletedOnly->setToolTip(tr("Files that weren't deleted are listed too, for a drive whose file system won't open. "
                                 "Untick this to see them."));
    filters->addWidget(m_deletedOnly);
    filters->addWidget(m_hideDamaged);
    m_grid->setAutoDefault(false);
    filters->addWidget(m_grid);
    middleLayout->addLayout(filters);
    m_view->setObjectName(QStringLiteral("files"));
    m_view->setModel(m_model);
    m_view->setViewMode(QListView::IconMode);
    m_view->setItemDelegate(new GridDelegate(m_view));
    m_view->setIconSize(QSize(kThumbnail, kThumbnail));
    m_view->setGridSize(QSize(kThumbnail + 40, kThumbnail + 44));
    m_view->setUniformItemSizes(true);
    m_view->setResizeMode(QListView::Adjust);
    m_view->setMovement(QListView::Static);
    m_view->setWordWrap(true);
    m_view->setLayoutMode(QListView::Batched);
    m_view->setBatchSize(300);
    middleLayout->addWidget(m_view, 1);
    split->addWidget(middle);
    auto *preview = new QWidget;
    auto *previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    m_previewImage->setAlignment(Qt::AlignCenter);
    m_previewImage->setMinimumSize(320, 240);
    m_previewImage->setObjectName(QStringLiteral("preview"));
    previewLayout->addWidget(m_previewImage, 1);
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::RichText);
    m_details->setObjectName(QStringLiteral("details"));
    previewLayout->addWidget(m_details);
    m_hex->setReadOnly(true);
    m_hex->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_hex->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_hex->setVisible(false);
    previewLayout->addWidget(m_hex, 1);
    split->addWidget(preview);
    split->setStretchFactor(1, 3);
    split->setStretchFactor(2, 2);
    resultsLayout->addWidget(split, 1);

    auto *bottom = new QHBoxLayout;
    m_tickedInfo->setObjectName(QStringLiteral("ticked"));
    bottom->addWidget(m_tickedInfo, 1);
    auto *tickAll = new QPushButton(tr("Tick All Shown"));
    auto *untick = new QPushButton(tr("Untick All"));
    for (QPushButton *b : {tickAll, untick, m_save})
        b->setAutoDefault(false);
    bottom->addWidget(tickAll);
    bottom->addWidget(untick);
    bottom->addWidget(m_save);
    resultsLayout->addLayout(bottom);
    m_pages->addWidget(results);

    // 3. Saving
    auto *saving = new QWidget;
    auto *savingLayout = new QVBoxLayout(saving);
    m_saveStatus->setWordWrap(true);
    m_saveStatus->setObjectName(QStringLiteral("saveStatus"));
    savingLayout->addWidget(m_saveStatus);
    savingLayout->addWidget(m_saveBar);
    savingLayout->addWidget(m_savePhase);
    savingLayout->addStretch();
    auto *savingButtons = new QHBoxLayout;
    savingButtons->addStretch();
    for (QPushButton *b : {m_backToFiles, m_openFolder}) {
        b->setAutoDefault(false);
        savingButtons->addWidget(b);
    }
    savingLayout->addLayout(savingButtons);
    m_pages->addWidget(saving);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_pages, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    box->button(QDialogButtonBox::Close)->setDefault(true);
    connect(box, &QDialogButtonBox::rejected, this, &LostFilesDialog::reject);
    layout->addWidget(box);

    connect(m_drives, &QTreeWidget::currentItemChanged, this, &LostFilesDialog::driveChosen);
    connect(m_drives, &QTreeWidget::itemDoubleClicked, this, &LostFilesDialog::start);
    connect(m_look, &QPushButton::clicked, this, &LostFilesDialog::start);
    connect(image, &QPushButton::clicked, this, &LostFilesDialog::chooseImage);
    connect(m_copiesLook, &QPushButton::clicked, this, &LostFilesDialog::lookInCopies);
    connect(m_stop, &QPushButton::clicked, this, [this] {
        if (m_quick)
            m_quick->cancel();
        if (m_scan)
            m_scan->cancel();
        m_stop->setEnabled(false);
    });
    connect(m_categories, &QListWidget::currentRowChanged, this, &LostFilesDialog::filterChanged);
    connect(m_folders, &QTreeWidget::currentItemChanged, this, &LostFilesDialog::filterChanged);
    connect(m_search, &QLineEdit::textChanged, this, &LostFilesDialog::filterChanged);
    connect(m_hideDamaged, &QCheckBox::toggled, this, &LostFilesDialog::filterChanged);
    connect(m_deletedOnly, &QCheckBox::toggled, this, [this] {
        refreshFolderCounts();
        filterChanged();
    });
    connect(m_grid, &QPushButton::clicked, this, [this] {
        const bool toList = m_view->viewMode() == QListView::IconMode;
        m_view->setViewMode(toList ? QListView::ListMode : QListView::IconMode);
        m_view->setIconSize(toList ? QSize(24, 24) : QSize(kThumbnail, kThumbnail));
        m_view->setGridSize(toList ? QSize() : QSize(kThumbnail + 40, kThumbnail + 44));
        m_grid->setText(toList ? tr("Show as Pictures") : tr("Show as a List"));
    });
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this, &LostFilesDialog::showPreview);
    connect(m_model, &LostFilesModel::tickedChanged, this, &LostFilesDialog::updateTicked);
    // The loader is made new for each scan, so it's looked up when a thumbnail is wanted.
    connect(m_model, &LostFilesModel::wantThumbnail, this, [this](int index) {
        if (m_thumbs)
            m_thumbs->want(index, m_model->file(index), kThumbnail);
    });
    connect(tickAll, &QPushButton::clicked, this, [this] { m_model->tickShown(true); });
    connect(untick, &QPushButton::clicked, this, [this] { m_model->tickShown(false); });
    connect(m_save, &QPushButton::clicked, this, &LostFilesDialog::saveTicked);
    connect(m_backToFiles, &QPushButton::clicked, this, [this] { m_pages->setCurrentIndex(1); });
    connect(m_openFolder, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_savedTo)); });

    fillDrives(preferredPath);
    updateTicked();
}

LostFilesDialog::~LostFilesDialog()
{
    stopWorkers();
}

void LostFilesDialog::stopWorkers()
{
    if (m_quick)
        m_quick->cancel();
    if (m_scan)
        m_scan->cancel();
    if (m_copyScan)
        m_copyScan->cancel();
    if (m_saver)
        m_saver->cancel();
    if (m_thumbs)
        m_thumbs->stop();
    for (QThread *t : {m_quickThread, m_scanThread, m_copyThread, m_saveThread, m_thumbThread}) {
        if (t) {
            t->quit();
            t->wait();
        }
    }
    m_quick = nullptr;
    m_scan = nullptr;
    m_copyScan = nullptr;
    m_saver = nullptr;
    m_thumbs = nullptr;
    m_quickThread = m_scanThread = m_copyThread = m_saveThread = m_thumbThread = nullptr;
}

void LostFilesDialog::fillDrives(const QString &preferredPath)
{
    m_drives->clear();
    const QVector<Disk> &disks = m_udisks->disks();
    QTreeWidgetItem *preferred = nullptr;
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        auto *disk = new QTreeWidgetItem(m_drives, {tr("Disk %1: %2").arg(i).arg(diskTitle(d)), formatSize(d.size), QString(), QString()});
        disk->setIcon(0, QIcon::fromTheme(d.isLoop ? QStringLiteral("media-optical")
                                                   : d.removable || d.bus == QLatin1String("usb") ? QStringLiteral("drive-removable-media-usb")
                                                                                                  : QStringLiteral("drive-harddisk")));
        disk->setData(0, Qt::UserRole, d.blockPath);
        disk->setData(0, Qt::UserRole + 1, d.device);
        disk->setData(0, Qt::UserRole + 2, i);
        if (d.blockPath == preferredPath)
            preferred = disk;
        for (const Volume &v : d.volumes) {
            if (v.isContainer)
                continue;
            auto *part = new QTreeWidgetItem(disk, {shortDevice(v.device), formatSize(v.size), v.fsType, v.label});
            part->setData(0, Qt::UserRole, v.objectPath);
            part->setData(0, Qt::UserRole + 1, d.device);
            part->setData(0, Qt::UserRole + 2, i);
            if (v.objectPath == preferredPath)
                preferred = part;
        }
        disk->setExpanded(true);
    }
    for (int c = 1; c < 4; ++c)
        m_drives->resizeColumnToContents(c);
    m_drives->setCurrentItem(preferred);
    driveChosen();
}

void LostFilesDialog::driveChosen()
{
    QTreeWidgetItem *item = m_drives->currentItem();
    m_look->setEnabled(item != nullptr);
    if (!item) {
        m_driveNote->setText(QString());
        return;
    }
    const Disk d = m_udisks->disks().value(item->data(0, Qt::UserRole + 2).toInt());
    QStringList notes;
    if (health::hasUnreadableSpots(d.health))
        notes << redText(tr("This drive has spots it can't read. Every read can make a failing drive worse: make a Rescue Copy "
                            "of it first (Action menu) and look in the copy with \"A Disk Image File\"."));
    if (d.isSystem)
        notes << tr("This is the drive the running system is on. Anything saved to it now can overwrite what was deleted, so look "
                    "soon, and save what you find to another drive.");
    if (!d.isLoop && d.rotationRate == 0 && !d.removable && d.bus != QLatin1String("usb"))
        notes << tr("This is an SSD. SSDs usually erase deleted files within minutes, so those are rarely found. Files from an SSD "
                    "that was formatted or won't open often still are.");
    m_driveNote->setText(notes.join(QStringLiteral("<br><br>")));
}

void LostFilesDialog::start()
{
    QTreeWidgetItem *item = m_drives->currentItem();
    if (!item)
        return;
    const QString path = item->data(0, Qt::UserRole).toString();
    const QString device = item->data(0, Qt::UserRole + 1).toString();
    const QString title = item->parent() ? QStringLiteral("%1 (%2)").arg(item->text(0), item->parent()->text(0)) : item->text(0);
    m_look->setEnabled(false);
    m_driveNote->setText(tr("Opening it (it may ask for the admin password)…"));
    // Read-only, and nothing gets unmounted: the benchmark way in.
    openBlockThen(m_udisks, this, path, UDisks::OpenMode::Benchmark, [this, title, device](int fd) {
        m_look->setEnabled(true);
        if (fd < 0) {
            m_driveNote->setText(redText(tr("It couldn't be opened for reading.")));
            return;
        }
        m_driveNote->clear();
        scanSource(lost::Source::fromFd(fd, true), title, device);
    });
}

void LostFilesDialog::chooseImage()
{
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Choose a Disk Image"), QDir::homePath(),
        tr("Disk images (*.img *.iso *.raw *.dd *.bin *.vdi *.vmdk *.vhd *.vhdx *.qcow2);;Virtual machines' disks (*.vdi *.vmdk *.vhd *.vhdx "
           "*.qcow2);;All files (*)"));
    if (!file.isEmpty())
        scanImage(file);
}

void LostFilesDialog::scanImage(const QString &path)
{
    if (path.endsWith(QLatin1String(".zst")) || path.endsWith(QLatin1String(".xz")) || path.endsWith(QLatin1String(".gz"))) {
        m_driveNote->setText(redText(tr("A packed image can't be looked through as it is. Unpack it first (or restore the backup "
                                        "to a drive).")));
        return;
    }
    QString error, warning;
    // A virtual machine's disk file is read as the disk inside it.
    auto source = vdisk::formatOf(path).isEmpty() ? lost::Source::fromImage(path, &error) : vdisk::open(path, &error, &warning);
    if (!source) {
        m_driveNote->setText(redText(tr("%1 can't be looked through. %2").arg(QFileInfo(path).fileName(), error).toHtmlEscaped()));
        return;
    }
    scanSource(source, QFileInfo(path).fileName(), QString());
    m_sourceWarning = warning;
}

void LostFilesDialog::scanSource(std::shared_ptr<lost::Source> source, const QString &title, const QString &diskDevice)
{
    stopWorkers();
    m_source = std::move(source);
    m_sourceWarning.clear();
    m_title = title;
    m_diskDevice = diskDevice;
    m_model->clear();
    m_previewIndex = -1;
    m_previewImage->clear();
    m_details->clear();
    m_hex->hide();
    m_pages->setCurrentIndex(1);
    m_scanning = true;
    m_stop->setEnabled(true);
    m_stop->show();
    m_bar->show();
    m_phase->show();
    m_status->setText(tr("Reading the file systems on %1…").arg(title));
    qCInfo(lcOps).noquote() << "Find Lost Files: looking through" << title;
    m_volumes.clear();
    m_folders->clear();
    m_folderItems.clear();
    m_inFolder.clear();
    m_deletedInFolder.clear();
    m_contentsItem = nullptr;
    m_recentItem = nullptr;
    m_copiesBar->hide();
    {
        const QSignalBlocker block(m_folders);
        auto *everywhere = new QTreeWidgetItem(m_folders, {tr("Everywhere")});
        everywhere->setIcon(0, QIcon::fromTheme(QStringLiteral("drive-harddisk")));
        m_folders->setCurrentItem(everywhere);
    }

    m_thumbs = new PreviewLoader(m_source);
    connect(m_thumbs, &PreviewLoader::ready, this, [this](int index, int pixels, const QImage &image, const QString &text, bool failed) {
        if (pixels == kThumbnail) {
            if (!image.isNull())
                m_model->setThumbnail(index, QPixmap::fromImage(image));
        } else if (index == m_previewIndex) {
            showPreviewResult(image, text, failed);
        }
    });
    m_thumbThread = startOnThread(this, m_thumbs);

    // First what the file systems still know (quick, and with the real names), then every
    // sector for files by their contents.
    m_quick = new lost::QuickScan(m_source);
    connect(m_quick, &lost::QuickScan::found, this, [this](const QVector<lost::Found> &files) {
        m_model->append(files);
        addFolders(files);
        refreshCategories();
    });
    connect(m_quick, &lost::QuickScan::progress, this, [this](quint64 done, quint64 total) {
        // A count of files, not bytes: the meter's speed and time left don't fit here.
        m_bar->setRange(0, 100);
        m_bar->setValue(total ? int(done * 100 / total) : 0);
        m_phase->setText(tr("Checking what's in the deleted files: %1 of %2").arg(done).arg(total));
    });
    connect(m_quick, &lost::QuickScan::finished, this, [this](bool stopped) {
        // The scan's thread is idle now: what it found is safe to read until it's let go.
        const QVector<lost::Extent> ranges = m_quick->knownRanges();
        const QVector<quint64> starts = m_quick->knownStarts();
        m_volumes = m_quick->volumes();
        m_quick = nullptr;
        if (m_quickThread)
            m_quickThread->quit();
        if (stopped)
            return scanDone(true);
        offerCopies();
        startDeepScan(ranges, starts);
    });
    m_quickThread = startOnThread(this, m_quick);
    refreshCategories();
    filterChanged();
    updateTicked();
}

namespace {

QString when(qint64 ms)
{
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(ms), QLocale::ShortFormat);
}

} // namespace

void LostFilesDialog::offerCopies()
{
    int copies = 0;
    qint64 newest = 0;
    QStringList none, problems;
    for (const lost::Volume &v : std::as_const(m_volumes)) {
        if (v.type != QLatin1String("ntfs"))
            continue; // only Windows' own file system has them
        if (!v.copiesProblem.isEmpty())
            problems << v.copiesProblem;
        if (v.copies.isEmpty())
            none << v.label;
        copies += int(v.copies.size());
        for (const vss::Copy &c : v.copies)
            newest = std::max(newest, c.created);
    }
    if (copies > 0) {
        m_copiesText->setText(tr("Windows kept %n earlier version(s) of this drive, the newest from %1. Files that have changed or gone since "
                                 "can come back from them.", nullptr, copies)
                                  .arg(when(newest)));
        m_copiesLook->setText(tr("Look in Them"));
        m_copiesLook->setEnabled(true);
        m_copiesLook->show();
    } else if (!none.isEmpty() || !problems.isEmpty()) {
        m_copiesText->setText(problems.isEmpty() ? tr("Windows kept no earlier versions of %1: System Restore was off for it, or they've "
                                                      "been deleted.")
                                                       .arg(none.join(QStringLiteral(", ")))
                                                 : problems.join(QLatin1Char(' ')));
        m_copiesLook->hide();
    } else {
        return;
    }
    m_copiesBar->show();
}

void LostFilesDialog::lookInCopies()
{
    if (m_copyScan) { // the button stops it while it looks
        m_copyScan->cancel();
        m_copiesLook->setEnabled(false);
        return;
    }
    m_copyScan = new lost::CopyScan(m_source, m_volumes);
    m_copiesLook->setText(tr("Stop"));
    connect(m_copyScan, &lost::CopyScan::found, this, [this](const QVector<lost::Found> &files) {
        m_model->append(files);
        addFolders(files);
        refreshCategories();
    });
    connect(m_copyScan, &lost::CopyScan::progress, this, [this](int done, int total, qint64 taken) {
        if (taken > 0)
            m_copiesText->setText(tr("Looking in the drive as it was on %1 (%2 of %3)…").arg(when(taken)).arg(done + 1).arg(total));
    });
    connect(m_copyScan, &lost::CopyScan::finished, this, [this](bool stopped, int listed, const QString &problem) {
        m_copyScan = nullptr;
        if (m_copyThread)
            m_copyThread->quit(); // stopWorkers() waits for it
        m_copiesLook->hide();
        QString text = stopped ? tr("Stopped looking in the earlier versions.")
                     : listed == 0 ? tr("Everything in the earlier versions is the same on the drive now.")
                                   : tr("%n file(s) from the earlier versions have changed or gone since: they're in the \"as it was on\" folders.", nullptr,
                                        listed);
        if (!problem.isEmpty())
            text += QLatin1Char(' ') + problem;
        m_copiesText->setText(text);
    });
    m_copyThread = startOnThread(this, m_copyScan);
}

void LostFilesDialog::startDeepScan(const QVector<lost::Extent> &knownRanges, const QVector<quint64> &knownStarts)
{
    QStringList read;
    for (const lost::Volume &v : std::as_const(m_volumes))
        read << v.label;
    m_status->setText(read.isEmpty() ? tr("Looking through %1 for files by what's inside them…").arg(m_title)
                                     : tr("Read %1. Now looking through all of %2 for files by what's inside them…")
                                           .arg(read.join(QStringLiteral(", ")), m_title));
    m_scan = new lost::DeepScan(m_source);
    m_scan->skip(knownRanges, knownStarts);
    connect(m_scan, &lost::DeepScan::found, this, [this](const QVector<lost::Found> &files) {
        m_model->append(files);
        addFolders(files);
        refreshCategories();
    });
    connect(m_scan, &lost::DeepScan::progress, this, [this](quint64 done, quint64 total) {
        m_meter.update(tr("Looking through the drive"), done, total);
    });
    connect(m_scan, &lost::DeepScan::finished, this, [this](bool stopped) {
        if (m_scanThread)
            m_scanThread->quit();
        m_scan = nullptr;
        scanDone(stopped);
    });
    m_scanThread = startOnThread(this, m_scan);
}

void LostFilesDialog::scanDone(bool stopped)
{
    m_scanning = false;
    m_stop->hide();
    int deleted = 0;
    for (int i = 0; i < m_model->fileCount(); ++i)
        deleted += m_model->file(i).deleted;
    const int kept = m_model->fileCount() - deleted;
    QString text;
    if (stopped)
        text = tr("Stopped. Found %1 files before that.").arg(deleted);
    else if (deleted == 0 && kept == 0)
        text = tr("Done. Nothing was found on %1.").arg(m_title);
    else if (kept == 0)
        text = tr("Done: found %1 files on %2.").arg(deleted).arg(m_title);
    else if (deleted == 0)
        text = tr("Done: nothing deleted was found on %1. %n file(s) that weren't deleted show when \"Only deleted files\" is unticked.", nullptr, kept)
                   .arg(m_title);
    else
        text = tr("Done: found %1 deleted files on %2, and %3 that weren't deleted.").arg(deleted).arg(m_title).arg(kept);
    if (!m_sourceWarning.isEmpty())
        text += QLatin1Char(' ') + m_sourceWarning;
    m_status->setText(text);
    refreshFolderCounts();
    updateTicked();
}

void LostFilesDialog::addFolders(const QVector<lost::Found> &files)
{
    QSet<QString> changed;
    for (const lost::Found &f : files) {
        if (f.deletedAt > 0) {
            if (!m_recentItem) {
                // Right under "Everywhere".
                m_recentItem = new QTreeWidgetItem;
                m_folders->insertTopLevelItem(std::min(1, m_folders->topLevelItemCount()), m_recentItem);
                m_recentItem->setData(0, Qt::UserRole, LostFilter::kDeletedRecently);
                m_recentItem->setIcon(0, QIcon::fromTheme(QStringLiteral("document-open-recent")));
                m_recentItem->setToolTip(0, tr("Deleted files whose time of deleting is known (from NTFS's change journal or the "
                                               "Recycle Bin), the newest first"));
            }
            m_inFolder[LostFilter::kDeletedRecently] += 1;
            m_deletedInFolder[LostFilter::kDeletedRecently] += 1;
            changed.insert(LostFilter::kDeletedRecently);
        }
        if (f.origin == lost::Origin::Contents) {
            if (!m_contentsItem) {
                m_contentsItem = new QTreeWidgetItem(m_folders);
                m_contentsItem->setData(0, Qt::UserRole, LostFilter::kFoundByContents);
                m_contentsItem->setIcon(0, QIcon::fromTheme(QStringLiteral("edit-find")));
                m_contentsItem->setToolTip(0, tr("Files found by what's inside them, without a name or a folder"));
            }
            m_inFolder[LostFilter::kFoundByContents] += 1;
            m_deletedInFolder[LostFilter::kFoundByContents] += f.deleted;
            changed.insert(LostFilter::kFoundByContents);
            continue;
        }
        // The folder and every one above it, made as needed; each counts the file.
        QString path;
        QTreeWidgetItem *parent = nullptr;
        for (const QString &part : f.folder.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            path = path.isEmpty() ? part : path + QLatin1Char('/') + part;
            QTreeWidgetItem *&item = m_folderItems[path];
            if (!item) {
                item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_folders);
                item->setData(0, Qt::UserRole, path);
                item->setData(0, Qt::UserRole + 1, part);
                item->setIcon(0, QIcon::fromTheme(parent ? QStringLiteral("folder") : QStringLiteral("drive-removable-media")));
                if (!parent)
                    item->setExpanded(true);
            }
            m_inFolder[path] += 1;
            m_deletedInFolder[path] += f.deleted;
            changed.insert(path);
            parent = item;
        }
    }
    if (!changed.isEmpty())
        refreshFolderCounts();
}

void LostFilesDialog::refreshFolderCounts()
{
    const bool deletedOnly = m_deletedOnly->isChecked();
    auto label = [&](QTreeWidgetItem *item, const QString &name, const QString &key) {
        const int n = deletedOnly ? m_deletedInFolder.value(key) : m_inFolder.value(key);
        item->setText(0, QStringLiteral("%1 (%2)").arg(name).arg(n));
        // A folder with nothing deleted in it is greyed out while only deleted files show.
        item->setForeground(0, n == 0 ? palette().brush(QPalette::Disabled, QPalette::Text) : palette().brush(QPalette::Text));
    };
    for (auto it = m_folderItems.cbegin(); it != m_folderItems.cend(); ++it)
        label(it.value(), it.value()->data(0, Qt::UserRole + 1).toString(), it.key());
    if (m_contentsItem)
        label(m_contentsItem, tr("Found by what's inside them"), LostFilter::kFoundByContents);
    if (m_recentItem)
        label(m_recentItem, tr("Deleted recently"), LostFilter::kDeletedRecently);
}

int LostFilesDialog::chosenCategory() const
{
    const QListWidgetItem *item = m_categories->currentItem();
    return item ? item->data(Qt::UserRole).toInt() : -1;
}

void LostFilesDialog::filterChanged()
{
    LostFilter filter;
    filter.category = chosenCategory();
    filter.hideDamaged = m_hideDamaged->isChecked();
    filter.deletedOnly = m_deletedOnly->isChecked();
    filter.text = m_search->text();
    if (const QTreeWidgetItem *folder = m_folders->currentItem())
        filter.folder = folder->data(0, Qt::UserRole).toString();
    m_model->setFilter(filter);
}

// The list of kinds, with how many of each, keeping the one that's chosen. New finds are added
// to the file list as they come, without building it again (which would jump the view).
void LostFilesDialog::refreshCategories()
{
    const int chosen = chosenCategory();
    {
        const QSignalBlocker block(m_categories);
        m_categories->clear();
        auto *all = new QListWidgetItem(tr("All files (%1)").arg(m_model->fileCount()), m_categories);
        all->setData(Qt::UserRole, -1);
        for (int c = 0; c <= int(filetypes::Category::Other); ++c) {
            const int n = m_model->count(c);
            if (n == 0 && c != chosen)
                continue;
            auto *item = new QListWidgetItem(QIcon::fromTheme(categoryIcon(filetypes::Category(c))),
                                             QStringLiteral("%1 (%2)").arg(filetypes::categoryName(filetypes::Category(c))).arg(n), m_categories);
            item->setData(Qt::UserRole, c);
            if (c == chosen)
                m_categories->setCurrentItem(item);
        }
        if (chosen < 0)
            m_categories->setCurrentRow(0);
    }
}

void LostFilesDialog::showPreview()
{
    const int i = m_model->fileIndex(m_view->currentIndex().row());
    m_previewIndex = i;
    m_previewImage->clear();
    m_hex->hide();
    if (i < 0) {
        m_details->clear();
        return;
    }
    const lost::Found &f = m_model->file(i);
    const filetypes::Type type = f.type >= 0 ? filetypes::types()[f.type] : filetypes::Type{QString(), filetypes::Category::Other, tr("File")};
    const QColor colour = f.condition == lost::Condition::Good ? QColor(0x2b, 0xee, 0x8a)
                        : f.condition == lost::Condition::MaybeDamaged ? QColor(0xff, 0xb3, 0x00)
                                                                      : QColor(0xff, 0x33, 0x55);
    const quint64 where = f.extents.isEmpty() ? 0 : f.extents.first().start;
    QString html = QStringLiteral("<b>%1</b><br>%2, %3<br>")
                       .arg(f.name.toHtmlEscaped(), type.name.toHtmlEscaped(), f.nameOnly ? tr("size not known") : formatSize(f.size));
    if (f.origin == lost::Origin::Contents) {
        html += tr("Found by what's inside it, %1 into the drive. Its real name and folder aren't known, so it has a made-up name.")
                    .arg(formatSize(where));
    } else {
        html += (f.deleted ? tr("Was in %1") : tr("In %1")).arg(f.folder.toHtmlEscaped());
        if (f.modified > 0)
            html += QStringLiteral("<br>") + tr("Last changed %1").arg(QLocale().toString(QDateTime::fromMSecsSinceEpoch(f.modified), QLocale::ShortFormat));
        if (f.deletedAt > 0)
            html += QStringLiteral("<br>") + tr("Deleted %1").arg(QLocale().toString(QDateTime::fromMSecsSinceEpoch(f.deletedAt), QLocale::ShortFormat));
        if (f.nameGuessed)
            html += QStringLiteral("<br>") + tr("Deleting it took away the first letter of its name; _ stands in for it.");
        if (f.piecesGuessed)
            html += QStringLiteral("<br>") + tr("The file system doesn't say where all of it is anymore, so it's taken to follow on "
                                                 "from where it started. That's right for most files.");
    }
    html += QStringLiteral("<br><br><span style=\"color:%1\">●</span> %2").arg(colour.name(), conditionWords(f).toHtmlEscaped());
    if (!f.note.isEmpty())
        html += QStringLiteral("<br>") + f.note.toHtmlEscaped();
    m_details->setText(html);
    if (f.nameOnly)
        return; // nothing of it to show
    // The picture and the words come from the preview helper; until then, a moment.
    m_previewImage->setText(tr("Loading…"));
    if (m_thumbs)
        m_thumbs->want(i, f, kPreview);
}

void LostFilesDialog::showPreviewResult(const QImage &image, const QString &text, bool failed)
{
    const lost::Found &f = m_model->file(m_previewIndex);
    const filetypes::Category category = f.type >= 0 ? filetypes::types()[f.type].category : filetypes::Category::Other;
    if (!image.isNull()) {
        // Bigger than the room there is (a PDF page in a narrow window): scaled down to fit.
        const QSize room = m_previewImage->size();
        QPixmap pixmap = QPixmap::fromImage(image);
        if (room.width() > 16 && room.height() > 16 && (pixmap.width() > room.width() || pixmap.height() > room.height()))
            pixmap = pixmap.scaled(room, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_previewImage->setPixmap(pixmap);
    } else if (failed)
        m_previewImage->setText(tr("It can't be shown: it may be damaged, or not what its name says."));
    else if (category == filetypes::Category::Picture)
        m_previewImage->setText(tr("This picture can't be shown (it may be damaged)."));
    else
        m_previewImage->setPixmap(QIcon::fromTheme(categoryIcon(category)).pixmap(96, 96));
    // Its words when there are some (a document's text, what's in an archive, a song's details),
    // otherwise what its first bytes are.
    if (!text.isEmpty()) {
        m_hex->setPlainText(text);
    } else {
        lost::FileReader reader(m_source, f);
        m_hex->setPlainText(gpt::hexDump(reader.head(512), 0));
    }
    m_hex->show();
}

void LostFilesDialog::updateTicked()
{
    const int n = m_model->tickedCount();
    m_tickedInfo->setText(n == 0 ? tr("Tick the files you want, then save them to another drive.")
                          : n == 1 ? tr("One file ticked (%1).").arg(formatSize(m_model->tickedSize()))
                                   : tr("%1 files ticked (%2).").arg(n).arg(formatSize(m_model->tickedSize())));
    m_save->setEnabled(n > 0 && !m_scanning && !m_saving);
    m_save->setToolTip(m_scanning ? tr("Stop the search first, or wait until it's done.") : QString());
}

void LostFilesDialog::saveTicked()
{
    const QString start = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Save the Ticked Files Where?"), start);
    if (!folder.isEmpty())
        saveTo(folder);
}

void LostFilesDialog::saveTo(const QString &folder)
{
    const QVector<lost::Found> files = m_model->ticked();
    if (files.isEmpty())
        return;
    // Never onto the drive the files are being got back from: it could overwrite them.
    if (!m_diskDevice.isEmpty() && blockio::pathIsOnDisk(folder, m_diskDevice)) {
        QMessageBox::warning(this, windowTitle(),
                             tr("That folder is on the drive you're getting the files back from. Saving there could overwrite the "
                                "very files you're after.\n\nPick a folder on another drive, like a USB stick or an external drive."));
        return;
    }
    const QStorageInfo where(folder);
    const QByteArray fs = where.fileSystemType();
    if (fs == "tmpfs" || fs == "ramfs" || fs == "overlay" || fs == "squashfs") {
        const auto answer = QMessageBox::warning(this, windowTitle(),
                                                 tr("That folder is in the computer's memory: what's saved there is gone when it shuts "
                                                    "down. Save to a USB stick or an external drive instead?"),
                                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes)
            return;
    }
    quint64 total = 0;
    bool tooBigForFat = false;
    for (const lost::Found &f : files) {
        total += f.size;
        tooBigForFat = tooBigForFat || f.size >= 4ULL * 1024 * 1024 * 1024;
    }
    if (where.isValid() && quint64(where.bytesAvailable()) < total) {
        QMessageBox::warning(this, windowTitle(), tr("There's %1 free there, and the ticked files need %2.").arg(formatSize(quint64(where.bytesAvailable())), formatSize(total)));
        return;
    }
    if (tooBigForFat && (fs == "vfat" || fs == "msdos")) {
        const auto answer = QMessageBox::warning(this, windowTitle(),
                                                 tr("Some ticked files are 4 GB or bigger, and that drive is FAT32, which can't hold "
                                                    "them. They'll be skipped. Save the rest?"),
                                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    m_saving = true;
    updateTicked();
    m_pages->setCurrentIndex(2);
    m_saveStatus->setText(tr("Saving %1 files to %2…").arg(files.size()).arg(folder));
    m_openFolder->hide();
    m_backToFiles->hide();
    m_saver = new lost::Saver(m_source, files, folder);
    connect(m_saver, &lost::Saver::progress, this, [this](quint64 done, quint64 total) { m_saveMeter.update(tr("Saving"), done, total); });
    connect(m_saver, &lost::Saver::finished, this, [this](int saved, int failed, const QString &to, const QString &message) {
        m_saving = false;
        if (m_saveThread)
            m_saveThread->quit();
        m_saver = nullptr;
        m_savedTo = to;
        QString text = failed == 0 ? tr("Saved %1 files to %2.").arg(saved).arg(to) : tr("Saved %1 files to %2. %3 couldn't be saved.").arg(saved).arg(to).arg(failed);
        if (!message.isEmpty())
            text = message + QLatin1Char(' ') + text;
        m_saveStatus->setText(text + QLatin1Char(' ') + tr("\"What was saved.txt\" there lists them, and any that may be damaged."));
        m_openFolder->show();
        m_backToFiles->show();
        updateTicked();
    });
    m_saveThread = startOnThread(this, m_saver);
}

void LostFilesDialog::reject()
{
    if (m_saving) {
        if (QMessageBox::question(this, windowTitle(), tr("Stop saving? What's saved so far stays."), QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No)
            != QMessageBox::Yes)
            return;
    } else if (m_scanning && m_model->fileCount() > 0) {
        if (QMessageBox::question(this, windowTitle(), tr("Close? What was found so far isn't kept."), QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No)
            != QMessageBox::Yes)
            return;
    }
    stopWorkers();
    QDialog::reject();
}

