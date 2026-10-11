// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractButton>
#include <QPixmap>
#include <QVector>
#include <QWidget>

#include <functional>

class QGridLayout;
class QLabel;
class UDisks;

// DiskForge Live's home screen: big tiles for what people start the stick for, a row of everyday
// programs, and a strip that says what state the PC is in (Secure Boot, network, drives). In
// DiskForge Live it is the desktop itself (diskforge --home). On a normal PC the same tiles open as
// a window, Tools → Quick Fixes, without the ones that only make sense on the stick.
//
// Every tile starts its tool as its own process (diskforge --open <tool>, or the program's
// .desktop file), so a tool that's busy or crashes never takes the home screen with it.
class HomeWindow : public QWidget
{
    Q_OBJECT
public:
    enum class Mode { Desktop, Window };

    struct Tile {
        QString id;          // tests find the tile as "tile-<id>"
        QString icon;        // icon name, and one most themes have
        QString fallbackIcon;
        QString title;
        QString text;
        QString tool;        // DiskForge's --open <tool>, or
        QString desktopId;   // a program's .desktop file, or
        bool mainWindow = false; // DiskForge's own window
        bool small = false;  // in the programs row instead of the big tiles
        bool confirm = false; // ask first (restarting into the firmware settings)
    };

    HomeWindow(UDisks *udisks, Mode mode, QWidget *parent = nullptr);

    // The tiles for this mode, the ones whose programs aren't installed left out.
    QVector<Tile> tiles() const { return m_tiles; }
    // Starts what a tile opens. Public for the tests, which also replace `launch`.
    void start(const Tile &tile);
    // How programs get started: QProcess::startDetached unless a test puts something else here.
    static std::function<bool(const QString &program, const QStringList &args)> launch;
    // Where .desktop files are looked for, for the tests.
    static QStringList applicationDirs;

    enum class SecureBoot { On, Off, Bios, Unknown };
    // From the firmware's SecureBoot variable (efivarsDir is /sys/firmware/efi/efivars).
    static SecureBoot secureBoot(const QString &efiDir = QStringLiteral("/sys/firmware/efi"));
    // A program's command line from its .desktop file, without %f and the like; empty if
    // there's no such file.
    static QStringList desktopCommand(const QString &desktopId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QVector<Tile> allTiles() const;
    void layoutTiles();
    void updateStatus();
    QColor color(const char *role) const;

    UDisks *m_udisks;
    Mode m_mode;
    QVector<Tile> m_tiles;
    QPixmap m_wallpaper;
    QGridLayout *m_grid = nullptr;
    QGridLayout *m_programs = nullptr;
    QList<QAbstractButton *> m_bigTiles;
    QList<QAbstractButton *> m_smallTiles;
    int m_columns = 0;
    int m_programColumns = 0;
    QLabel *m_secureBoot = nullptr;
    QLabel *m_network = nullptr;
    QLabel *m_drives = nullptr;
    QLabel *m_logs = nullptr;
};
