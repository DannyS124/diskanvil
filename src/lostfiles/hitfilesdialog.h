// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Which Files Were Hit?: the files a Rescue Copy couldn't read all of, from hitfiles::find on a
// thread, with how much of each is missing; the list can be saved next to the copy.

#include "hitfiles.h"
#include "rescuecopy.h"

#include <QDialog>

#include <memory>

class QLabel;
class QPushButton;
class QTreeWidget;

class HitFilesDialog : public QDialog
{
    Q_OBJECT
public:
    // `copy`: the image, or the drive copied to; `map`: the rescue's; `copyName` as people know it;
    // `saveTo`: where Save List suggests (next to the image).
    HitFilesDialog(std::shared_ptr<lost::Source> copy, const RescueMap &map, const QString &copyName, const QString &saveTo,
                   QWidget *parent = nullptr);
    ~HitFilesDialog() override;

    const hitfiles::Report &report() const { return m_report; }
    bool done() const { return m_done; }

signals:
    void finished(); // the list is shown

private:
    void showReport(const hitfiles::Report &report);

    QLabel *m_summary;
    QTreeWidget *m_list;
    QPushButton *m_save;
    QString m_copyName, m_saveTo;
    hitfiles::Report m_report;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
    bool m_done = false;
};
