// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Which Windows a tool works on: the one that's running (the Windows version), or one on a
// drive, mounted first if it isn't (read-only, unless the tool is going to change something).
// With `anySystem`, Linux and macOS too (What This PC Did), and on a Linux PC the running one.

#include <QDialog>

#include <functional>

class QLabel;
class QListWidget;
class QPushButton;
class UDisks;

class WindowsPicker : public QDialog
{
    Q_OBJECT
public:
    // `writable`: mounted so it can be changed. `purpose`: what it's for, a line at the top.
    WindowsPicker(UDisks *udisks, bool writable, const QString &purpose, QWidget *parent = nullptr, bool anySystem = false);
    // Where the picked Windows is mounted; empty for the running one (with anySystem on Linux: "/").
    QString root() const { return m_root; }
    bool running() const { return m_running; }

    // The ones already at hand, without asking: mounted drives with Windows on them.
    static QStringList mounted(UDisks *udisks);

private:
    void fill();
    void choose();

    UDisks *m_udisks;
    bool m_writable;
    bool m_anySystem;
    QListWidget *m_list;
    QLabel *m_note;
    QPushButton *m_ok;
    QString m_root;
    bool m_running = false;
};
