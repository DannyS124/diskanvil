// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The drive settings part of an ATA drive's Properties (hard drives and SSDs): what it's doing
// now and Sleep Now (hard drives), when to spin down, how hard to save power, how quietly to
// seek, the write cache, and reading ahead. Only what the drive can do is shown.

#include <QGroupBox>
#include <QVariantMap>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class UDisks;
struct Disk;

class PowerBox : public QGroupBox
{
    Q_OBJECT
public:
    PowerBox(UDisks *udisks, const Disk &disk, QWidget *parent = nullptr);
    static bool applies(const Disk &disk); // an ATA drive that has any of these
    QVariantMap configuration() const;      // what Save would set

private:
    void showState();
    void changed();

    UDisks *m_udisks;
    QString m_blockPath;
    QVariantMap m_current;
    bool m_cacheWas = false;
    bool m_lookaheadWas = false;
    QLabel *m_state;
    QPushButton *m_sleep;
    QComboBox *m_standby = nullptr;
    QComboBox *m_apm = nullptr;
    QComboBox *m_aam = nullptr;
    QCheckBox *m_cache = nullptr;
    QCheckBox *m_lookahead = nullptr;
    QPushButton *m_save;
};
