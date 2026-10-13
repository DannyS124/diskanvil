// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "homewindow.h"

#include "applog.h"
#include "rescuestick.h"
#include "writeblock.h"
#include "udisks.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkInformation>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTextLayout>
#include <QVBoxLayout>

std::function<bool(const QString &, const QStringList &)> HomeWindow::launch = [](const QString &program, const QStringList &args) {
    return QProcess::startDetached(program, args);
};
QStringList HomeWindow::applicationDirs;

namespace {

// DiskAnvil Live's colours (rescue/art/palette.txt), for the desktop.
QColor liveColor(const char *role)
{
    static const QHash<QByteArray, QColor> colours = {
        {"background", QColor(0x06, 0x0a, 0x16)}, {"surface", QColor(0x0d, 0x14, 0x26)},
        {"line", QColor(0x1a, 0x2a, 0x4a)},       {"text", QColor(0xe6, 0xf1, 0xff)},
        {"dim", QColor(0x7f, 0x93, 0xb5)},        {"neon", QColor(0x00, 0xe5, 0xff)},
        {"blue", QColor(0x1f, 0x6f, 0xff)},       {"baby", QColor(0x9a, 0xd8, 0xff)},
        {"red", QColor(0xff, 0x33, 0x55)},        {"green", QColor(0x2b, 0xee, 0x8a)},
        {"amber", QColor(0xff, 0xb3, 0x00)},
    };
    return colours.value(role);
}

// One tile: an icon, a title and a line saying what it's for. Painted by hand so it looks the
// same with any widget style.
class TileButton : public QAbstractButton
{
public:
    TileButton(const HomeWindow::Tile &tile, std::function<QColor(const char *)> colour, QWidget *parent)
        : QAbstractButton(parent)
        , m_tile(tile)
        , m_colour(std::move(colour))
    {
        setObjectName(QStringLiteral("tile-") + tile.id);
        setText(tile.title);
        setToolTip(tile.text);
        setIcon(QIcon::fromTheme(tile.icon, QIcon::fromTheme(tile.fallbackIcon)));
        setAccessibleName(tile.title);
        setAccessibleDescription(tile.text);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        // Big tiles give up some height on a short screen (1280x800 and the like), so the
        // programs row still shows above the taskbar.
        setSizePolicy(QSizePolicy::Expanding, m_tile.small ? QSizePolicy::Fixed : QSizePolicy::Maximum);
    }

    QSize sizeHint() const override { return m_tile.small ? QSize(168, 48) : QSize(320, 100); }
    QSize minimumSizeHint() const override { return m_tile.small ? QSize(120, 48) : QSize(240, 84); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool hot = underMouse() || hasFocus();
        QColor fill = m_colour("surface");
        if (isDown())
            fill = fill.lighter(150);
        else if (hot)
            fill = fill.lighter(125);
        p.setPen(QPen(hot ? m_colour("neon") : m_colour("line"), hot ? 1.5 : 1.0));
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 12, 12);

        // Narrow big tiles (four across on a 1280 screen) keep more room for their words.
        const bool narrow = !m_tile.small && width() < 300;
        const int iconSize = m_tile.small ? 24 : narrow ? 34 : 44;
        const int pad = m_tile.small ? 12 : narrow ? 14 : 20;
        const QRect iconRect(pad, (height() - iconSize) / 2, iconSize, iconSize);
        icon().paint(&p, iconRect);
        const int left = iconRect.right() + (m_tile.small ? 10 : narrow ? 12 : 18);
        const int room = width() - left - pad;

        QFont title = font();
        title.setBold(true);
        if (!m_tile.small)
            title.setPointSizeF(font().pointSizeF() * (narrow ? 1.08 : 1.2));
        const QFontMetrics titleMetrics(title);
        p.setFont(title);
        p.setPen(m_colour("text"));
        if (m_tile.small) {
            p.drawText(QRect(left, 0, room, height()), Qt::AlignVCenter | Qt::AlignLeft, titleMetrics.elidedText(text(), Qt::ElideRight, room));
            return;
        }
        const int titleTop = height() / 2 - titleMetrics.height() - 1;
        p.drawText(QRect(left, titleTop, room, titleMetrics.height()), Qt::AlignVCenter | Qt::AlignLeft,
                   titleMetrics.elidedText(text(), Qt::ElideRight, room));
        p.setFont(font());
        p.setPen(m_colour("dim"));
        // As many lines as fit; a longer one ends in "…" (the tooltip has it all).
        const QFontMetrics metrics(font());
        const int lines = std::max(1, (height() / 2 - 6) / metrics.lineSpacing());
        QTextLayout layout(m_tile.text, font());
        layout.beginLayout();
        QStringList shown;
        for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
            line.setLineWidth(room);
            if (shown.size() == lines - 1) {
                shown << metrics.elidedText(m_tile.text.mid(line.textStart()).simplified(), Qt::ElideRight, room);
                break;
            }
            shown << m_tile.text.mid(line.textStart(), line.textLength()).trimmed();
        }
        layout.endLayout();
        for (int i = 0; i < shown.size(); ++i)
            p.drawText(QRect(left, height() / 2 + 3 + i * metrics.lineSpacing(), room, metrics.lineSpacing()), Qt::AlignTop | Qt::AlignLeft, shown[i]);
    }

private:
    HomeWindow::Tile m_tile;
    std::function<QColor(const char *)> m_colour;
};

QString dot(const QColor &colour, const QString &text)
{
    return QStringLiteral("<span style=\"color:%1\">●</span>&nbsp; %2").arg(colour.name(), text.toHtmlEscaped());
}

} // namespace

HomeWindow::HomeWindow(UDisks *udisks, Mode mode, QWidget *parent)
    : QWidget(parent)
    , m_udisks(udisks)
    , m_mode(mode)
{
    const bool desktop = mode == Mode::Desktop;
    setObjectName(QStringLiteral("home"));
    m_tiles = allTiles();

    if (desktop) {
        // The desktop itself: no frame, under every other window, the whole screen.
        setWindowFlags(Qt::FramelessWindowHint);
        setAttribute(Qt::WA_X11NetWmWindowTypeDesktop);
        if (QScreen *screen = QGuiApplication::primaryScreen()) {
            setGeometry(screen->geometry());
            connect(screen, &QScreen::geometryChanged, this, [this](const QRect &area) { setGeometry(area); });
            // The taskbar claims its strip after we're up: keep the bottom row above it.
            connect(screen, &QScreen::availableGeometryChanged, this, [this] { resizeEvent(nullptr); });
        }
        m_wallpaper.load(QStringLiteral("/usr/local/share/diskanvil-live/wallpaper.png"));
        setWindowTitle(QStringLiteral("DiskAnvil Live"));
    } else {
        setWindowTitle(tr("Quick Fixes"));
        resize(1040, 640);
    }

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(desktop ? 56 : 24, desktop ? 40 : 20, desktop ? 56 : 24, 24);
    outer->setSpacing(desktop ? 18 : 14);

    // The top: the name on the left, how the PC is doing on the right.
    auto *header = new QHBoxLayout;
    header->setSpacing(18);
    if (desktop) {
        auto *logo = new QLabel;
        logo->setPixmap(QIcon::fromTheme(QStringLiteral("diskanvil-live"), QIcon(QStringLiteral("/usr/local/share/diskanvil-live/diskanvil-live-icon.svg")))
                            .pixmap(72, 72));
        header->addWidget(logo);
    }
    auto *names = new QVBoxLayout;
    names->setSpacing(2);
    auto *name = new QLabel;
    QFont nameFont = font();
    nameFont.setPointSizeF(font().pointSizeF() * (desktop ? 2.6 : 1.8));
    nameFont.setBold(true);
    name->setFont(nameFont);
    name->setTextFormat(Qt::RichText);
    name->setText(desktop ? QStringLiteral("<span style=\"color:#9ad8ff\">Disk</span><span style=\"color:#00e5ff\">Anvil</span><span style=\"color:#2bee8a\">&nbsp;Live</span>")
                          : tr("Quick Fixes").toHtmlEscaped());
    auto *tagline = new QLabel(desktop ? tr("Rescue · Recover · Repair") : tr("The usual jobs, one click each."));
    tagline->setObjectName(QStringLiteral("tagline"));
    names->addStretch();
    names->addWidget(name);
    names->addWidget(tagline);
    names->addStretch();
    header->addLayout(names);
    header->addStretch();
    auto *status = new QVBoxLayout;
    status->setSpacing(4);
    m_secureBoot = new QLabel;
    m_network = new QLabel;
    m_drives = new QLabel;
    m_logs = new QLabel;
    m_kept = new QLabel;
    m_protected = new QLabel;
    for (QLabel *label : {m_secureBoot, m_network, m_drives, m_logs, m_kept, m_protected}) {
        label->setTextFormat(Qt::RichText);
        status->addWidget(label, 0, Qt::AlignRight);
    }
    m_secureBoot->setObjectName(QStringLiteral("secureBoot"));
    m_drives->setObjectName(QStringLiteral("drives"));
    m_secureBoot->setVisible(desktop);
    m_network->setVisible(desktop);
    m_logs->setVisible(desktop && rescue::runningInRescue());
    m_kept->setVisible(desktop && rescue::runningInRescue());
    m_kept->setObjectName(QStringLiteral("kept"));
    m_protected->setObjectName(QStringLiteral("protected"));
    m_protected->hide();
    header->addLayout(status);
    outer->addLayout(header);

    auto *heading = new QLabel(tr("What do you want to do?"));
    QFont headingFont = font();
    headingFont.setPointSizeF(font().pointSizeF() * 1.15);
    heading->setFont(headingFont);
    outer->addSpacing(desktop ? 12 : 4);
    outer->addWidget(heading);

    m_grid = new QGridLayout;
    m_grid->setSpacing(14);
    for (const Tile &tile : std::as_const(m_tiles)) {
        if (tile.small)
            continue;
        auto *button = new TileButton(tile, [this](const char *role) { return color(role); }, this);
        connect(button, &QAbstractButton::clicked, this, [this, tile] { start(tile); });
        m_bigTiles << button;
    }
    outer->addLayout(m_grid);
    outer->addStretch();

    bool anySmall = false;
    for (const Tile &tile : std::as_const(m_tiles))
        anySmall = anySmall || tile.small;
    if (anySmall) {
        auto *programsHeading = new QLabel(tr("Programs"));
        programsHeading->setFont(headingFont);
        outer->addWidget(programsHeading);
        m_programs = new QGridLayout;
        m_programs->setSpacing(10);
        for (const Tile &tile : std::as_const(m_tiles)) {
            if (!tile.small)
                continue;
            auto *button = new TileButton(tile, [this](const char *role) { return color(role); }, this);
            connect(button, &QAbstractButton::clicked, this, [this, tile] { start(tile); });
            m_smallTiles << button;
        }
        outer->addLayout(m_programs);
    }

    // Text colours that the palette doesn't give a desktop window.
    QPalette pal = palette();
    pal.setColor(QPalette::WindowText, color("text"));
    setPalette(pal);
    tagline->setStyleSheet(QStringLiteral("color: %1").arg(color("dim").name()));
    heading->setStyleSheet(QStringLiteral("color: %1").arg(color("baby").name()));

    connect(m_udisks, &UDisks::changed, this, &HomeWindow::updateStatus);
    if (desktop && QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability)) {
        connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this, &HomeWindow::updateStatus);
    }
    updateStatus();
    layoutTiles();
}

QVector<HomeWindow::Tile> HomeWindow::allTiles() const
{
    const bool desktop = m_mode == Mode::Desktop;
    const bool stick = desktop || rescue::runningInRescue();
    QVector<Tile> tiles;
    bool programs = false; // the small tiles at the bottom, once this is set
    auto add = [&tiles, &programs](const QString &id, const QString &icon, const QString &fallback, const QString &title,
                                   const QString &text) -> Tile & {
        Tile tile;
        tile.id = id;
        tile.icon = icon;
        tile.fallbackIcon = fallback;
        tile.title = title;
        tile.text = text;
        tile.small = programs;
        tiles.append(tile);
        return tiles.last();
    };
    if (desktop)
        add(QStringLiteral("drives"), QStringLiteral("diskanvil-live-drives"), QStringLiteral("drive-harddisk"), tr("Drives and Partitions"),
            tr("Every drive in the PC: health, partitions, backups and more"))
            .mainWindow = true;
    add(QStringLiteral("whathappened"), QStringLiteral("diskanvil-live-whathappened"), QStringLiteral("help-hint"), tr("What Happened?"),
        tr("Lost files? Start here: the odds, what not to do, and what to do"))
        .tool = QStringLiteral("what-happened");
    add(QStringLiteral("savefiles"), QStringLiteral("diskanvil-live-savefiles"), QStringLiteral("document-save"), tr("Save My Files"),
        tr("People's folders and bookmarks, off a PC that won't start"))
        .tool = QStringLiteral("save-files");
    add(QStringLiteral("recover"), QStringLiteral("diskanvil-live-recover"), QStringLiteral("edit-find"), tr("Get Files Back"),
        tr("Deleted files, or files from a formatted or damaged drive"))
        .tool = QStringLiteral("lost-files");
    add(QStringLiteral("lookcloser"), QStringLiteral("diskanvil-live-lookcloser"), QStringLiteral("zoom-in"), tr("Look Closer"),
        tr("Pictures and films: enhance, pick a spot, stack frames"))
        .tool = QStringLiteral("look-closer");
    add(QStringLiteral("viruses"), QStringLiteral("diskanvil-live-virus"), QStringLiteral("security-high"), tr("Scan for Viruses"),
        desktop ? tr("Windows' drive, while Windows can't hide anything") : tr("Known malware on a drive or in a folder"))
        .tool = QStringLiteral("virus-scan");
    add(QStringLiteral("whycrash"), QStringLiteral("diskanvil-live-whycrash"), QStringLiteral("dialog-warning"), tr("Why Did It Crash?"),
        tr("A Windows that keeps blue-screening or restarting: why"))
        .tool = QStringLiteral("why-crash");
    add(QStringLiteral("testdisk"), QStringLiteral("diskanvil-live-testdisk"), QStringLiteral("drive-harddisk"), tr("Find Lost Partitions"),
        tr("Partitions that disappeared, and boot sectors (TestDisk)"))
        .desktopId = QStringLiteral("diskanvil-live-testdisk");
    add(QStringLiteral("writeimage"), QStringLiteral("diskanvil-live-writeimage"), QStringLiteral("media-flash"), tr("Write an Image to USB"),
        tr("Linux, Raspberry Pi and other images, packed or not"))
        .tool = QStringLiteral("write-image");
    add(QStringLiteral("windowsusb"), QStringLiteral("diskanvil-live-windowsusb"), QStringLiteral("media-flash"), tr("Make a Windows USB"),
        tr("A stick that installs Windows 10 or 11"))
        .tool = QStringLiteral("windows-usb");
    add(QStringLiteral("checkstick"), QStringLiteral("diskanvil-live-checkstick"), QStringLiteral("drive-removable-media-usb"), tr("Check a USB Stick"),
        tr("Bad spots, and sticks that are smaller than they say"))
        .tool = QStringLiteral("check-stick");
    add(QStringLiteral("writeprotected"), QStringLiteral("diskanvil-live-writeprotect"), QStringLiteral("object-locked"), tr("Write-Protected Drive"),
        tr("A stick or card that won't let you save: why, and the fix"))
        .tool = QStringLiteral("write-protected");
    add(QStringLiteral("copystick"), QStringLiteral("diskanvil-live-copystick"), QStringLiteral("media-flash"),
        stick ? tr("Copy This Stick") : tr("Make a DiskAnvil Live USB"),
        stick ? tr("Another DiskAnvil Live stick, made from this one") : tr("A rescue stick that starts any PC"))
        .tool = QStringLiteral("rescue-usb");

    if (desktop) {
        programs = true;
        add(QStringLiteral("files"), QStringLiteral("system-file-manager"), QStringLiteral("folder"), tr("Files"), {}).desktopId =
            QStringLiteral("pcmanfm-qt");
        add(QStringLiteral("web"), QStringLiteral("web-browser"), QStringLiteral("applications-internet"), tr("Web Browser"), {}).desktopId =
            QStringLiteral("firefox-esr");
        add(QStringLiteral("terminal"), QStringLiteral("utilities-terminal"), QStringLiteral("utilities-terminal"), tr("Terminal"), {})
            .desktopId = QStringLiteral("qterminal");
        add(QStringLiteral("tasks"), QStringLiteral("utilities-system-monitor"), QStringLiteral("utilities-system-monitor"), tr("Task Manager"), {})
            .desktopId = QStringLiteral("qps");
        add(QStringLiteral("editor"), QStringLiteral("accessories-text-editor"), QStringLiteral("accessories-text-editor"), tr("Text Editor"), {})
            .desktopId = QStringLiteral("featherpad");
        add(QStringLiteral("help"), QStringLiteral("diskanvil-live-remotehelp"), QStringLiteral("preferences-desktop-remote-desktop"), tr("Let Someone Help"), {})
            .tool = QStringLiteral("remote-help");
        add(QStringLiteral("netboot"), QStringLiteral("diskanvil-live-netboot"), QStringLiteral("network-server"), tr("Start Other PCs"), {}).tool =
            QStringLiteral("net-boot");
        add(QStringLiteral("logs"), QStringLiteral("diskanvil-live-logs"), QStringLiteral("text-x-log"), tr("Save Logs"), {}).desktopId =
            QStringLiteral("diskanvil-live-logs");
        add(QStringLiteral("readme"), QStringLiteral("diskanvil-live-readme"), QStringLiteral("help-about"), tr("Read Me"), {}).desktopId =
            QStringLiteral("diskanvil-live-readme");
        // Only UEFI can be asked to open its setup at the next start.
        if (secureBoot() != SecureBoot::Bios) {
            Tile &firmware = add(QStringLiteral("firmware"), QStringLiteral("diskanvil-live-firmware"), QStringLiteral("system-reboot"),
                                 tr("Firmware Settings"), tr("Restart into the PC's UEFI setup (Secure Boot, boot order...)"));
            firmware.desktopId = QStringLiteral("diskanvil-live-firmware");
            firmware.confirm = true;
        }
    }

    // A tile is only there when what it opens is.
    QVector<Tile> present;
    for (const Tile &tile : std::as_const(tiles)) {
        if (!tile.desktopId.isEmpty() && desktopCommand(tile.desktopId).isEmpty())
            continue;
        present << tile;
    }
    return present;
}

QColor HomeWindow::color(const char *role) const
{
    if (m_mode == Mode::Desktop)
        return liveColor(role);
    // As a window it follows DiskAnvil's theme.
    const QPalette pal = QGuiApplication::palette();
    const QByteArray name(role);
    if (name == "background")
        return pal.color(QPalette::Window);
    if (name == "surface")
        return pal.color(QPalette::Base);
    if (name == "line")
        return pal.color(QPalette::Mid);
    if (name == "text")
        return pal.color(QPalette::Text);
    if (name == "dim") {
        QColor dim = pal.color(QPalette::Text);
        dim.setAlphaF(0.65f);
        return dim;
    }
    if (name == "neon")
        return pal.color(QPalette::Highlight);
    if (name == "baby")
        return pal.color(QPalette::Text);
    return liveColor(role);
}

void HomeWindow::layoutTiles()
{
    const int margins = m_mode == Mode::Desktop ? 112 : 48;
    const int columns = std::clamp((width() - margins) / 290, 1, 4);
    if (columns != m_columns) {
        m_columns = columns;
        for (QAbstractButton *tile : std::as_const(m_bigTiles))
            m_grid->removeWidget(tile);
        for (int i = 0; i < m_bigTiles.size(); ++i)
            m_grid->addWidget(m_bigTiles[i], i / columns, i % columns);
    }
    // The programs go on one row when there's room for their names, two when there isn't.
    if (!m_programs)
        return;
    const int programColumns = (width() - margins) / std::max<int>(1, m_smallTiles.size()) >= 170
                                   ? int(m_smallTiles.size())
                                   : int(m_smallTiles.size() + 1) / 2;
    if (programColumns == m_programColumns || programColumns == 0)
        return;
    m_programColumns = programColumns;
    for (QAbstractButton *tile : std::as_const(m_smallTiles))
        m_programs->removeWidget(tile);
    for (int i = 0; i < m_smallTiles.size(); ++i)
        m_programs->addWidget(m_smallTiles[i], i / programColumns, i % programColumns);
}

void HomeWindow::resizeEvent(QResizeEvent *event)
{
    if (event)
        QWidget::resizeEvent(event);
    if (m_mode == Mode::Desktop) {
        // The taskbar sits on top of the desktop: keep the programs row above it.
        if (QScreen *screen = this->screen()) {
            const int below = screen->geometry().bottom() - screen->availableGeometry().bottom();
            layout()->setContentsMargins(56, 40, 56, below + 24);
        }
    }
    layoutTiles();
}

void HomeWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    if (m_mode == Mode::Window) {
        p.fillRect(rect(), color("background"));
        return;
    }
    if (!m_wallpaper.isNull()) {
        const QPixmap scaled = m_wallpaper.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        p.drawPixmap((width() - scaled.width()) / 2, (height() - scaled.height()) / 2, scaled);
        return;
    }
    QLinearGradient gradient(0, 0, width(), height());
    gradient.setColorAt(0, QColor(0x0a, 0x11, 0x24));
    gradient.setColorAt(1, QColor(0x03, 0x06, 0x0e));
    p.fillRect(rect(), gradient);
}

void HomeWindow::updateStatus()
{
    switch (secureBoot()) {
    case SecureBoot::On:
        m_secureBoot->setText(dot(color("green"), tr("Secure Boot is on")));
        break;
    case SecureBoot::Off:
        m_secureBoot->setText(dot(color("amber"), tr("Secure Boot is off")));
        break;
    case SecureBoot::Bios:
        m_secureBoot->setText(dot(color("dim"), tr("Started the old way (BIOS)")));
        break;
    case SecureBoot::Unknown:
        m_secureBoot->setText(dot(color("dim"), tr("Secure Boot: can't tell")));
        break;
    }

    if (QNetworkInformation *net = QNetworkInformation::instance()) {
        const bool online = net->reachability() == QNetworkInformation::Reachability::Online;
        m_network->setText(online ? dot(color("green"), tr("Connected to the internet")) : dot(color("dim"), tr("No internet")));
    } else {
        m_network->hide();
    }

    int drives = 0;
    for (const Disk &d : m_udisks->disks()) {
        if (!d.isLoop && !d.isSystem)
            ++drives;
    }
    m_drives->setText(drives == 0   ? dot(color("dim"), tr("No drives found yet"))
                      : drives == 1 ? dot(color("neon"), tr("One drive found"))
                                    : dot(color("neon"), tr("%1 drives found").arg(drives)));

    if (m_mode == Mode::Desktop && rescue::runningInRescue()) {
        const QString medium = QStringLiteral("/run/live/medium");
        const bool onStick = QStorageInfo(medium).fileSystemType() == "vfat";
        // Started from its partition on the PC's own drive (Put DiskAnvil Live on This PC), not a stick.
        bool onPc = false;
        for (const Disk &d : m_udisks->disks()) {
            for (const Volume &v : d.volumes)
                onPc = onPc || (v.mounts().contains(medium) && !d.removable && d.bus != QLatin1String("usb"));
        }
        QFile cmdlineFile(QStringLiteral("/proc/cmdline"));
        const bool overNetwork = cmdlineFile.open(QIODevice::ReadOnly) && cmdlineFile.readAll().contains(" fetch=");
        m_logs->setText(overNetwork ? dot(color("dim"), tr("Logs stay in memory (started over the network)"))
                        : !onStick ? dot(color("dim"), tr("Logs stay in memory (started from a CD or ISO)"))
                        : onPc   ? dot(color("green"), tr("Logs are saved on its partition on this PC"))
                                 : dot(color("green"), tr("Logs are saved on the stick")));
        QFile cmdline(QStringLiteral("/proc/cmdline"));
        const bool kept = cmdline.open(QIODevice::ReadOnly) && cmdline.readAll().split(' ').contains("persistence");
        m_kept->setText(kept ? dot(color("green"), tr("Changes are kept on the stick")) : dot(color("dim"), tr("Changes go when it's turned off")));
    }
    if (m_mode == Mode::Desktop) {
        m_protected->setVisible(writeblock::isOn());
        m_protected->setText(dot(color("amber"), tr("Drives are write-protected")));
    }
}

HomeWindow::SecureBoot HomeWindow::secureBoot(const QString &efiDir)
{
    if (!QFileInfo::exists(efiDir))
        return SecureBoot::Bios;
    // The variable is 4 bytes of attributes, then 1 when Secure Boot is on. Firmware without
    // Secure Boot doesn't have it at all; with no variables to be seen, they aren't readable.
    const QString vars = efiDir + QStringLiteral("/efivars");
    QFile var(vars + QStringLiteral("/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c"));
    if (!var.exists())
        return QDir(vars).isEmpty() ? SecureBoot::Unknown : SecureBoot::Off;
    if (!var.open(QIODevice::ReadOnly))
        return SecureBoot::Unknown;
    const QByteArray data = var.read(16);
    if (data.size() < 5)
        return SecureBoot::Unknown;
    return data.at(4) == 1 ? SecureBoot::On : SecureBoot::Off;
}

QStringList HomeWindow::desktopCommand(const QString &desktopId)
{
    QStringList dirs = applicationDirs;
    if (dirs.isEmpty()) {
        for (const QString &data : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
            dirs << data + QStringLiteral("/applications");
    }
    for (const QString &dir : std::as_const(dirs)) {
        QFile file(dir + QLatin1Char('/') + desktopId + QStringLiteral(".desktop"));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        bool inEntry = false;
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (line.startsWith(QLatin1Char('[')))
                inEntry = line == QLatin1String("[Desktop Entry]");
            else if (inEntry && line.startsWith(QLatin1String("Exec="))) {
                QStringList command = QProcess::splitCommand(line.mid(5));
                // %f, %u and the like are for files dropped on the program; there are none.
                command.removeIf([](const QString &arg) { return arg.size() == 2 && arg.startsWith(QLatin1Char('%')); });
                return command;
            }
        }
    }
    return {};
}

void HomeWindow::start(const Tile &tile)
{
    if (tile.confirm) {
        const auto answer = QMessageBox::question(this, tile.title, tr("%1\n\nRestart now?").arg(tile.text),
                                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    QString program;
    QStringList args;
    if (tile.mainWindow) {
        program = QCoreApplication::applicationFilePath();
    } else if (!tile.tool.isEmpty()) {
        program = QCoreApplication::applicationFilePath();
        args = {QStringLiteral("--open"), tile.tool};
    } else {
        args = desktopCommand(tile.desktopId);
        if (!args.isEmpty())
            program = args.takeFirst();
    }
    qCInfo(lcOps).noquote() << "Home: starting" << tile.id << program << args;
    if (program.isEmpty() || !launch(program, args))
        QMessageBox::warning(this, tile.title, tr("%1 couldn't be started.").arg(tile.title));
}
