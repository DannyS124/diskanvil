// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Where they were taken: the photos and films Find Lost Files found that say where they were made,
// on a map (placesmap.h) and by place, without going online. Each place lists its files, which
// open in the viewer from here.

#include "lostscan.h"
#include "placesmap.h"

#include <QDialog>

#include <atomic>
#include <memory>

class QLabel;
class QListWidget;
class QPushButton;
class QThread;
class QTreeWidget;

class PlacesDialog : public QDialog
{
    Q_OBJECT
public:
    PlacesDialog(std::shared_ptr<lost::Source> source, const QVector<lost::Found> &files, const QString &title, QWidget *parent = nullptr);
    ~PlacesDialog() override;
    // For the tests: wait until every file has been looked at.
    void waitUntilDone();
    int located() const { return int(m_spots.size()); }

Q_SIGNALS:
    // Show this one (the window's list follows, and the viewer opens it).
    void view(int index);

private:
    struct Spot {
        int index = -1; // the file's
        double latitude = 0, longitude = 0;
        qint64 when = 0;
        QString place;
    };
    void add(const QVector<Spot> &spots);
    void finish(int looked, bool stopped);
    void listFiles(const QVector<int> &indexes, double latitude, double longitude);

    std::shared_ptr<lost::Source> m_source;
    QVector<lost::Found> m_files;
    QVector<Spot> m_spots;
    QHash<int, int> m_spotOf; // file index -> its spot
    PlacesMap *m_map;
    QTreeWidget *m_places;
    QListWidget *m_list;
    QLabel *m_status;
    QPushButton *m_view, *m_osm;
    double m_pickedLatitude = 0, m_pickedLongitude = 0;
    bool m_hasPick = false;
    QThread *m_thread = nullptr;
    std::atomic<bool> m_stop{false};
};
