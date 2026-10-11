// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usagedialog.h"

#include "format.h"
#include "jobui.h"
#include "squarify.h"
#include "theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStorageInfo>
#include <QToolTip>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QTextDocument>

#include <climits>
#include <cmath>

#include <algorithm>

namespace {

constexpr int kHeader = 16; // name strip on top of a folder that shows what's inside

QVector<double> sizesOf(const UsageNode &node)
{
    QVector<double> sizes;
    sizes.reserve(node.children.size());
    for (const UsageNode &c : node.children)
        sizes << double(c.size);
    return sizes;
}

QString countText(quint64 files)
{
    return QObject::tr("%n file(s)", nullptr, int(std::min<quint64>(files, INT_MAX)));
}

} // namespace

TreemapWidget::TreemapWidget(QWidget *parent)
    : QWidget(parent)
{
    connect(&Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
    setMouseTracking(true);
    setMinimumSize(240, 180);
}

void TreemapWidget::setNode(const UsageNode *node)
{
    m_node = node;
    m_hover = -1;
    layoutBoxes();
    update();
}

void TreemapWidget::resizeEvent(QResizeEvent *)
{
    layoutBoxes();
}

void TreemapWidget::layoutBoxes()
{
    m_boxes = m_node ? squarify(sizesOf(*m_node), QRectF(rect()).adjusted(0, 0, -1, -1)) : QVector<QRectF>();
}

QColor TreemapWidget::colour(int child) const
{
    const UsageNode &c = m_node->children[child];
    const Theme &theme = Theme::instance();
    if (!c.isDir) // the folded "n smaller files", or a single file
        return theme.color(c.files > 1 ? Theme::Role::UsageSmallFiles : Theme::Role::UsageFile);
    return theme.usageColor(child);
}

void TreemapWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Base));
    if (!m_node)
        return;
    const QFontMetrics fm = fontMetrics();
    for (int i = 0; i < m_boxes.size(); ++i) {
        const QRectF box = m_boxes[i];
        if (box.width() < 1 || box.height() < 1)
            continue;
        const UsageNode &c = m_node->children[i];
        QColor fill = colour(i);
        if (i == m_hover)
            fill = fill.lighter(112);
        p.fillRect(box, fill);

        // A big folder shows its own contents faintly inside, so the next level is visible.
        const bool nested = c.isDir && !c.children.isEmpty() && box.width() > 90 && box.height() > 70;
        if (nested) {
            const QRectF inner = box.adjusted(3, kHeader, -3, -3);
            const QVector<QRectF> parts = squarify(sizesOf(c), inner);
            p.setPen(fill.darker(118));
            for (const QRectF &r : parts) {
                if (r.width() >= 2 && r.height() >= 2)
                    p.drawRect(r.adjusted(0, 0, -1, -1));
            }
        }
        p.setPen(palette().color(QPalette::Base));
        p.drawRect(box);

        if (box.width() > 50 && box.height() > fm.height() + 2) {
            p.setPen(Theme::textOn(fill));
            const QRectF text = box.adjusted(4, 1, -4, -1);
            const int width = int(text.width());
            const QString name = fm.elidedText(c.name, Qt::ElideMiddle, width);
            const QString size = formatSize(c.size);
            if (nested || box.height() < 2.2 * fm.height())
                p.drawText(text, Qt::AlignLeft | Qt::AlignTop, fm.elidedText(c.name + QStringLiteral("  ") + size, Qt::ElideMiddle, width));
            else
                p.drawText(text, Qt::AlignLeft | Qt::AlignTop, name + QLatin1Char('\n') + fm.elidedText(size, Qt::ElideRight, width));
        }
    }
}

int TreemapWidget::childAt(const QPointF &pos) const
{
    for (int i = 0; i < m_boxes.size(); ++i) {
        if (m_boxes[i].contains(pos))
            return i;
    }
    return -1;
}

void TreemapWidget::mousePressEvent(QMouseEvent *event)
{
    const int i = childAt(event->position());
    if (i < 0)
        return;
    if (event->button() == Qt::LeftButton)
        emit clicked(i);
    else if (event->button() == Qt::RightButton)
        emit menuRequested(i, event->globalPosition().toPoint());
}

void TreemapWidget::mouseMoveEvent(QMouseEvent *event)
{
    const int i = childAt(event->position());
    if (i != m_hover) {
        m_hover = i;
        setCursor(i >= 0 && m_node->children[i].isDir && !m_node->children[i].children.isEmpty() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void TreemapWidget::leaveEvent(QEvent *)
{
    m_hover = -1;
    update();
}

bool TreemapWidget::event(QEvent *event)
{
    if (event->type() != QEvent::ToolTip)
        return QWidget::event(event);
    auto *help = static_cast<QHelpEvent *>(event);
    const int i = childAt(help->pos());
    if (i < 0 || !m_node) {
        QToolTip::hideText();
        return true;
    }
    const UsageNode &c = m_node->children[i];
    QString text = QStringLiteral("%1\n%2").arg(c.name, formatSize(c.size));
    if (c.isDir)
        text += QStringLiteral(", ") + countText(c.files);
    if (m_node->size)
        text += tr("\n%1% of this folder").arg(QString::number(100.0 * c.size / m_node->size, 'f', 1));
    QToolTip::showText(help->globalPos(), Qt::convertFromPlainText(text, Qt::WhiteSpaceNormal), this);
    return true;
}

UsageDialog::UsageDialog(const QString &mountPoint, const QString &title, QWidget *parent)
    : QDialog(parent)
    , m_root(mountPoint)
    , m_crumbs(new QLabel)
    , m_up(new QPushButton(tr("Up")))
    , m_map(new TreemapWidget)
    , m_list(new QTreeWidget)
    , m_status(new QLabel)
    , m_busy(new QProgressBar)
{
    setWindowTitle(tr("Disk Usage: %1").arg(title));
    m_crumbs->setTextFormat(Qt::RichText);
    m_up->setIcon(QIcon::fromTheme(QStringLiteral("go-up")));
    m_up->setEnabled(false);
    m_list->setHeaderLabels({tr("Name"), tr("Size"), tr("Share"), tr("Files")});
    m_list->setRootIsDecorated(false);
    m_list->setAlternatingRowColors(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_busy->setRange(0, 0);
    m_busy->setMaximumHeight(10);
    m_busy->setTextVisible(false);
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText); // folder names come from the drive

    auto *top = new QHBoxLayout;
    top->addWidget(m_up);
    top->addWidget(m_crumbs, 1);
    auto *split = new QSplitter;
    split->addWidget(m_map);
    split->addWidget(m_list);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setSizes({580, 400});

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(split, 1);
    layout->addWidget(m_busy);
    layout->addWidget(m_status);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *rescan = box->addButton(tr("Scan Again"), QDialogButtonBox::ActionRole);
    connect(rescan, &QPushButton::clicked, this, &UsageDialog::scan);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);

    connect(m_up, &QPushButton::clicked, this, [this] { showLevel(int(m_stack.size()) - 2); });
    connect(m_crumbs, &QLabel::linkActivated, this, [this](const QString &link) { showLevel(link.toInt()); });
    connect(m_map, &TreemapWidget::clicked, this, &UsageDialog::enter);
    connect(m_map, &TreemapWidget::menuRequested, this, &UsageDialog::openChild);
    connect(m_list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) { enter(item->data(0, Qt::UserRole).toInt()); });
    connect(m_list, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (QTreeWidgetItem *item = m_list->itemAt(pos))
            openChild(item->data(0, Qt::UserRole).toInt(), m_list->viewport()->mapToGlobal(pos));
    });
    resize(1000, 680);
    scan();
}

UsageDialog::~UsageDialog()
{
    if (m_scan)
        m_scan->cancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

void UsageDialog::scan()
{
    if (m_thread)
        return;
    m_stack.clear();
    m_map->setNode(nullptr);
    m_list->clear();
    m_busy->setVisible(true);
    m_status->setText(tr("Adding up %1…").arg(m_root));
    m_scan = new UsageScan(m_root);
    connect(m_scan, &UsageScan::progress, this, [this](quint64 bytes, quint64 files) {
        m_status->setText(tr("Adding up %1… %2 in %3 so far").arg(m_root, formatSize(bytes), countText(files)));
    });
    connect(m_scan, &UsageScan::finished, this, [this](bool completed, std::shared_ptr<UsageNode> tree, int unreadable) {
        m_thread->quit();
        m_thread->wait();
        m_thread = nullptr;
        m_scan = nullptr;
        m_busy->setVisible(false);
        m_tree = tree;
        m_unreadable = unreadable;
        m_completed = completed;
        m_stack = {m_tree.get()};
        showLevel(0);
    });
    m_thread = startOnThread(this, m_scan);
}

QString UsageDialog::pathOf(int depth, int child) const
{
    QString path = m_root;
    for (int i = 1; i <= depth && i < m_stack.size(); ++i)
        path += (path.endsWith(QLatin1Char('/')) ? QString() : QStringLiteral("/")) + m_stack[i]->name;
    if (child >= 0)
        path += (path.endsWith(QLatin1Char('/')) ? QString() : QStringLiteral("/")) + m_stack[depth]->children[child].name;
    return path;
}

void UsageDialog::showLevel(int depth)
{
    if (m_stack.isEmpty() || depth < 0)
        return;
    m_stack.resize(std::min<qsizetype>(depth + 1, m_stack.size()));
    const UsageNode *node = m_stack.last();
    QStringList crumbs;
    for (int i = 0; i < m_stack.size(); ++i) {
        const QString name = (i == 0 ? m_root : m_stack[i]->name).toHtmlEscaped();
        crumbs << (i + 1 == m_stack.size() ? QStringLiteral("<b>%1</b>").arg(name) : QStringLiteral("<a href=\"%1\">%2</a>").arg(i).arg(name));
    }
    m_crumbs->setText(crumbs.join(QStringLiteral(" › ")));
    m_up->setEnabled(m_stack.size() > 1);
    m_map->setNode(node);

    m_list->clear();
    for (int i = 0; i < node->children.size(); ++i) {
        const UsageNode &c = node->children[i];
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(0, c.name);
        item->setIcon(0, QIcon::fromTheme(c.isDir ? QStringLiteral("folder") : QStringLiteral("text-x-generic")));
        item->setData(0, Qt::UserRole, i);
        if (c.skipped && c.isDir) {
            item->setText(1, tr("not counted"));
            item->setToolTip(0, tr("Another drive or Btrfs subvolume is mounted here, or it couldn't be opened"));
        } else {
            item->setText(1, formatSize(c.size));
            item->setText(2, node->size ? QStringLiteral("%1%").arg(QString::number(100.0 * c.size / node->size, 'f', 1)) : QString());
            item->setText(3, c.isDir || c.files > 1 ? QString::number(c.files) : QString());
        }
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
    }
    for (int c = 1; c < 4; ++c)
        m_list->resizeColumnToContents(c);

    QString status = tr("%1 in %2.").arg(formatSize(node->size), countText(node->files));
    if (m_unreadable > 0)
        status += QLatin1Char(' ') + tr("%n folder(s) couldn't be opened (they belong to the system or other users), so the total may be a little low.",
                                        nullptr, m_unreadable);
    // At the top: what the drive itself counts as used, when that's a good deal more, says why.
    const QStorageInfo drive(m_root);
    const quint64 inUse = drive.isValid() && drive.bytesTotal() > drive.bytesFree() ? quint64(drive.bytesTotal() - drive.bytesFree()) : 0;
    if (depth == 0 && m_completed && inUse > node->size + std::max<quint64>(quint64(1) << 30, inUse / 10)) {
#ifdef Q_OS_WIN
        status += QLatin1Char(' ') + tr("Windows counts %1 as used: the rest is its own, mostly room it keeps free for updates, and the file system's tables.")
                                         .arg(formatSize(inUse));
#else
        status += QLatin1Char(' ') + tr("The drive counts %1 as used: the rest isn't in its folders (the file system's own tables, snapshots, "
                                        "deleted files that are still open).")
                                         .arg(formatSize(inUse));
#endif
    }
    m_status->setText(status);
}

void UsageDialog::enter(int child)
{
    if (m_stack.isEmpty() || child < 0)
        return;
    const UsageNode &c = m_stack.last()->children.value(child);
    if (!c.isDir || c.children.isEmpty())
        return;
    m_stack.append(&m_stack.last()->children[child]);
    showLevel(int(m_stack.size()) - 1);
}

void UsageDialog::openChild(int child, const QPoint &globalPos)
{
    if (m_stack.isEmpty() || child < 0 || child >= m_stack.last()->children.size())
        return;
    const UsageNode &c = m_stack.last()->children[child];
    const int depth = int(m_stack.size()) - 1;
    // "n smaller files" isn't a real file; its folder is the useful place to open.
    const bool real = c.isDir || c.files == 1;
    const QString path = real ? pathOf(depth, child) : pathOf(depth);
    QMenu menu(this);
    if (c.isDir && !c.children.isEmpty())
        menu.addAction(QIcon::fromTheme(QStringLiteral("go-down")), tr("Look Inside"), this, [this, child] { enter(child); });
    menu.addAction(QIcon::fromTheme(QStringLiteral("system-file-manager")), c.isDir ? tr("Open in File Manager") : tr("Show Folder in File Manager"), this,
                   [path, c] { QDesktopServices::openUrl(QUrl::fromLocalFile(c.isDir ? path : QFileInfo(path).absolutePath())); });
    if (real)
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Copy Path"), this, [path] { QApplication::clipboard()->setText(path); });
    menu.exec(globalPos);
}
