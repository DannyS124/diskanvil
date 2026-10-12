// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lostfilesdialog.h"

#include "applog.h"
#include "blockio.h"
#include "dialogs.h"
#include "flowlayout.h"
#include "filetypes.h"
#include "format.h"
#include "gpt.h"
#include "health.h"
#include "lookcopies.h"
#include "lostfs.h"
#include "previewloader.h"
#include "viewer.h"
#include "udisks.h"
#include "vdisk.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
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
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProcess>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyledItemDelegate>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <numeric>

namespace {

constexpr int kThumbnail = 128;
constexpr int kPreview = 480;      // the smallest a preview is asked for
constexpr int kPreviewMost = 2048; // and the biggest, its longest side
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
constexpr int kNameRole = Qt::UserRole + 1;  // the file's own name, without what the list adds to it
constexpr int kSavedRole = Qt::UserRole + 2; // where it was saved to, once it has been
constexpr int kMarkRole = Qt::UserRole + 3;  // "copy" or "broken": marked on its picture

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

    // A red "!" on a file that didn't open when it was checked, a grey "=" on a copy of another.
    static void drawMark(QPainter *p, const QRectF &badge, const QString &mark)
    {
        if (mark.isEmpty())
            return;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(mark == QLatin1String("broken") ? QColor(0xd0, 0x1f, 0x3c) : QColor(0x6b, 0x72, 0x80));
        p->drawEllipse(badge);
        QFont f = p->font();
        f.setBold(true);
        f.setPixelSize(12);
        p->setFont(f);
        p->setPen(Qt::white);
        p->drawText(badge, Qt::AlignCenter, mark == QLatin1String("broken") ? QStringLiteral("!") : QStringLiteral("="));
        p->restore();
    }

    // A green tick on a file that has been saved, so it's clear what's done.
    static void drawSaved(QPainter *p, const QRectF &badge)
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0x0f, 0x7b, 0x0f));
        p->drawEllipse(badge);
        p->setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF at = badge.topLeft();
        p->drawPolyline(QPolygonF{at + QPointF(4, 8.5), at + QPointF(7, 11.5), at + QPointF(12, 5)});
        p->restore();
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const bool saved = !index.data(kSavedRole).toString().isEmpty();
        if (!grid(option)) {
            QStyledItemDelegate::paint(p, option, index);
            if (saved)
                drawSaved(p, QRectF(option.rect.right() - 22, option.rect.center().y() - 8, 16, 16));
            drawMark(p, QRectF(option.rect.right() - (saved ? 42 : 22), option.rect.center().y() - 8, 16, 16), index.data(kMarkRole).toString());
            return;
        }
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
        if (saved)
            drawSaved(p, QRectF(opt.rect.right() - 22, opt.rect.top() + 5, 16, 16));
        drawMark(p, QRectF(opt.rect.right() - (saved ? 42 : 22), opt.rect.top() + 5, 16, 16), index.data(kMarkRole).toString());
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

bool LostFilesModel::passes(int index) const
{
    const lost::Found &f = m_files[index];
    const LostFilter &w = m_filter;
    if (w.hideSmallPictures && f.type >= 0 && filetypes::types()[f.type].category == filetypes::Category::Picture && f.size < LostFilter::kSmallPicture)
        return false;
    if (w.hideCopies && m_copyOf.contains(index))
        return false;
    if (w.hideBroken && !m_opens.value(index, true))
        return false;
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
    const int first = int(m_files.size());
    m_files += files;
    QVector<int> add;
    for (int i = first; i < m_files.size(); ++i) {
        if (passes(i))
            add << i;
    }
    if (add.isEmpty())
        return;
    if (!inFoundOrder()) {
        // Put in where they go, the view keeping what's picked (and where it's scrolled to).
        Q_EMIT layoutAboutToBeChanged();
        const QModelIndexList was = persistentIndexList();
        QVector<int> files;
        for (const QModelIndex &i : was)
            files << fileIndex(i.row());
        std::stable_sort(add.begin(), add.end(), [this](int a, int b) { return before(a, b); });
        QVector<int> merged;
        merged.reserve(m_shown.size() + add.size());
        std::merge(m_shown.cbegin(), m_shown.cend(), add.cbegin(), add.cend(), std::back_inserter(merged), [this](int a, int b) { return before(a, b); });
        m_shown = std::move(merged);
        indexRows();
        QModelIndexList now;
        for (int f : std::as_const(files))
            now << (f >= 0 ? index(m_rowOf.value(f)) : QModelIndex());
        changePersistentIndexList(was, now);
        Q_EMIT layoutChanged();
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
    m_saved.clear();
    m_thumbnails.clear();
    m_asked.clear();
    m_copyOf.clear();
    m_opens.clear();
    m_rowOf.clear();
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
        if (passes(i))
            m_shown << i;
    }
    sortShown();
    endResetModel();
}

bool LostFilesModel::before(int a, int b) const
{
    const lost::Found &x = m_files[a], &y = m_files[b];
    if (m_filter.folder == LostFilter::kDeletedRecently) // newest deleted first, whatever else
        return x.deletedAt != y.deletedAt ? x.deletedAt > y.deletedAt : a < b;
    switch (m_filter.order) {
    case LostFilter::Order::Name:
        if (const int c = x.name.compare(y.name, Qt::CaseInsensitive))
            return c < 0;
        break;
    case LostFilter::Order::Date: { // newest first, the ones without a date last
        const qint64 dx = dateOf(x), dy = dateOf(y);
        if (dx != dy)
            return dy == 0 || (dx != 0 && dx > dy);
        break;
    }
    case LostFilter::Order::Size:
        if (x.size != y.size)
            return x.size > y.size;
        break;
    case LostFilter::Order::Kind: {
        const int cx = x.type >= 0 ? int(filetypes::types()[x.type].category) : 99, cy = y.type >= 0 ? int(filetypes::types()[y.type].category) : 99;
        if (cx != cy)
            return cx < cy;
        const QString ex = x.name.section(QLatin1Char('.'), -1).toLower(), ey = y.name.section(QLatin1Char('.'), -1).toLower();
        if (ex != ey)
            return ex < ey;
        if (const int c = x.name.compare(y.name, Qt::CaseInsensitive))
            return c < 0;
        break;
    }
    case LostFilter::Order::Found:
        break;
    }
    return a < b;
}

void LostFilesModel::sortShown()
{
    if (!inFoundOrder())
        std::stable_sort(m_shown.begin(), m_shown.end(), [this](int a, int b) { return before(a, b); });
    indexRows();
}

void LostFilesModel::indexRows()
{
    m_rowOf.clear();
    if (inFoundOrder())
        return; // found order is index order: a binary search finds a row
    m_rowOf.reserve(m_shown.size());
    for (int row = 0; row < m_shown.size(); ++row)
        m_rowOf.insert(m_shown[row], row);
}

int LostFilesModel::rowOf(int index) const
{
    if (!inFoundOrder())
        return m_rowOf.value(index, -1);
    const auto it = std::lower_bound(m_shown.cbegin(), m_shown.cend(), index);
    return it != m_shown.cend() && *it == index ? int(it - m_shown.cbegin()) : -1;
}

void LostFilesModel::removeShown(int index)
{
    const int row = rowOf(index);
    if (row < 0)
        return;
    beginRemoveRows({}, row, row);
    m_shown.removeAt(row);
    indexRows();
    endRemoveRows();
}

void LostFilesModel::setCopies(const QHash<int, int> &copyOf)
{
    m_copyOf = copyOf;
    if (m_filter.hideCopies)
        setFilter(m_filter);
    else if (!m_shown.isEmpty())
        Q_EMIT dataChanged(index(0), index(int(m_shown.size()) - 1), {Qt::ToolTipRole, kMarkRole});
}

void LostFilesModel::setOpens(int index, bool opens)
{
    m_opens.insert(index, opens);
    if (!opens && m_filter.hideBroken) {
        removeShown(index);
        return;
    }
    if (const int row = rowOf(index); row >= 0)
        Q_EMIT dataChanged(this->index(row), this->index(row), {Qt::ToolTipRole, kMarkRole});
}

int LostFilesModel::brokenCount() const
{
    return int(std::count(m_opens.cbegin(), m_opens.cend(), false));
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
    if (const int row = rowOf(index); row >= 0)
        Q_EMIT dataChanged(this->index(row), this->index(row), {Qt::DecorationRole});
}

QVector<lost::Found> LostFilesModel::ticked() const
{
    QVector<lost::Found> out;
    for (int i : tickedIndexes())
        out << m_files[i];
    return out;
}

QVector<int> LostFilesModel::tickedIndexes() const
{
    QVector<int> indexes = m_ticked.values();
    std::sort(indexes.begin(), indexes.end());
    return indexes;
}

void LostFilesModel::markSaved(int index, const QString &path)
{
    m_saved.insert(index, path);
    const int row = rowOf(index);
    if (row >= 0)
        Q_EMIT dataChanged(this->index(row), this->index(row), {kSavedRole, Qt::ToolTipRole});
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
        if (m_saved.contains(i))
            tip += QLatin1Char('\n') + tr("Saved to %1").arg(m_saved.value(i));
        if (m_copyOf.contains(i))
            tip += QLatin1Char('\n') + tr("The same as \"%1\"").arg(m_files[m_copyOf.value(i)].name);
        if (!m_opens.value(i, true))
            tip += QLatin1Char('\n') + tr("It didn't open when it was checked: it's probably damaged");
        return tip;
    }
    case kMarkRole:
        return !m_opens.value(i, true) ? QStringLiteral("broken") : m_copyOf.contains(i) ? QStringLiteral("copy") : QString();
    case Qt::UserRole:
        return i;
    case kNameRole:
        return f.name;
    case kSavedRole:
        return m_saved.value(i);
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
    , m_hideSaved(new QPushButton(tr("Hide")))
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
    m_status->setWordWrap(true);
    top->addWidget(m_status, 1);
    m_stop->setAutoDefault(false);
    top->addWidget(m_stop);
    resultsLayout->addLayout(top);
    resultsLayout->addWidget(m_bar);
    m_phase->setWordWrap(true);
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
    split->addWidget(left);
    auto *middle = new QWidget;
    auto *middleLayout = new QVBoxLayout(middle);
    middleLayout->setContentsMargins(0, 0, 0, 0);
    // The search and the view on one line, the filters under them, so the middle stays narrow
    // enough for a small screen.
    auto *search = new QHBoxLayout;
    m_search->setPlaceholderText(tr("Search the names"));
    m_search->setClearButtonEnabled(true);
    search->addWidget(m_search, 1);
    m_sort = new QComboBox;
    m_sort->setObjectName(QStringLiteral("sort"));
    m_sort->addItem(tr("In the order found"), int(LostFilter::Order::Found));
    m_sort->addItem(tr("By name"), int(LostFilter::Order::Name));
    m_sort->addItem(tr("By date, newest first"), int(LostFilter::Order::Date));
    m_sort->addItem(tr("By size, biggest first"), int(LostFilter::Order::Size));
    m_sort->addItem(tr("By kind"), int(LostFilter::Order::Kind));
    m_sort->setToolTip(tr("The date is the one the drive kept, or for a file found by what's inside it, the one it carries "
                          "(when a photo was taken, an email sent)"));
    m_sort->setCurrentIndex(std::max(0, m_sort->findData(QSettings().value(QStringLiteral("lostFiles/order"), 0).toInt())));
    search->addWidget(m_sort);
    m_grid->setAutoDefault(false);
    search->addWidget(m_grid);
    middleLayout->addLayout(search);
    // The filters wrap onto a second line where the window is narrow.
    auto *filterBox = new QWidget;
    auto *filters = new FlowLayout(filterBox);
    filters->setContentsMargins(0, 0, 0, 0);
    m_deletedOnly->setObjectName(QStringLiteral("deletedOnly"));
    m_deletedOnly->setChecked(true);
    m_deletedOnly->setToolTip(tr("Files that weren't deleted are listed too, for a drive whose file system won't open. "
                                 "Untick this to see them."));
    filters->addWidget(m_deletedOnly);
    filters->addWidget(m_hideDamaged);
    m_hideSmall = new QCheckBox(tr("Hide small pictures"));
    m_hideSmall->setObjectName(QStringLiteral("hideSmall"));
    m_hideSmall->setToolTip(tr("Pictures under %1: icons, and the small copies web browsers and programs keep, which often outnumber the photos")
                                .arg(formatSize(LostFilter::kSmallPicture)));
    filters->addWidget(m_hideSmall);
    m_hideCopies = new QCheckBox;
    m_hideCopies->setObjectName(QStringLiteral("hideCopies"));
    m_hideCopies->hide(); // once copies were looked for
    filters->addWidget(m_hideCopies);
    m_hideBroken = new QCheckBox;
    m_hideBroken->setObjectName(QStringLiteral("hideBroken"));
    m_hideBroken->hide(); // once they were checked
    filters->addWidget(m_hideBroken);
    m_organise = new QToolButton;
    m_organise->setObjectName(QStringLiteral("organise"));
    m_organise->setText(tr("Organise"));
    m_organise->setPopupMode(QToolButton::InstantPopup);
    m_organise->setEnabled(false); // once the search is done
    auto *organise = new QMenu(m_organise);
    organise->setToolTipsVisible(true);
    m_findCopies = organise->addAction(tr("Find Copies"));
    m_findCopies->setObjectName(QStringLiteral("findCopies"));
    m_findCopies->setToolTip(tr("Finds the files that are the same as another one found, byte for byte, so each needs saving once"));
    m_checkOpen = organise->addAction(tr("Check They Open"));
    m_checkOpen->setObjectName(QStringLiteral("checkOpen"));
    m_checkOpen->setToolTip(tr("Opens each file shown the way its preview does (safely, apart from DiskForge), and marks the ones "
                               "that don't open: they're probably damaged or partly overwritten"));
    m_organise->setMenu(organise);
    filters->addWidget(m_organise);
    middleLayout->addWidget(filterBox);
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
    // Shift and Ctrl pick several, to save just those (right-click).
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    // Picked files can be dragged out to a folder (copies, never moved off the drive).
    m_view->viewport()->installEventFilter(this);
    middleLayout->addWidget(m_view, 1);
    split->addWidget(middle);
    auto *preview = new QWidget;
    auto *previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    m_previewImage->setAlignment(Qt::AlignCenter);
    m_previewImage->setMinimumSize(240, 180);
    // The picture is fitted to the room it has, so it never pushes the panes around.
    m_previewImage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_previewImage->installEventFilter(this);
    m_previewImage->setObjectName(QStringLiteral("preview"));
    previewLayout->addWidget(m_previewImage, 1);
    // The whole file, before saving it: pictures to zoom into, every page, films that play.
    m_viewButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-preview")), tr("View…"));
    m_viewButton->setObjectName(QStringLiteral("view"));
    m_viewButton->setAutoDefault(false);
    m_viewButton->setToolTip(tr("See all of it before saving it (double-click a file, or Enter, does the same)"));
    m_viewButton->setEnabled(false);
    previewLayout->addWidget(m_viewButton, 0, Qt::AlignLeft);
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
    // Every pane can be made wider or narrower, on any screen; none can be squeezed away.
    split->setChildrenCollapsible(false);
    split->setObjectName(QStringLiteral("panes"));
    m_split = split;
    resultsLayout->addWidget(split, 1);

    // Saving happens here, under the files, which stay for saving more.
    m_saveStrip = new QWidget;
    m_saveStrip->setObjectName(QStringLiteral("saveStrip"));
    auto *strip = new QGridLayout(m_saveStrip);
    strip->setContentsMargins(0, 0, 0, 0);
    m_saveStatus->setWordWrap(true);
    m_saveStatus->setObjectName(QStringLiteral("saveStatus"));
    strip->addWidget(m_saveStatus, 0, 0);
    for (QPushButton *b : {m_openFolder, m_hideSaved}) {
        b->setAutoDefault(false);
        strip->addWidget(b, 0, b == m_openFolder ? 1 : 2);
    }
    strip->addWidget(m_saveBar, 1, 0, 1, 3);
    m_savePhase->setWordWrap(true);
    strip->addWidget(m_savePhase, 2, 0, 1, 3);
    strip->setColumnStretch(0, 1);
    m_saveStrip->hide();
    resultsLayout->addWidget(m_saveStrip);

    auto *bottom = new QHBoxLayout;
    m_tickedInfo->setObjectName(QStringLiteral("ticked"));
    m_tickedInfo->setWordWrap(true);
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

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_pages, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    // Enter never closes it: everything found would be gone.
    box->button(QDialogButtonBox::Close)->setAutoDefault(false);
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
    for (QCheckBox *box : {m_hideSmall, m_hideCopies, m_hideBroken})
        connect(box, &QCheckBox::toggled, this, &LostFilesDialog::filterChanged);
    connect(m_sort, &QComboBox::currentIndexChanged, this, [this] {
        QSettings().setValue(QStringLiteral("lostFiles/order"), m_sort->currentData().toInt());
        filterChanged();
    });
    connect(m_findCopies, &QAction::triggered, this, &LostFilesDialog::findCopies);
    connect(m_checkOpen, &QAction::triggered, this, [this] {
        if (m_checker)
            stopChecking();
        else
            checkTheyOpen();
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
    connect(m_hideSaved, &QPushButton::clicked, m_saveStrip, &QWidget::hide);
    connect(m_openFolder, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_savedTo)); });
    connect(m_view, &QWidget::customContextMenuRequested, this, &LostFilesDialog::showFilesMenu);
    connect(m_view, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex &index) { openViewer(m_model->fileIndex(index.row())); });
    m_view->installEventFilter(this);
    connect(m_viewButton, &QPushButton::clicked, this, [this] { openViewer(m_previewIndex); });
    m_previewResize = new QTimer(this);
    m_previewResize->setSingleShot(true);
    m_previewResize->setInterval(200);
    connect(m_previewResize, &QTimer::timeout, this, &LostFilesDialog::fitPreview);

    fillDrives(preferredPath);
    updateTicked();
}

LostFilesDialog::~LostFilesDialog()
{
    stopWorkers();
}

void LostFilesDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!m_placed) {
        m_placed = true;
        placeWindow();
    }
}

void LostFilesDialog::hideEvent(QHideEvent *event)
{
    // Its size and panes, for next time.
    if (m_placed) {
        QSettings settings;
        settings.setValue(QStringLiteral("lostFiles/geometry"), saveGeometry());
        settings.setValue(QStringLiteral("lostFiles/panes"), m_split->saveState());
    }
    QDialog::hideEvent(event);
}

void LostFilesDialog::placeWindow()
{
    QSettings settings;
    const QScreen *screen = this->screen();
    const QRect room = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    const QByteArray geometry = settings.value(QStringLiteral("lostFiles/geometry")).toByteArray();
    if (geometry.isEmpty() || !restoreGeometry(geometry)) {
        // Most of the screen, but not stretched across a very wide one, and never more than there is.
        const int width = std::min(room.width(), std::max(std::min(1120, room.width()), std::min(room.width() * 3 / 4, 2000)));
        const int height = std::min(room.height(), std::max(std::min(720, room.height()), room.height() * 17 / 20));
        const QSize frame = frameGeometry().size() - size();
        resize(width - frame.width(), height - frame.height());
        move(room.center() - QPoint(width / 2, height / 2));
    }
    keepOnScreen(this);
    const QByteArray panes = settings.value(QStringLiteral("lostFiles/panes")).toByteArray();
    if (panes.isEmpty() || !m_split->restoreState(panes)) {
        // Kinds and folders get a fifth, the preview about a third: room for long names and a picture.
        const int across = std::max(m_split->width(), width() - 40);
        const int left = std::clamp(across / 5, 200, 440);
        const int preview = std::clamp(across * 3 / 10, 240, 1000);
        m_split->setSizes({left, std::max(240, across - left - preview), preview});
    }
}

void LostFilesDialog::stopWorkers()
{
    stopViewer();
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
    if (m_copyFinder)
        m_copyFinder->cancel();
    if (m_checker)
        m_checker->stop();
    for (QThread *t : {m_quickThread, m_scanThread, m_copyThread, m_saveThread, m_thumbThread, m_copyFinderThread, m_checkerThread}) {
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
    m_copyFinder = nullptr;
    m_checker = nullptr;
    m_quickThread = m_scanThread = m_copyThread = m_saveThread = m_thumbThread = m_copyFinderThread = m_checkerThread = nullptr;
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
    m_facts.clear();
    m_viewerIndex = -1;
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
    connect(m_thumbs, &PreviewLoader::dropped, this, [this](int index, int pixels) {
        if (pixels == kThumbnail)
            m_model->forgetAsked(index);
    });
    connect(m_thumbs, &PreviewLoader::ready, this,
            [this](int index, int pixels, const QImage &image, const QString &text, const QStringList &facts, bool failed) {
        if (pixels == kThumbnail) {
            if (!image.isNull())
                m_model->setThumbnail(index, QPixmap::fromImage(image));
        } else if (index == m_previewIndex) {
            showPreviewResult(image, text, facts, failed);
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
    // Done: the bar and what it was doing only take room from the files now.
    m_bar->hide();
    m_phase->hide();
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
    m_organise->setEnabled(m_model->fileCount() > 0);
    m_findCopies->setEnabled(m_model->fileCount() > 1);
}

void LostFilesDialog::findCopies()
{
    if (m_copyFinder || m_scanning)
        return;
    QVector<int> indexes(m_model->fileCount());
    std::iota(indexes.begin(), indexes.end(), 0);
    m_copyFinder = new lost::CopyFinder(m_source, m_model->files(), indexes);
    m_findCopies->setEnabled(false);
    m_organise->setText(tr("Finding copies…"));
    connect(m_copyFinder, &lost::CopyFinder::progress, this, [this](quint64 done, quint64 total) {
        m_organise->setText(tr("Finding copies… %1%").arg(total ? done * 100 / total : 0));
    });
    connect(m_copyFinder, &lost::CopyFinder::finished, this, [this](const QHash<int, int> &copyOf, quint64 bytes, bool stopped) {
        if (m_copyFinderThread)
            m_copyFinderThread->quit();
        m_copyFinder = nullptr;
        m_organise->setText(m_checker ? m_organise->text() : tr("Organise"));
        m_findCopies->setEnabled(true);
        if (stopped)
            return;
        m_model->setCopies(copyOf);
        m_hideCopies->setText(copyOf.isEmpty() ? tr("No copies") : tr("Hide copies (%1, %2)").arg(copyOf.size()).arg(formatSize(bytes)));
        m_hideCopies->setEnabled(!copyOf.isEmpty());
        m_hideCopies->show();
        if (m_previewIndex >= 0)
            showPreview();
    });
    m_copyFinderThread = startOnThread(this, m_copyFinder);
}

std::optional<bool> LostFilesDialog::opened(int type, const QImage &image, const QString &text, const QStringList &facts, bool failed)
{
    if (failed)
        return false;
    if (type < 0 || type >= filetypes::types().size())
        return {};
    switch (filetypes::types()[type].category) {
    case filetypes::Category::Picture:
        return !image.isNull();
    case filetypes::Category::Document:
        return !image.isNull() || !text.isEmpty() || !facts.isEmpty(); // a PDF's page, a document's words
    case filetypes::Category::Archive:
    case filetypes::Category::Music:
    case filetypes::Category::Video:
        return !image.isNull() || !text.isEmpty();
    case filetypes::Category::Other:
        break;
    }
    return {}; // a database or the like: the previews can't tell
}

void LostFilesDialog::checkTheyOpen()
{
    if (m_checker || m_scanning)
        return;
    m_checkQueue.clear();
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const int i = m_model->fileIndex(row);
        if (i >= 0 && !m_model->file(i).nameOnly && !m_model->checkedOpen(i))
            m_checkQueue << i;
    }
    if (m_checkQueue.isEmpty())
        return;
    m_checkDone = 0;
    m_checkTotal = int(m_checkQueue.size());
    m_checker = new PreviewLoader(m_source);
    connect(m_checker, &PreviewLoader::ready, this,
            [this](int index, int, const QImage &image, const QString &text, const QStringList &facts, bool failed) {
                if (index != m_checkWaiting)
                    return;
                m_checkWaiting = -1;
                const std::optional<bool> opens = opened(m_model->file(index).type, image, text, facts, failed);
                if (opens.has_value())
                    m_model->setOpens(index, *opens);
                if (!image.isNull())
                    m_model->setThumbnail(index, QPixmap::fromImage(image));
                ++m_checkDone;
                checkNext();
            });
    connect(m_checker, &PreviewLoader::dropped, this, [this](int index) {
        if (index == m_checkWaiting) { // let go of unanswered: asked again
            m_checkWaiting = -1;
            m_checkQueue.prepend(index);
            checkNext();
        }
    });
    m_checkerThread = startOnThread(this, m_checker);
    m_checkOpen->setText(tr("Stop Checking"));
    checkNext();
}

void LostFilesDialog::checkNext()
{
    if (!m_checker)
        return;
    if (m_checkQueue.isEmpty()) {
        stopChecking();
        return;
    }
    m_organise->setText(tr("Checking %1 of %2…").arg(m_checkDone + 1).arg(m_checkTotal));
    m_checkWaiting = m_checkQueue.takeFirst();
    m_checker->want(m_checkWaiting, m_model->file(m_checkWaiting), 128);
}

void LostFilesDialog::stopChecking()
{
    if (m_checker)
        m_checker->stop();
    if (m_checkerThread) {
        m_checkerThread->quit();
        m_checkerThread->wait();
    }
    m_checker = nullptr;
    m_checkerThread = nullptr;
    m_checkWaiting = -1;
    m_checkQueue.clear();
    m_checkOpen->setText(tr("Check They Open"));
    m_organise->setText(m_copyFinder ? m_organise->text() : tr("Organise"));
    const int broken = m_model->brokenCount();
    m_hideBroken->setText(broken == 0 ? tr("All checked ones open") : tr("Hide ones that don't open (%1)").arg(broken));
    m_hideBroken->setEnabled(broken > 0);
    m_hideBroken->show();
    if (m_previewIndex >= 0)
        showPreview();
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
    filter.hideSmallPictures = m_hideSmall->isChecked();
    filter.hideCopies = m_hideCopies->isChecked();
    filter.hideBroken = m_hideBroken->isChecked();
    filter.order = LostFilter::Order(m_sort->currentData().toInt());
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
        static const QRegularExpression madeUp(QStringLiteral("^\\S+ \\d{6}\\.[^.]+$"));
        html += (madeUp.match(f.name).hasMatch()
                     ? tr("Found by what's inside it, %1 into the drive. Its real name and folder aren't known, so it has a made-up name.")
                     : tr("Found by what's inside it, %1 into the drive. Its real name and folder aren't known; this name is from what it "
                          "says inside it."))
                    .arg(formatSize(where));
        if (f.inside > 0)
            html += QStringLiteral("<br>") + tr("Its own date: %1").arg(QLocale().toString(QDateTime::fromMSecsSinceEpoch(f.inside), QLocale::LongFormat));
    } else {
        html += (f.deleted ? tr("Was in %1") : tr("In %1")).arg(f.folder.toHtmlEscaped());
        if (f.nameGuessed)
            html += QStringLiteral("<br>") + tr("Deleting it took away the first letter of its name; _ stands in for it.");
        if (f.piecesGuessed)
            html += QStringLiteral("<br>") + tr("The file system doesn't say where all of it is anymore, so it's taken to follow on "
                                                 "from where it started. That's right for most files.");
    }
    if (const QString saved = m_model->savedTo(i); !saved.isEmpty())
        html += QStringLiteral("<br>") + tr("Saved to %1").arg(saved.toHtmlEscaped());
    if (const int original = m_model->copyOf(i); original >= 0)
        html += QStringLiteral("<br>") + tr("The same as \"%1\", byte for byte.").arg(m_model->file(original).name.toHtmlEscaped());
    if (!m_model->opens(i))
        html += QStringLiteral("<br><span style=\"color:#ff3355\">●</span> ") + tr("It didn't open when it was checked: it's probably damaged.");
    html += QStringLiteral("<br><br><span style=\"color:%1\">●</span> %2").arg(colour.name(), conditionWords(f).toHtmlEscaped());
    if (!f.note.isEmpty())
        html += QStringLiteral("<br>") + f.note.toHtmlEscaped();
    const QVector<QPair<QString, QString>> times = driveTimes(f);
    m_viewButton->setEnabled(!f.nameOnly);
    if (!times.isEmpty())
        html += QStringLiteral("<br><br><b>%1</b>").arg(tr("On the drive")) + detailsTable(times);
    m_detailsHtml = html;
    m_details->setText(html);
    if (f.nameOnly)
        return; // nothing of it to show
    // The picture and the words come from the preview helper; until then, a moment.
    m_previewImage->setText(tr("Loading…"));
    m_previewPicture = QImage();
    m_previewAsked = previewPixels();
    if (m_thumbs)
        m_thumbs->want(i, f, m_previewAsked);
}

int LostFilesDialog::previewPixels() const
{
    const QSize room = m_previewImage->size() * m_previewImage->devicePixelRatioF();
    return std::clamp(std::max(room.width(), room.height()), kPreview, kPreviewMost);
}

// The preview's room changed (the window or a pane was resized): the picture is fitted again, and
// asked for bigger when it was made for a lot less room than there is now.
void LostFilesDialog::fitPreview()
{
    if (m_previewIndex < 0 || m_previewPicture.isNull())
        return;
    const qreal ratio = m_previewImage->devicePixelRatioF();
    const QSize room = m_previewImage->size() * ratio;
    QImage fitted = m_previewPicture;
    if (room.width() > 16 && room.height() > 16 && (fitted.width() > room.width() || fitted.height() > room.height()))
        fitted = fitted.scaled(room, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPixmap pixmap = QPixmap::fromImage(fitted);
    pixmap.setDevicePixelRatio(ratio);
    m_previewImage->setPixmap(pixmap);
    const int wanted = previewPixels();
    if (m_thumbs && wanted > m_previewAsked * 5 / 4 && std::max(m_previewPicture.width(), m_previewPicture.height()) >= m_previewAsked) {
        m_previewAsked = wanted;
        m_thumbs->want(m_previewIndex, m_model->file(m_previewIndex), wanted);
    }
}

bool LostFilesDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_previewImage && event->type() == QEvent::Resize && m_previewResize)
        m_previewResize->start();
    if (watched == m_view && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && m_view->currentIndex().isValid()) {
            openViewer(m_model->fileIndex(m_view->currentIndex().row()));
            return true;
        }
    }
    if (watched == m_view->viewport()) {
        // A press on a picked file, then a move: that's a drag out. Elsewhere the view picks.
        if (event->type() == QEvent::MouseButtonPress) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const QModelIndex under = m_view->indexAt(mouse->position().toPoint());
            m_dragFrom = mouse->button() == Qt::LeftButton && under.isValid() && m_view->selectionModel()->isSelected(under)
                           ? mouse->position().toPoint()
                           : QPoint(-1, -1);
        } else if (event->type() == QEvent::MouseMove && m_dragFrom.x() >= 0) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if ((mouse->buttons() & Qt::LeftButton)
                && (mouse->position().toPoint() - m_dragFrom).manhattanLength() >= QApplication::startDragDistance()) {
                m_dragFrom = QPoint(-1, -1);
                dragOut();
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            m_dragFrom = QPoint(-1, -1);
        }
    }
    return QDialog::eventFilter(watched, event);
}

QString LostFilesDialog::lookCopy(int index)
{
    if (!m_looks)
        m_looks = std::make_unique<lost::LookCopies>(m_diskDevice);
    const lost::Found &f = m_model->file(index);
    QString why;
    const auto make = [&] {
        if (f.size < (32ull << 20))
            return m_looks->make(m_source, f, index, &why);
        QProgressDialog progress(tr("Making a copy of \"%1\" to look at…").arg(f.name), tr("Stop"), 0, 1000, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(300);
        return m_looks->make(m_source, f, index, &why, [&progress](quint64 done, quint64 total) {
            progress.setValue(int(done * 1000 / std::max<quint64>(total, 1)));
            QCoreApplication::processEvents();
            return !progress.wasCanceled();
        });
    };
    QString copy = make();
    if (copy.isEmpty() && !why.isEmpty() && m_looks->folderFor(f.size, nullptr).isEmpty()) {
        // Nowhere safe for it: a folder on another drive, picked now, does for the rest of the time.
        QMessageBox box(QMessageBox::Information, windowTitle(), why, QMessageBox::Cancel, this);
        QPushButton *pick = box.addButton(tr("Choose a Folder…"), QMessageBox::AcceptRole);
        box.exec();
        if (box.clickedButton() != pick)
            return {};
        const QString folder = QFileDialog::getExistingDirectory(this, tr("A Folder for Copies to Look At"), QDir::homePath());
        if (folder.isEmpty())
            return {};
        if (!m_looks->setFolder(folder, &why)) {
            QMessageBox::warning(this, windowTitle(), why);
            return {};
        }
        why.clear();
        copy = make();
    }
    if (copy.isEmpty() && !why.isEmpty())
        QMessageBox::warning(this, windowTitle(), why);
    return copy;
}

void LostFilesDialog::openInProgram(int index)
{
    const lost::Found &f = m_model->file(index);
    if (!lost::LookCopies::safeToOpen(f.name)) {
        QMessageBox::information(this, windowTitle(),
                                 tr("\"%1\" isn't opened in another program: opening a program or a script can run it. Save it instead, "
                                    "and check it with Scan for Viruses before using it.")
                                     .arg(f.name));
        return;
    }
    const QString copy = lookCopy(index);
    if (!copy.isEmpty() && lost::LookCopies::hasMacros(copy)) {
        QMessageBox::information(this, windowTitle(),
                                 tr("\"%1\" has macros in it (small programs), which could run when it's opened in its own program. "
                                    "Look at it here with View, or save it and check it with Scan for Viruses first.")
                                     .arg(f.name));
        return;
    }
    if (!copy.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(copy)))
        QMessageBox::information(this, windowTitle(), tr("There's no program on this computer for that kind of file."));
}

void LostFilesDialog::openViewer(int index)
{
    if (index < 0 || index >= m_model->fileCount() || m_model->file(index).nameOnly)
        return;
    if (!m_viewer)
        startViewer();
    viewerShow(index);
}

void LostFilesDialog::startViewer()
{
    if (!m_looks)
        m_looks = std::make_unique<lost::LookCopies>(m_diskDevice);
    m_viewerReadable = m_looks->folders();
    m_viewerReady = false;
    m_viewerHeard.clear();
    m_viewerWaiting.clear();
    auto *process = new QProcess(this);
    m_viewer = process;
    process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] { viewerSaid(process->readAllStandardOutput()); });
    connect(process, &QProcess::finished, this, [this, process] {
        process->deleteLater();
        if (process != m_viewer)
            return;
        const bool neverStarted = !m_viewerReady;
        m_viewer = nullptr;
        m_viewerReady = false;
        // The sandbox didn't work here (some systems don't allow one): it runs without.
        if (neverStarted && m_viewerSandboxed && m_viewerIndex >= 0) {
            qCInfo(lcOps).noquote() << "Find Lost Files: the viewer couldn't start in its sandbox here; it runs without one";
            m_viewerSandboxed = false;
            openViewer(m_viewerIndex);
        } else {
            m_viewerIndex = -1;
        }
    });
    const QStringList command = viewer::command(m_viewerSandboxed, m_viewerReadable);
    process->start(command.first(), command.mid(1));
    // It looks like DiskForge does.
    viewer::Message look;
    look.type = viewer::Message::Look;
    look.style = QApplication::style()->name();
    look.palette = QApplication::palette();
    sendToViewer(viewer::encode(look));
    // A sandbox that hangs rather than failing is given up on after a while, the same way.
    QTimer::singleShot(15000, process, [this, process] {
        if (process == m_viewer && !m_viewerReady)
            process->kill();
    });
}

void LostFilesDialog::stopViewer()
{
    if (!m_viewer)
        return;
    QProcess *process = m_viewer;
    m_viewer = nullptr;
    m_viewerReady = false;
    m_viewerIndex = -1;
    m_viewerShown = -1;
    process->disconnect(this);
    // With nothing more to read, it closes by itself.
    process->closeWriteChannel();
    if (!process->waitForFinished(1500)) {
        process->kill();
        process->waitForFinished(1000);
    }
    process->deleteLater();
}

void LostFilesDialog::sendToViewer(const QByteArray &message)
{
    if (m_viewer && m_viewerReady)
        m_viewer->write(message);
    else
        m_viewerWaiting << message;
}

void LostFilesDialog::viewerSaid(const QByteArray &said)
{
    m_viewerHeard += said;
    viewer::Message m;
    bool bad = false;
    while (viewer::decode(m_viewerHeard, m, &bad)) {
        if (m.type != viewer::Message::AskFor)
            continue;
        switch (m.ask) {
        case viewer::Ask::Ready:
            m_viewerReady = true;
            for (const QByteArray &waiting : std::as_const(m_viewerWaiting))
                m_viewer->write(waiting);
            m_viewerWaiting.clear();
            break;
        case viewer::Ask::Previous:
        case viewer::Ask::Next: {
            const int to = neighbour(m_viewerIndex, m.ask == viewer::Ask::Next ? 1 : -1);
            if (to < 0)
                break;
            // The list here follows, so its preview and details are the same file.
            const QModelIndex row = m_model->index(m_model->rowOf(to));
            m_view->setCurrentIndex(row);
            m_view->scrollTo(row);
            viewerShow(to);
            break;
        }
        case viewer::Ask::Save:
            if (m_viewerIndex >= 0 && !m_saving && !m_scanning) {
                raise();
                activateWindow();
                const int index = m_viewerIndex;
                const QString start = m_savedTo.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                                                          : QFileInfo(m_savedTo).absolutePath();
                const QString folder = QFileDialog::getExistingDirectory(this, tr("Save \"%1\" Where?").arg(m_model->file(index).name), start);
                if (!folder.isEmpty())
                    saveFiles({index}, folder);
            }
            break;
        case viewer::Ask::Open:
            if (m_viewerIndex >= 0)
                openInProgram(m_viewerIndex);
            break;
        case viewer::Ask::Shown:
            m_viewerShown = m_viewerIndex;
            break;
        }
    }
    if (bad)
        stopViewer();
}

void LostFilesDialog::viewerShow(int index)
{
    const lost::Found &f = m_model->file(index);
    const QString copy = lookCopy(index);
    if (copy.isEmpty())
        return;
    // A copy where its sandbox can't see it (a folder picked since it started): it starts again.
    const bool seen = std::any_of(m_viewerReadable.cbegin(), m_viewerReadable.cend(),
                                  [&copy](const QString &folder) { return copy.startsWith(folder + QLatin1Char('/')); });
    if (!seen && m_viewer) {
        stopViewer();
        startViewer();
    }
    if (!m_viewer)
        startViewer();
    viewer::Message m;
    m.type = viewer::Message::ShowFile;
    m.show.kind = viewer::kindFor(f.type, f.name);
    m.show.type = f.type;
    m.show.path = copy;
    m.show.name = f.name;
    const int row = m_model->rowOf(index);
    m.show.where = row >= 0 ? tr("%1 of %2").arg(row + 1).arg(m_model->rowCount()) : QString();
    m.show.previous = neighbour(index, -1) >= 0;
    m.show.next = neighbour(index, 1) >= 0;
    m.show.openable = lost::LookCopies::safeToOpen(f.name) && !lost::LookCopies::hasMacros(copy);
    m.show.facts = factsFor(index);
    m_viewerIndex = index;
    sendToViewer(viewer::encode(m));
}

int LostFilesDialog::neighbour(int index, int step) const
{
    const int row = m_model->rowOf(index);
    if (row < 0)
        return -1;
    for (int r = row + step; r >= 0 && r < m_model->rowCount(); r += step) {
        const int i = m_model->fileIndex(r);
        if (i >= 0 && !m_model->file(i).nameOnly)
            return i;
    }
    return -1;
}

bool LostFilesDialog::sourceMounted() const
{
    for (const Disk &d : m_udisks->disks()) {
        if (d.device != m_diskDevice)
            continue;
        for (const Volume &v : d.volumes) {
            if (!v.mounts().isEmpty())
                return true;
        }
    }
    return false;
}

// Copies of the picked files, dragged to a folder or the desktop: the file manager puts them
// there. Big ones are better saved, which shows how it's going.
void LostFilesDialog::dragOut()
{
    const QVector<int> picked = selectedIndexes();
    if (picked.isEmpty())
        return;
    quint64 total = 0;
    for (int i : picked)
        total += m_model->file(i).size;
    if (total > (2ull << 30)) {
        QMessageBox::information(this, windowTitle(), tr("That's a lot to drag (%1). Tick them and use Save Ticked Files To… instead.").arg(formatSize(total)));
        return;
    }
    // A drop onto the drive they're on would write over the space they're found in.
    if (!m_warnedDrop && !m_diskDevice.isEmpty() && sourceMounted()) {
        m_warnedDrop = true;
        m_saveStrip->show();
        m_saveBar->hide();
        m_savePhase->hide();
        m_openFolder->hide();
        m_hideSaved->show();
        m_saveStatus->setText(tr("Drop them anywhere but on %1 itself: putting them back on the drive they were lost on can overwrite other "
                                 "files that are still to be found.")
                                  .arg(m_title));
    }
    QList<QUrl> urls;
    for (int i : picked) {
        const QString copy = lookCopy(i);
        if (copy.isEmpty())
            return;
        urls << QUrl::fromLocalFile(copy);
    }
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    auto *drag = new QDrag(m_view);
    drag->setMimeData(mime);
    const QModelIndex first = m_view->currentIndex();
    const QVariant look = first.data(Qt::DecorationRole);
    QPixmap pixmap = look.typeId() == QMetaType::QPixmap ? look.value<QPixmap>() : look.value<QIcon>().pixmap(48, 48);
    if (!pixmap.isNull())
        drag->setPixmap(pixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    drag->exec(Qt::CopyAction, Qt::CopyAction);
}

// The times the file system kept: FAT keeps only the day it was last opened.
QVector<QPair<QString, QString>> LostFilesDialog::driveTimes(const lost::Found &f) const
{
    const auto when = [](qint64 ms, bool dayOnly = false) {
        const QDateTime t = QDateTime::fromMSecsSinceEpoch(ms);
        return dayOnly ? QLocale().toString(t.date(), QLocale::LongFormat)
                       : QLocale().toString(t.date(), QLocale::LongFormat) + QStringLiteral(", ") + QLocale().toString(t.time(), QLocale::ShortFormat);
    };
    QVector<QPair<QString, QString>> times;
    if (f.created > 0)
        times.append({tr("Created"), when(f.created)});
    if (f.modified > 0)
        times.append({tr("Changed"), when(f.modified)});
    if (f.accessed > 0)
        times.append({tr("Last opened"), when(f.accessed, f.accessedDayOnly)});
    if (f.changed > 0 && f.changed != f.modified)
        times.append({tr("Its entry changed"), when(f.changed)});
    if (f.deletedAt > 0)
        times.append({tr("Deleted"), when(f.deletedAt)});
    return times;
}

QStringList LostFilesDialog::factsFor(int index) const
{
    const lost::Found &f = m_model->file(index);
    QStringList facts;
    facts << tr("Size") + QLatin1Char('\t') + (f.nameOnly ? tr("not known") : formatSize(f.size));
    if (f.origin == lost::Origin::FileSystem)
        facts << (f.deleted ? tr("Was in") : tr("In")) + QLatin1Char('\t') + f.folder;
    for (const auto &[label, value] : driveTimes(f))
        facts << label + QLatin1Char('\t') + value;
    if (const QString saved = m_model->savedTo(index); !saved.isEmpty())
        facts << tr("Saved to") + QLatin1Char('\t') + saved;
    // What the file itself said, once its preview did.
    facts << m_facts.value(index);
    return facts;
}

QString LostFilesDialog::detailsTable(const QVector<QPair<QString, QString>> &rows) const
{
    const QString quiet = palette().color(QPalette::PlaceholderText).name();
    QString html = QStringLiteral("<table cellspacing=\"0\" cellpadding=\"1\">");
    for (const auto &[label, value] : rows)
        html += QStringLiteral("<tr><td style=\"color:%1; padding-right:12px\">%2</td><td>%3</td></tr>")
                    .arg(quiet, label.toHtmlEscaped(), value.toHtmlEscaped());
    return html + QStringLiteral("</table>");
}

void LostFilesDialog::showPreviewResult(const QImage &image, const QString &text, const QStringList &facts, bool failed)
{
    // What the file says about itself, under what the drive said (and in the viewer, when it
    // shows this one).
    m_facts.insert(m_previewIndex, facts);
    if (m_viewer && m_viewerIndex == m_previewIndex) {
        viewer::Message m;
        m.type = viewer::Message::Facts;
        m.show.facts = factsFor(m_previewIndex);
        sendToViewer(viewer::encode(m));
    }
    if (!facts.isEmpty()) {
        QVector<QPair<QString, QString>> rows;
        for (const QString &fact : facts)
            rows.append({fact.section(QLatin1Char('\t'), 0, 0), fact.section(QLatin1Char('\t'), 1)});
        m_details->setText(m_detailsHtml + QStringLiteral("<br><br><b>%1</b>").arg(tr("In the file")) + detailsTable(rows));
    }
    const lost::Found &f = m_model->file(m_previewIndex);
    const filetypes::Category category = f.type >= 0 ? filetypes::types()[f.type].category : filetypes::Category::Other;
    if (!image.isNull()) {
        // Bigger than the room there is (a PDF page in a narrow window): scaled down to fit. Never
        // blown up past its own size.
        m_previewPicture = image;
        const int asked = m_previewAsked;
        fitPreview();
        m_previewAsked = asked;
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
    const QVector<int> indexes = m_model->tickedIndexes();
    if (indexes.isEmpty())
        return;
    const QString start = m_savedTo.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                                              : QFileInfo(m_savedTo).absolutePath();
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Save the Ticked Files Where?"), start);
    if (!folder.isEmpty())
        saveFiles(indexes, folder);
}

void LostFilesDialog::saveSelected()
{
    const QVector<int> indexes = selectedIndexes();
    if (indexes.isEmpty())
        return;
    const QString start = m_savedTo.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                                              : QFileInfo(m_savedTo).absolutePath();
    const QString folder = QFileDialog::getExistingDirectory(
        this, indexes.size() == 1 ? tr("Save \"%1\" Where?").arg(m_model->file(indexes[0]).name) : tr("Save These Files Where?"), start);
    if (!folder.isEmpty())
        saveFiles(indexes, folder);
}

// The picked files that have something to save (not the ones with only a name left).
QVector<int> LostFilesDialog::selectedIndexes() const
{
    QVector<int> out;
    for (const QModelIndex &index : m_view->selectionModel()->selectedIndexes()) {
        const int i = m_model->fileIndex(index.row());
        if (i >= 0 && !m_model->file(i).nameOnly && !out.contains(i))
            out << i;
    }
    std::sort(out.begin(), out.end());
    return out;
}

void LostFilesDialog::showFilesMenu(const QPoint &at)
{
    // A right-click on a file that isn't picked yet picks just that one, the way file managers do.
    const QModelIndex under = m_view->indexAt(at);
    if (under.isValid() && !m_view->selectionModel()->isSelected(under))
        m_view->selectionModel()->select(under, QItemSelectionModel::ClearAndSelect);
    const QVector<int> picked = selectedIndexes();
    if (picked.isEmpty())
        return;
    QMenu menu(this);
    if (picked.size() == 1) {
        const int one = picked.first();
        QAction *view = menu.addAction(QIcon::fromTheme(QStringLiteral("document-preview")), tr("View"));
        QFont bold = view->font();
        bold.setBold(true);
        view->setFont(bold);
        connect(view, &QAction::triggered, this, [this, one] { openViewer(one); });
        QAction *open = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), tr("Open in Its Own Program"));
        if (!lost::LookCopies::safeToOpen(m_model->file(one).name))
            open->setToolTip(tr("Programs and scripts aren't opened: they could run"));
        connect(open, &QAction::triggered, this, [this, one] { openInProgram(one); });
        menu.addSeparator();
    }
    QAction *save = menu.addAction(QIcon::fromTheme(QStringLiteral("document-save")),
                                   picked.size() == 1 ? tr("Save This File…") : tr("Save These %1 Files…").arg(picked.size()));
    save->setEnabled(!m_saving && !m_scanning);
    if (m_scanning)
        save->setToolTip(tr("Stop the search first, or wait until it's done."));
    connect(save, &QAction::triggered, this, &LostFilesDialog::saveSelected);
    menu.addSeparator();
    QAction *tick = menu.addAction(picked.size() == 1 ? tr("Tick It") : tr("Tick Them"));
    QAction *untick = menu.addAction(picked.size() == 1 ? tr("Untick It") : tr("Untick Them"));
    for (QAction *a : {tick, untick}) {
        connect(a, &QAction::triggered, this, [this, a, tick] {
            for (const QModelIndex &index : m_view->selectionModel()->selectedIndexes())
                m_model->setData(index, a == tick ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
        });
    }
    menu.exec(m_view->viewport()->mapToGlobal(at));
}

void LostFilesDialog::saveTo(const QString &folder)
{
    saveFiles(m_model->tickedIndexes(), folder);
}

void LostFilesDialog::saveFiles(const QVector<int> &indexes, const QString &folder)
{
    QVector<lost::Found> files;
    for (int i : indexes)
        files << m_model->file(i);
    if (files.isEmpty() || m_saving)
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
    m_saveStrip->show();
    m_saveBar->setValue(0);
    m_saveBar->show();
    m_savePhase->clear();
    m_savePhase->show();
    m_saveStatus->setText(files.size() == 1 ? tr("Saving \"%1\" to %2…").arg(files[0].name, folder)
                                            : tr("Saving %1 files to %2…").arg(files.size()).arg(folder));
    m_openFolder->hide();
    m_hideSaved->hide();
    m_saver = new lost::Saver(m_source, files, folder);
    connect(m_saver, &lost::Saver::progress, this, [this](quint64 done, quint64 total) { m_saveMeter.update(tr("Saving"), done, total); });
    connect(m_saver, &lost::Saver::fileSaved, this, [this, indexes](int position, const QString &path) {
        if (position >= 0 && position < indexes.size())
            m_model->markSaved(indexes[position], path);
    });
    connect(m_saver, &lost::Saver::finished, this, [this, indexes](int saved, int failed, const QString &to, const QString &message) {
        m_saving = false;
        if (m_saveThread)
            m_saveThread->quit();
        m_saver = nullptr;
        m_savedTo = to;
        QString text = saved == 1 && failed == 0 ? tr("Saved one file to %1.").arg(to)
                     : failed == 0               ? tr("Saved %1 files to %2.").arg(saved).arg(to)
                                                 : tr("Saved %1 files to %2. %3 couldn't be saved.").arg(saved).arg(to).arg(failed);
        if (!message.isEmpty())
            text = message + QLatin1Char(' ') + text;
        m_saveStatus->setText(text + QLatin1Char(' ') + tr("\"What was saved.txt\" there lists them, and any that may be damaged. "
                                                             "Everything found is still here, to save more."));
        m_saveBar->hide();
        m_savePhase->hide();
        m_openFolder->show();
        m_hideSaved->show();
        updateTicked();
        if (indexes.contains(m_previewIndex))
            showPreview(); // its details now say where it went
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
    } else if (m_model->fileCount() > 0) {
        if (QMessageBox::question(this, windowTitle(),
                                  tr("Close Find Lost Files? What it found isn't kept: to save more later, it would have to look "
                                     "through the drive again."),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;
    }
    stopWorkers();
    QDialog::reject();
}

