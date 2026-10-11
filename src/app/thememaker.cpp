// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thememaker.h"

#include "dialogs.h"
#include "theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <tuple>

namespace {

QColor rgb(quint32 value)
{
    return QColor(QRgb(0xff000000u | value));
}

quint32 value(const QColor &c)
{
    return c.rgb() & 0xffffffu;
}

QIcon swatch(const QColor &c)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(c);
    return QIcon(pixmap);
}

// "My Dark Theme" -> "my-dark-theme"
QString slug(const QString &name)
{
    QString s = name.toLower();
    s.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    while (s.startsWith(QLatin1Char('-')))
        s.remove(0, 1);
    while (s.endsWith(QLatin1Char('-')))
        s.chop(1);
    return s.left(40);
}

} // namespace

ThemePreview::ThemePreview(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(380, 170);
}

void ThemePreview::setTheme(const AddonTheme &theme)
{
    m_theme = Theme::checked(theme);
    update();
}

void ThemePreview::paintEvent(QPaintEvent *)
{
    const QPalette pal = m_theme.palette.isEmpty() ? QApplication::palette() : Theme::paletteFor(m_theme);
    auto color = [this](const char *role, const QColor &fallback) {
        return m_theme.colors.contains(QLatin1String(role)) ? rgb(m_theme.colors.value(QLatin1String(role))) : fallback;
    };
    QPainter p(this);
    p.fillRect(rect(), pal.color(QPalette::Window));
    const QFontMetrics fm = fontMetrics();

    // A disk row: its box, two partitions and some unallocated space.
    const QRect header(8, 8, 90, 58);
    p.fillRect(header, pal.color(QPalette::Button));
    p.setPen(pal.color(QPalette::Mid));
    p.drawRect(header.adjusted(0, 0, -1, -1));
    p.setPen(pal.color(QPalette::ButtonText));
    p.drawText(header.adjusted(6, 4, 0, 0), Qt::AlignLeft | Qt::AlignTop, tr("Disk 1"));
    const QList<QPair<QString, QColor>> dots = {{tr("Healthy"), color("good", QColor(0x2e, 0xcc, 0x71))},
                                                {tr("Warning"), color("warning", QColor(0xe8, 0x91, 0x2d))},
                                                {tr("Failing"), color("danger", QColor(0xe0, 0x50, 0x50))}};
    const QColor partition = color("partition", pal.color(QPalette::Highlight));
    const QList<QPair<QString, QColor>> segments = {{QStringLiteral("ext4"), partition}, {QStringLiteral("btrfs"), partition},
                                                    {tr("Unallocated"), color("free", QColor(0x80, 0x80, 0x80))}};
    int x = header.right() + 4;
    const int width = (this->width() - x - 8) / 3;
    for (int i = 0; i < segments.size(); ++i) {
        const QRect r(x, 8, width - 4, 58);
        QColor strip = segments[i].second;
        if (i < 2 && m_theme.filesystems.contains(segments[i].first))
            strip = rgb(m_theme.filesystems.value(segments[i].first));
        p.fillRect(r, pal.color(QPalette::Base));
        p.fillRect(QRect(r.left(), r.top(), r.width(), 8), strip);
        p.setPen(i == 0 ? color("selection", pal.color(QPalette::Highlight)) : pal.color(QPalette::Mid));
        p.drawRect(r.adjusted(0, 0, -1, -1));
        p.setPen(pal.color(QPalette::Text));
        p.drawText(r.adjusted(6, 12, 0, 0), Qt::AlignLeft | Qt::AlignTop, segments[i].first);
        x += width;
    }

    // Health dots, the bad sector map and a warning.
    int y = 80;
    x = 8;
    for (const auto &[text, c] : dots) {
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRect(x, y + 3, 9, 9));
        p.setPen(pal.color(QPalette::WindowText));
        p.drawText(QPoint(x + 14, y + fm.ascent()), text);
        x += 18 + fm.horizontalAdvance(text) + 12;
    }
    y += fm.height() + 10;
    const QList<QColor> cells = {color("map_good", QColor(0x3f, 0xae, 0x5a)), color("map_slow", QColor(0xe8, 0x9a, 0x2f)),
                                 color("map_retry", QColor(0xd8, 0xc8, 0x3a)), color("map_bad", QColor(0xd9, 0x3a, 0x34)),
                                 color("map_unread", pal.color(QPalette::Mid))};
    for (int i = 0; i < 40; ++i)
        p.fillRect(QRect(8 + i * 11, y, 9, 9), cells[i < 18 ? 0 : i < 22 ? 1 : i == 22 ? 2 : i < 25 ? 3 : 4]);
    y += 22;
    QFont bold = font();
    bold.setBold(true);
    p.setFont(bold);
    p.setPen(color("danger", QColor(0xe0, 0x50, 0x50)));
    p.drawText(QPoint(8, y + fm.ascent()), tr("Everything on sdb will be erased."));
}

ThemeMaker::ThemeMaker(Addons *addons, QWidget *parent)
    : QDialog(parent)
    , m_addons(addons)
    , m_name(new QLineEdit(tr("My Theme")))
    , m_author(new QLineEdit)
    , m_ownWindow(new QCheckBox(tr("Use my own window colors (otherwise the desktop's)")))
    , m_windowBox(new QWidget)
    , m_preview(new ThemePreview)
    , m_notes(new QLabel)
{
    setWindowTitle(tr("Make a Theme"));
    m_notes->setTextFormat(Qt::PlainText);
    m_notes->setWordWrap(true);

    // Start from what's on screen now.
    const Theme &now = Theme::instance();
    const QPalette pal = QApplication::palette();
    auto *windowGrid = new QGridLayout(m_windowBox);
    windowGrid->setContentsMargins(0, 0, 0, 0);
    auto *colorGrid = new QGridLayout;
    const QList<std::tuple<QString, QString, QColor>> window = {
        {QStringLiteral("window"), tr("Window"), pal.color(QPalette::Window)},
        {QStringLiteral("base"), tr("Lists and fields"), pal.color(QPalette::Base)},
        {QStringLiteral("button"), tr("Buttons"), pal.color(QPalette::Button)},
        {QStringLiteral("text"), tr("Text"), pal.color(QPalette::WindowText)},
        {QStringLiteral("highlight"), tr("Selection"), pal.color(QPalette::Highlight)},
        {QStringLiteral("highlighted_text"), tr("Selected text"), pal.color(QPalette::HighlightedText)},
    };
    const QList<std::tuple<QString, QString, QColor>> colors = {
        {QStringLiteral("partition"), tr("Partitions"), now.color(Theme::Role::Partition)},
        {QStringLiteral("free"), tr("Unallocated"), now.color(Theme::Role::Free)},
        {QStringLiteral("good"), tr("Healthy"), now.color(Theme::Role::Good)},
        {QStringLiteral("warning"), tr("Warning"), now.color(Theme::Role::Warning)},
        {QStringLiteral("danger"), tr("Danger"), now.color(Theme::Role::Danger)},
        {QStringLiteral("map_good"), tr("Map: reads fine"), now.color(Theme::Role::MapGood)},
        {QStringLiteral("map_slow"), tr("Map: slow"), now.color(Theme::Role::MapSlow)},
        {QStringLiteral("map_bad"), tr("Map: can't be read"), now.color(Theme::Role::MapBad)},
    };
    for (const auto &[role, label, start] : window)
        addColor(role, label, start, true);
    for (const auto &[role, label, start] : colors)
        addColor(role, label, start, false);
    int i = 0;
    for (const auto &[role, label, start] : window) {
        auto *name = new QLabel(label);
        name->setTextFormat(Qt::PlainText);
        windowGrid->addWidget(name, i / 2, (i % 2) * 2);
        windowGrid->addWidget(m_buttons.value(role), i / 2, (i % 2) * 2 + 1);
        ++i;
    }
    i = 0;
    for (const auto &[role, label, start] : colors) {
        auto *name = new QLabel(label);
        name->setTextFormat(Qt::PlainText);
        colorGrid->addWidget(name, i / 2, (i % 2) * 2);
        colorGrid->addWidget(m_buttons.value(role), i / 2, (i % 2) * 2 + 1);
        ++i;
    }

    auto *form = new QFormLayout;
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("Made by:"), m_author);
    auto *windowGroup = new QGroupBox(tr("Window"));
    auto *windowLayout = new QVBoxLayout(windowGroup);
    windowLayout->addWidget(m_ownWindow);
    windowLayout->addWidget(m_windowBox);
    auto *colorGroup = new QGroupBox(tr("DiskForge's colors"));
    colorGroup->setLayout(colorGrid);
    m_ownWindow->setChecked(now.currentId() != QLatin1String("system") && !Theme::builtIn(now.currentId()).palette.isEmpty());
    connect(m_ownWindow, &QCheckBox::toggled, this, &ThemeMaker::refresh);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *saveButton = buttons->addButton(tr("Save Theme"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(saveButton, &QPushButton::clicked, this, &ThemeMaker::save);
    connect(m_name, &QLineEdit::textChanged, this, [saveButton](const QString &text) { saveButton->setEnabled(!text.trimmed().isEmpty()); });

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(windowGroup);
    layout->addWidget(colorGroup);
    layout->addWidget(m_preview);
    layout->addWidget(m_notes);
    layout->addWidget(buttons);
    refresh();
}

void ThemeMaker::addColor(const QString &role, const QString &label, const QColor &start, bool window)
{
    (window ? m_window : m_colors).insert(role, start);
    auto *button = new QPushButton(swatch(start), start.name());
    m_buttons.insert(role, button);
    connect(button, &QPushButton::clicked, this, [this, role, label, window, button] {
        QMap<QString, QColor> &map = window ? m_window : m_colors;
        const QColor picked = QColorDialog::getColor(map.value(role), this, label);
        if (!picked.isValid())
            return;
        map.insert(role, picked);
        button->setIcon(swatch(picked));
        button->setText(picked.name());
        refresh();
    });
}

AddonTheme ThemeMaker::theme() const
{
    AddonTheme t;
    if (m_ownWindow->isChecked()) {
        for (auto it = m_window.constBegin(); it != m_window.constEnd(); ++it)
            t.palette.insert(it.key(), value(it.value()));
    }
    for (auto it = m_colors.constBegin(); it != m_colors.constEnd(); ++it)
        t.colors.insert(it.key(), value(it.value()));
    t.colors.insert(QStringLiteral("selection"), value(m_window.value(QStringLiteral("highlight"))));
    return t;
}

void ThemeMaker::refresh()
{
    m_windowBox->setEnabled(m_ownWindow->isChecked());
    const AddonTheme t = theme();
    m_preview->setTheme(t);
    QStringList replaced;
    Theme::checked(t, &replaced);
    QStringList notes;
    if (replaced.contains(QLatin1String("palette")))
        notes << tr("The text isn't readable on these window colors, so the desktop's are used instead.");
    for (const QString &role : {QStringLiteral("good"), QStringLiteral("warning"), QStringLiteral("danger")}) {
        if (replaced.contains(role))
            notes << tr("\"%1\" isn't readable here or doesn't look like it means it (danger is red, warning orange, healthy "
                        "green), so DiskForge's own is used.").arg(role);
    }
    m_notes->setText(notes.join(QLatin1Char('\n')));
    m_notes->setVisible(!notes.isEmpty());
}

void ThemeMaker::save()
{
    const QString name = m_name->text().trimmed();
    const QString id = slug(name).isEmpty() ? QStringLiteral("my-theme") : slug(name);
    for (const Addon &a : m_addons->all()) {
        if (a.id == id && !askPlain(this, windowTitle(), tr("There's already an add-on called \"%1\". Replace it?").arg(a.name)))
            return;
    }
    const AddonTheme t = theme();
    QJsonObject palette, colors;
    for (auto it = t.palette.constBegin(); it != t.palette.constEnd(); ++it)
        palette.insert(it.key(), rgb(it.value()).name());
    for (auto it = t.colors.constBegin(); it != t.colors.constEnd(); ++it)
        colors.insert(it.key(), rgb(it.value()).name());
    QJsonObject themeJson{{QStringLiteral("colors"), colors}};
    if (!palette.isEmpty())
        themeJson.insert(QStringLiteral("palette"), palette);
    const QJsonObject manifest{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("version"), QStringLiteral("1.0")},
        {QStringLiteral("author"), m_author->text().trimmed()},
        {QStringLiteral("description"), tr("Made with DiskForge's Theme Maker.")},
        {QStringLiteral("theme"), themeJson},
    };
    QString error;
    if (!Addons::install(QJsonDocument(manifest).toJson(QJsonDocument::Indented), &error)) {
        warnPlain(this, windowTitle(), error);
        return;
    }
    m_addons->load();
    if (askPlain(this, windowTitle(), tr("Saved. Use \"%1\" now?").arg(name)))
        Theme::instance().select(id, *m_addons);
    accept();
}
