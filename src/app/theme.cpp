// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "theme.h"

#include "rescuestick.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QIcon>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

#include <cmath>

namespace {

// Built-in themes; their names are translated here, not taken from the files.
const QStringList kBuiltIn = {QStringLiteral("classic"), QStringLiteral("deadshadow"), QStringLiteral("diskanvil-live"), QStringLiteral("high-contrast")};

QString builtInName(const QString &id)
{
    if (id == QLatin1String("classic"))
        return QCoreApplication::translate("theme", "Classic");
    if (id == QLatin1String("deadshadow"))
        return QCoreApplication::translate("theme", "Deadshadow");
    if (id == QLatin1String("diskanvil-live"))
        return QCoreApplication::translate("theme", "DiskAnvil Live");
    if (id == QLatin1String("high-contrast"))
        return QCoreApplication::translate("theme", "High Contrast");
    return id;
}

QSettings appSettings()
{
    return QSettings(QStringLiteral("diskanvil"), QStringLiteral("diskanvil"));
}

double luminance(const QColor &c)
{
    auto channel = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

double contrast(const QColor &a, const QColor &b)
{
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t), float(a.greenF() * (1 - t) + b.greenF() * t),
                            float(a.blueF() * (1 - t) + b.blueF() * t));
}

QColor rgb(quint32 value)
{
    return QColor(QRgb(0xff000000u | value));
}

// Good, warning and danger have to keep their meaning: hue in the right range, not grey.
bool meansWhatItSays(const QString &role, const QColor &c)
{
    const int hue = c.hsvHue();
    if (hue < 0 || c.hsvSaturationF() < 0.3)
        return false;
    if (role == QLatin1String("danger"))
        return hue >= 340 || hue <= 20;
    if (role == QLatin1String("warning"))
        return hue > 20 && hue <= 65;
    if (role == QLatin1String("good"))
        return hue >= 75 && hue <= 170;
    return true;
}

} // namespace

Theme &Theme::instance()
{
    static Theme theme;
    return theme;
}

Theme::Theme()
{
    // The default colors follow the desktop's (the highlight color, for one), so repaint
    // when the desktop's colors change.
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installEventFilter(this);
}

bool Theme::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == QCoreApplication::instance() && event->type() == QEvent::ApplicationPaletteChange)
        emit changed();
    return false;
}

QColor Theme::color(Role role) const
{
    const auto set = m_colors.constFind(int(role));
    if (set != m_colors.constEnd())
        return *set;
    const QPalette palette = QGuiApplication::palette();
    switch (role) {
    case Role::Partition:
    case Role::Selection:
        return palette.color(QPalette::Highlight);
    case Role::Free:
        return QColor(0x80, 0x80, 0x80);
    case Role::Good:
        return QColor(0x2e, 0xcc, 0x71);
    case Role::Warning:
        return QColor(0xe8, 0x91, 0x2d);
    case Role::Danger:
        return QColor(0xe0, 0x50, 0x50);
    case Role::Muted:
        return QColor(0x88, 0x88, 0x88);
    case Role::MapGood:
        return QColor(0x3f, 0xae, 0x5a);
    case Role::MapSlow:
        return QColor(0xe8, 0x9a, 0x2f);
    case Role::MapRetry:
        return QColor(0xd8, 0xc8, 0x3a);
    case Role::MapBad:
        return QColor(0xd9, 0x3a, 0x34);
    case Role::MapUnread:
        return palette.color(QPalette::Mid);
    case Role::UsageSmallFiles:
        return QColor(0xb8, 0xbc, 0xc4);
    case Role::UsageFile:
        return QColor(0x9a, 0xb0, 0xc8);
    case Role::Encrypted:
        return QColor(0x8e, 0x6f, 0xd8);
    }
    return palette.color(QPalette::Text);
}

QColor Theme::partitionColor(const QString &fsType) const
{
    return m_filesystems.value(fsType, color(Role::Partition));
}

QColor Theme::usageColor(int index) const
{
    if (!m_usage.isEmpty())
        return m_usage[index % m_usage.size()];
    // Well-spread hues, so neighbours rarely look alike.
    return QColor::fromHsvF(std::fmod(0.58 + index * 0.618034, 1.0), 0.45, 0.88);
}

QColor Theme::textOn(const QColor &fill)
{
    // Relative luminance, as in the WCAG contrast formula.
    auto channel = [](double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    const double l = 0.2126 * channel(fill.redF()) + 0.7152 * channel(fill.greenF()) + 0.0722 * channel(fill.blueF());
    return l > 0.18 ? QColor(0x20, 0x20, 0x20) : QColor(0xf2, 0xf2, 0xf2);
}

QVector<Theme::Choice> Theme::choices(const Addons &addons)
{
    QVector<Choice> out = {{QStringLiteral("system"), QCoreApplication::translate("theme", "System (your desktop's colors)"), true, {}}};
    for (const QString &id : kBuiltIn)
        out.push_back({id, builtInName(id), true, {}});
    for (const Addon &a : addons.all()) {
        if (!a.error.isEmpty() || a.theme.isEmpty() || kBuiltIn.contains(a.id) || a.id == QLatin1String("system"))
            continue;
        Choice c{a.id, a.name, true, {}};
        if (a.outside) {
            c.usable = false;
            c.reason = QCoreApplication::translate("theme", "Added outside DiskAnvil: press I Added It in Tools → Add-ons to use it");
        }
        out.push_back(c);
    }
    return out;
}

AddonTheme Theme::builtIn(const QString &id)
{
    if (!kBuiltIn.contains(id))
        return {};
    QFile f(QStringLiteral(":/themes/%1.json").arg(id));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return Addons::parseData(f.readAll(), f.fileName()).theme;
}

void Theme::select(const QString &id, const Addons &addons)
{
    use(id, addons);
    QSettings s = appSettings();
    s.setValue(QStringLiteral("theme"), m_id);
}

void Theme::use(const QString &id, const Addons &addons)
{
    AddonTheme theme;
    QString chosen = QStringLiteral("system");
    if (kBuiltIn.contains(id)) {
        theme = builtIn(id);
        chosen = id;
    } else {
        for (const Addon &a : addons.all()) {
            // A theme that turned up from outside isn't used until the user says it's theirs.
            if (a.id == id && a.error.isEmpty() && !a.outside && !a.theme.isEmpty()) {
                theme = a.theme;
                chosen = id;
            }
        }
    }
    m_id = chosen;
    apply(theme);
}

void Theme::restore(const Addons &addons)
{
    QString id = appSettings().value(QStringLiteral("theme"), QStringLiteral("system")).toString();
    // Its names before: Bluespark, then DiskForge Live.
    if (id == QLatin1String("bluespark") || id == QLatin1String("diskforge-live"))
        id = QStringLiteral("diskanvil-live");
    use(id, addons);
}

AddonTheme Theme::checked(const AddonTheme &theme, QStringList *replaced)
{
    AddonTheme out = theme;
    // Text on the theme's own window colors, or the desktop's.
    const QPalette palette = theme.palette.isEmpty() ? QGuiApplication::palette() : paletteFor(theme);
    const QColor background = palette.color(QPalette::Window);
    if (!theme.palette.isEmpty() && (contrast(palette.color(QPalette::Text), palette.color(QPalette::Base)) < 4.5
                                     || contrast(palette.color(QPalette::WindowText), background) < 4.5)) {
        out.palette.clear(); // unreadable text: keep the desktop's window colors
        if (replaced)
            *replaced << QStringLiteral("palette");
    }
    const bool dark = luminance(background) < 0.2;
    const QMap<QString, QColor> fallback = {
        {QStringLiteral("good"), dark ? QColor(0x4c, 0xd9, 0x7b) : QColor(0x2e, 0xcc, 0x71)},
        {QStringLiteral("warning"), dark ? QColor(0xff, 0xb3, 0x47) : QColor(0xe8, 0x91, 0x2d)},
        {QStringLiteral("danger"), dark ? QColor(0xff, 0x6b, 0x6b) : QColor(0xe0, 0x50, 0x50)},
    };
    for (auto it = fallback.constBegin(); it != fallback.constEnd(); ++it) {
        if (!out.colors.contains(it.key()))
            continue;
        // The point is that a theme can't hide a warning or make it look harmless, not to
        // grade colors: so "nearly invisible" (under 2:1) or the wrong meaning gets replaced.
        const QColor c = rgb(out.colors.value(it.key()));
        if ((!meansWhatItSays(it.key(), c) || contrast(c, background) < 2.0) && c != it.value()) {
            out.colors.insert(it.key(), it.value().rgb() & 0xffffffu);
            if (replaced)
                *replaced << it.key();
        }
    }
    return out;
}

QPalette Theme::paletteFor(const AddonTheme &theme)
{
    const QPalette desktop = QGuiApplication::palette();
    auto pick = [&theme](const char *name, const QColor &fallback) {
        return theme.palette.contains(QLatin1String(name)) ? rgb(theme.palette.value(QLatin1String(name))) : fallback;
    };
    const QColor window = pick("window", desktop.color(QPalette::Window));
    const QColor text = pick("text", desktop.color(QPalette::WindowText));
    const QColor base = pick("base", desktop.color(QPalette::Base));
    const QColor alternate = pick("alternate", mix(base, text, 0.04));
    const QColor button = pick("button", window);
    const QColor highlight = pick("highlight", desktop.color(QPalette::Highlight));
    const QColor highlighted = pick("highlighted_text", textOn(highlight));
    const QColor link = pick("link", highlight);
    const QColor mid = pick("mid", mix(window, text, 0.3));
    QPalette p;
    for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        // Greyed-out things (like add-on actions that don't fit) fade toward the background.
        const bool off = group == QPalette::Disabled;
        const QColor fg = off ? mix(text, window, 0.5) : text;
        p.setColor(group, QPalette::Window, window);
        p.setColor(group, QPalette::WindowText, fg);
        p.setColor(group, QPalette::Base, base);
        p.setColor(group, QPalette::AlternateBase, alternate);
        p.setColor(group, QPalette::Text, off ? mix(text, base, 0.5) : text);
        p.setColor(group, QPalette::Button, button);
        p.setColor(group, QPalette::ButtonText, off ? mix(text, button, 0.5) : text);
        p.setColor(group, QPalette::Highlight, off ? mid : highlight);
        p.setColor(group, QPalette::HighlightedText, highlighted);
        p.setColor(group, QPalette::Link, link);
        p.setColor(group, QPalette::LinkVisited, mix(link, text, 0.3));
        p.setColor(group, QPalette::Mid, mid);
        p.setColor(group, QPalette::Midlight, mix(button, text, 0.15));
        p.setColor(group, QPalette::Light, mix(button, QColor(Qt::white), 0.2));
        p.setColor(group, QPalette::Dark, mix(button, QColor(Qt::black), 0.4));
        p.setColor(group, QPalette::Shadow, QColor(Qt::black));
        p.setColor(group, QPalette::BrightText, QColor(Qt::white));
        p.setColor(group, QPalette::ToolTipBase, base);
        p.setColor(group, QPalette::ToolTipText, text);
        p.setColor(group, QPalette::PlaceholderText, mix(text, base, 0.5));
    }
    return p;
}

void Theme::apply(const AddonTheme &raw)
{
    auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (app && m_desktopStyle.isEmpty())
        m_desktopStyle = app->style()->name();
    // Back to the desktop's look first, so a theme without window colors is checked against
    // the desktop's, not against the theme used before it.
    if (app && raw.palette.isEmpty()) {
        if (app->style()->name() != m_desktopStyle)
            QApplication::setStyle(m_desktopStyle);
        QApplication::setPalette(QPalette());
    }
    const AddonTheme theme = checked(raw);
    static const QMap<QString, Role> roles = {
        {QStringLiteral("partition"), Role::Partition}, {QStringLiteral("free"), Role::Free}, {QStringLiteral("selection"), Role::Selection},
        {QStringLiteral("good"), Role::Good}, {QStringLiteral("warning"), Role::Warning}, {QStringLiteral("danger"), Role::Danger},
        {QStringLiteral("muted"), Role::Muted}, {QStringLiteral("map_good"), Role::MapGood}, {QStringLiteral("map_slow"), Role::MapSlow},
        {QStringLiteral("map_retry"), Role::MapRetry}, {QStringLiteral("map_bad"), Role::MapBad}, {QStringLiteral("map_unread"), Role::MapUnread},
        {QStringLiteral("usage_small_files"), Role::UsageSmallFiles}, {QStringLiteral("usage_file"), Role::UsageFile},
        {QStringLiteral("encrypted"), Role::Encrypted},
    };
    m_colors.clear();
    m_filesystems.clear();
    m_usage.clear();
    for (auto it = theme.colors.constBegin(); it != theme.colors.constEnd(); ++it) {
        if (roles.contains(it.key()))
            m_colors.insert(int(roles.value(it.key())), rgb(it.value()));
    }
    for (auto it = theme.filesystems.constBegin(); it != theme.filesystems.constEnd(); ++it)
        m_filesystems.insert(it.key(), rgb(it.value()));
    for (const quint32 c : theme.usage)
        m_usage << rgb(c);

    if (app && !theme.palette.isEmpty()) {
        // A full palette needs a style that draws with it as given.
        if (app->style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) != 0)
            QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QApplication::setPalette(paletteFor(theme));
    } else if (app && !raw.palette.isEmpty()) {
        // Its window colors weren't readable, so the desktop's stay.
        if (app->style()->name() != m_desktopStyle)
            QApplication::setStyle(m_desktopStyle);
        QApplication::setPalette(QPalette());
    }
#ifdef Q_OS_WIN
    // Windows has no icon theme to borrow from: DiskAnvil's own, dark or light like the windows.
    QIcon::setThemeName(QApplication::palette().color(QPalette::Window).lightness() < 128 ? QStringLiteral("DiskAnvil-Dark")
                                                                                         : QStringLiteral("DiskAnvil-Light"));
#else
    // DiskAnvil Live has no desktop to borrow one from: the stick's own icons (Breeze Dark under them).
    if (rescue::runningInRescue() && QIcon::themeName() != QLatin1String("DiskAnvil-Live"))
        QIcon::setThemeName(QStringLiteral("DiskAnvil-Live"));
#endif
    emit changed();
}
