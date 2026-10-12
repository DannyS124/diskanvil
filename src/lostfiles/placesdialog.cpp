// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "placesdialog.h"

#include "filetypes.h"
#include "naming.h"
#include "places.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QSplitter>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>

namespace {

bool wanted(const lost::Found &f)
{
    if (f.type < 0 || f.nameOnly || f.size == 0)
        return false;
    const filetypes::Category c = filetypes::types()[f.type].category;
    return c == filetypes::Category::Picture || c == filetypes::Category::Video;
}

} // namespace

PlacesDialog::PlacesDialog(std::shared_ptr<lost::Source> source, const QVector<lost::Found> &files, const QString &title, QWidget *parent)
    : QDialog(parent)
    , m_source(std::move(source))
    , m_files(files)
    , m_map(new PlacesMap)
    , m_places(new QTreeWidget)
    , m_list(new QListWidget)
    , m_status(new QLabel)
    , m_view(new QPushButton(tr("View")))
    , m_osm(new QPushButton(tr("Open on OpenStreetMap…")))
{
    setWindowTitle(tr("Where They Were Taken — %1").arg(title));
    m_map->setObjectName(QStringLiteral("map"));
    m_places->setObjectName(QStringLiteral("places"));
    m_list->setObjectName(QStringLiteral("files"));
    m_places->setHeaderLabels({tr("Place"), tr("Files")});
    m_places->setRootIsDecorated(false);
    m_places->header()->setStretchLastSection(false);
    m_places->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_places->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_status->setWordWrap(true);
    m_view->setEnabled(false);
    m_osm->setEnabled(false);
    m_osm->setToolTip(tr("Shows the spot on openstreetmap.org in your web browser, after asking: that website is told the spot"));

    auto *side = new QWidget;
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->addWidget(new QLabel(tr("Places (the nearest town or city):")));
    sideLayout->addWidget(m_places, 2);
    sideLayout->addWidget(new QLabel(tr("Taken there:")));
    sideLayout->addWidget(m_list, 3);
    auto *sideButtons = new QHBoxLayout;
    sideButtons->addWidget(m_view);
    sideButtons->addWidget(m_osm);
    sideButtons->addStretch();
    sideLayout->addLayout(sideButtons);
    auto *split = new QSplitter;
    split->addWidget(m_map);
    split->addWidget(side);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);

    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_status, 1);
    bottom->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Photos and films that say where they were taken. Scroll to zoom, drag to move, click a pin for "
                                    "its files. The places are worked out here, without the internet.")));
    layout->addWidget(split, 1);
    layout->addLayout(bottom);
    resize(1100, 680);

    connect(m_map, &PlacesMap::picked, this, &PlacesDialog::listFiles);
    connect(m_places, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        QVector<int> indexes;
        double north = -90, south = 90, east = -180, west = 180, lat = 0, lon = 0;
        for (const QVariant &v : item->data(0, Qt::UserRole).toList()) {
            const Spot &s = m_spots[m_spotOf.value(v.toInt())];
            indexes << s.index;
            north = std::max(north, s.latitude);
            south = std::min(south, s.latitude);
            east = std::max(east, s.longitude);
            west = std::min(west, s.longitude);
            lat += s.latitude;
            lon += s.longitude;
        }
        if (indexes.isEmpty())
            return;
        lat /= indexes.size();
        lon /= indexes.size();
        m_map->showSpot((north + south) / 2, (east + west) / 2, std::max({(east - west) * 1.5, (north - south) * 2.5, 0.2}));
        listFiles(indexes, lat, lon);
    });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) { m_view->setEnabled(row >= 0); });
    auto view = [this] {
        if (QListWidgetItem *item = m_list->currentItem())
            Q_EMIT this->view(item->data(Qt::UserRole).toInt());
    };
    connect(m_view, &QPushButton::clicked, this, view);
    connect(m_list, &QListWidget::itemActivated, this, view);
    connect(m_osm, &QPushButton::clicked, this, [this] {
        // The file picked, or the middle of the pin.
        double lat = m_pickedLatitude, lon = m_pickedLongitude;
        if (QListWidgetItem *item = m_list->currentItem()) {
            const Spot &s = m_spots[m_spotOf.value(item->data(Qt::UserRole).toInt())];
            lat = s.latitude;
            lon = s.longitude;
        }
        openOnOpenStreetMap(this, lat, lon);
    });

    int total = 0;
    for (const lost::Found &f : std::as_const(m_files))
        total += wanted(f);
    m_status->setText(tr("Looking at %1 pictures and films…").arg(QLocale().toString(total)));
    const std::shared_ptr<lost::Source> from = m_source;
    const QVector<lost::Found> list = m_files;
    m_thread = QThread::create([this, from, list, total] {
        QVector<Spot> batch;
        QElapsedTimer clock;
        clock.start();
        int looked = 0;
        for (int i = 0; i < list.size() && !m_stop; ++i) {
            const lost::Found &f = list[i];
            if (!wanted(f))
                continue;
            ++looked;
            lost::FileReader reader(from, f);
            const naming::Spot s = naming::where(f.type, f.size, [&reader](quint64 at, qint64 len) {
                QByteArray part(qsizetype(std::max<qint64>(0, len)), Qt::Uninitialized);
                part.truncate(qsizetype(std::max<qint64>(0, reader.read(at, part.data(), part.size()))));
                return part;
            });
            if (s.known)
                batch.push_back({i, s.latitude, s.longitude, s.when, places::describe(places::nearest(s.latitude, s.longitude))});
            if (clock.elapsed() > 300) {
                clock.restart();
                QMetaObject::invokeMethod(this, [this, batch, looked, total] {
                    add(batch);
                    m_status->setText(tr("Looking at %1 of %2 pictures and films: %3 say where so far…")
                                          .arg(QLocale().toString(looked), QLocale().toString(total), QLocale().toString(m_spots.size())));
                }, Qt::QueuedConnection);
                batch.clear();
            }
        }
        const bool stopped = m_stop;
        QMetaObject::invokeMethod(this, [this, batch, looked, stopped] {
            add(batch);
            finish(looked, stopped);
        }, Qt::QueuedConnection);
    });
    m_thread->start();
}

PlacesDialog::~PlacesDialog()
{
    m_stop = true;
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
}

void PlacesDialog::waitUntilDone()
{
    while (m_thread && !m_thread->isFinished())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCoreApplication::processEvents();
}

void PlacesDialog::add(const QVector<Spot> &spots)
{
    if (spots.isEmpty())
        return;
    const bool first = m_spots.isEmpty();
    for (const Spot &s : spots) {
        m_spotOf.insert(s.index, int(m_spots.size()));
        m_spots.push_back(s);
    }
    QVector<PlacesMap::Pin> pins;
    pins.reserve(m_spots.size());
    QHash<QString, QVariantList> byPlace;
    for (const Spot &s : std::as_const(m_spots)) {
        pins.push_back({s.latitude, s.longitude, s.index});
        byPlace[s.place.isEmpty() ? tr("Far from any town") : s.place] << s.index;
    }
    m_map->setPins(pins);
    if (first)
        m_map->showAll();
    // The places, the one with the most first.
    m_places->clear();
    QStringList names = byPlace.keys();
    std::sort(names.begin(), names.end(), [&byPlace](const QString &a, const QString &b) {
        return byPlace[a].size() != byPlace[b].size() ? byPlace[a].size() > byPlace[b].size() : a < b;
    });
    for (const QString &name : std::as_const(names)) {
        auto *item = new QTreeWidgetItem(m_places, {name, QLocale().toString(byPlace[name].size())});
        item->setData(0, Qt::UserRole, byPlace[name]);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    }
}

void PlacesDialog::finish(int looked, bool stopped)
{
    m_map->showAll();
    if (stopped)
        return;
    if (looked == 0)
        m_status->setText(tr("There are no pictures or films among what was found."));
    else if (m_spots.isEmpty())
        m_status->setText(tr("None of the %1 pictures and films says where it was taken. (Cameras only write it down when they have a "
                             "location: phones with it switched on, or cameras with GPS.)")
                              .arg(QLocale().toString(looked)));
    else
        m_status->setText(tr("%1 of the %2 pictures and films say where they were taken.").arg(QLocale().toString(m_spots.size()), QLocale().toString(looked)));
}

void PlacesDialog::listFiles(const QVector<int> &indexes, double latitude, double longitude)
{
    m_pickedLatitude = latitude;
    m_pickedLongitude = longitude;
    m_hasPick = true;
    m_osm->setEnabled(true);
    QVector<int> sorted = indexes;
    std::sort(sorted.begin(), sorted.end(), [this](int a, int b) {
        const qint64 wa = m_spots[m_spotOf.value(a)].when, wb = m_spots[m_spotOf.value(b)].when;
        return wa != wb ? wa < wb : a < b;
    });
    m_list->clear();
    for (const int index : std::as_const(sorted)) {
        const lost::Found &f = m_files[index];
        const qint64 when = m_spots[m_spotOf.value(index)].when;
        auto *item = new QListWidgetItem(when > 0 ? QStringLiteral("%1  ·  %2").arg(f.name, QLocale().toString(QDateTime::fromMSecsSinceEpoch(when), QLocale::ShortFormat))
                                                  : f.name,
                                         m_list);
        item->setData(Qt::UserRole, index);
        item->setToolTip(f.folder.isEmpty() ? f.name : f.folder + QLatin1Char('/') + f.name);
    }
    m_view->setEnabled(false);
}
