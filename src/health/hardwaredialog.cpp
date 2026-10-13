// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hardwaredialog.h"

#include "dialogs.h"
#include "flowlayout.h"
#include "format.h"
#include "jobui.h"
#include "pdfreport.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDate>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>

#ifdef DISKANVIL_MEDIA
#include <QCamera>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QVideoWidget>
#endif

#include <algorithm>

namespace hardware {

// Temperature (and speed) over the stress run.
class Chart : public QWidget
{
public:
    Chart()
    {
        setMinimumHeight(180);
    }
    void add(const Sample &s)
    {
        m_samples << s;
        update();
    }
    void clear()
    {
        m_samples.clear();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), palette().color(QPalette::Base));
        const QRectF area = QRectF(rect()).adjusted(48, 22, -16, -24);
        p.setPen(palette().color(QPalette::Text));
        p.drawText(QRectF(4, 2, 200, 16), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("°C"));
        for (int c = 20; c <= 100; c += 20) {
            const double y = area.bottom() - area.height() * (c - 20) / 80.0;
            p.setPen(QPen(palette().color(QPalette::Mid), 1, c == 100 || c == 80 ? Qt::DashLine : Qt::DotLine));
            p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
            p.setPen(palette().color(QPalette::Text));
            p.drawText(QRectF(0, y - 8, area.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(c));
        }
        if (m_samples.size() < 2)
            return;
        const double last = std::max(10.0, m_samples.last().seconds);
        QPainterPath line;
        bool started = false;
        for (const Sample &s : std::as_const(m_samples)) {
            if (s.celsius < 0)
                continue;
            const QPointF at(area.left() + area.width() * s.seconds / last, area.bottom() - area.height() * (std::clamp(s.celsius, 20.0, 100.0) - 20) / 80.0);
            started ? line.lineTo(at) : line.moveTo(at);
            started = true;
        }
        p.setPen(QPen(QColor(0xff, 0x33, 0x55), 2));
        p.drawPath(line);
    }

private:
    QVector<Sample> m_samples;
};

// Every key pressed, lit up as a chip.
class Keys : public QWidget
{
public:
    Keys()
    {
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(160);
        new FlowLayout(this);
        setObjectName(QStringLiteral("keys"));
    }
    int count() const { return int(m_seen.size()); }
    void press(int key, const QString &name)
    {
        if (m_seen.contains(key))
            return;
        m_seen.insert(key);
        auto *chip = new QLabel(name);
        chip->setStyleSheet(QStringLiteral("QLabel { background: #1f6fff; color: white; border-radius: 4px; padding: 3px 7px; }"));
        layout()->addWidget(chip);
    }

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        QString name = QKeySequence(e->key()).toString(QKeySequence::NativeText);
        if (name.isEmpty() || e->key() == Qt::Key_unknown)
            name = e->text().isEmpty() ? QStringLiteral("#%1").arg(e->nativeScanCode()) : e->text();
        // Left and right Shift, Ctrl and Alt are told apart by their scan code.
        press(e->key() == Qt::Key_unknown ? 0x10000000 + int(e->nativeScanCode()) : e->key() ^ (int(e->nativeScanCode()) << 20), name);
        e->accept();
    }
    bool focusNextPrevChild(bool) override { return false; } // Tab is a key to test too
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setPen(QPen(hasFocus() ? palette().color(QPalette::Highlight) : palette().color(QPalette::Mid), 1, Qt::DashLine));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 6, 6);
    }

private:
    QSet<int> m_seen;
};

} // namespace hardware

namespace {

// The whole screen in one colour at a time; a click or a key goes to the next, Esc ends.
class Colours : public QWidget
{
public:
    Colours()
    {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        setCursor(Qt::BlankCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        static const QColor colours[] = {Qt::black, Qt::white, Qt::red, Qt::green, Qt::blue, QColor(128, 128, 128)};
        QPainter p(this);
        p.fillRect(rect(), colours[m_at % 6]);
    }
    void mousePressEvent(QMouseEvent *) override { next(); }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape)
            close();
        else
            next();
    }

private:
    void next()
    {
        if (++m_at >= 6)
            close();
        else
            update();
    }
    int m_at = 0;
};

} // namespace

HardwareDialog::HardwareDialog(QWidget *parent)
    : QDialog(parent)
    , m_tabs(new QTabWidget)
{
    qRegisterMetaType<hardware::Sample>();
    qRegisterMetaType<hardware::StressResult>();
    setWindowTitle(tr("Hardware Check"));
    m_summary = hardware::read();
    auto *layout = new QVBoxLayout(this);
    m_tabs->setObjectName(QStringLiteral("tabs"));
    m_tabs->addTab(summaryTab(), tr("This PC"));
    m_tabs->addTab(processorTab(), tr("Processor"));
    m_tabs->addTab(keyboardTab(), tr("Keyboard"));
    m_tabs->addTab(screenTab(), tr("Screen"));
    m_tabs->addTab(cameraTab(), tr("Camera"));
    layout->addWidget(m_tabs, 1);
    auto *buttons = new QHBoxLayout;
    auto *pdf = new QPushButton(tr("Save Report as PDF…"));
    auto *close = new QPushButton(tr("Close"));
    buttons->addWidget(pdf);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(pdf, &QPushButton::clicked, this, [this] {
        pdfreport::saveAs(this, reportHtml(), tr("Hardware check %1.pdf").arg(QDate::currentDate().toString(Qt::ISODate)), tr("Hardware check"));
    });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    resize(720, 560);
}

HardwareDialog::~HardwareDialog()
{
    if (m_thread) {
        if (m_job)
            m_job->cancel();
        m_thread->quit();
        m_thread->wait();
    }
}

QWidget *HardwareDialog::summaryTab()
{
    auto *w = new QWidget;
    auto *form = new QFormLayout(w);
    auto label = [](const QString &text) {
        auto *l = new QLabel(text.isEmpty() ? QCoreApplication::translate("HardwareDialog", "Not known") : text);
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return l;
    };
    form->addRow(tr("Computer:"), label(m_summary.board));
    form->addRow(tr("Firmware:"), label(m_summary.firmware));
    form->addRow(tr("Processor:"), label(m_summary.threads ? tr("%1 (%2 threads)").arg(m_summary.cpu).arg(m_summary.threads) : m_summary.cpu));
    form->addRow(tr("Memory:"), label(m_summary.memory ? formatSize(m_summary.memory) : QString()));
    if (m_summary.batteries.isEmpty())
        form->addRow(tr("Battery:"), label(tr("None (a desktop, or it doesn't say)")));
    for (const hardware::Battery &b : std::as_const(m_summary.batteries)) {
        auto *l = label(hardware::batteryText(b));
        l->setObjectName(QStringLiteral("battery"));
        form->addRow(tr("Battery:"), l);
    }
    form->addRow(wrappingLabel(tr("The memory itself is tested from DiskAnvil Live's start menu (Memory Test), before anything else is "
                                  "loaded into it; drives, in Disk Health.")));
    return w;
}

QWidget *HardwareDialog::processorTab()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->addWidget(wrappingLabel(tr("Every core worked flat out for a while, its temperature drawn as it goes: a PC that gets too hot, shuts "
                                  "itself off or slows down under load shows it here. The fans get loud; that's expected.")));
    auto *row = new QHBoxLayout;
    m_minutes = new QComboBox;
    m_minutes->setObjectName(QStringLiteral("minutes"));
    for (int m : {1, 3, 10})
        m_minutes->addItem(tr("%n minute(s)", nullptr, m), m * 60);
    m_minutes->setCurrentIndex(1);
    m_go = new QPushButton(tr("Start"));
    m_go->setObjectName(QStringLiteral("go"));
    row->addWidget(new QLabel(tr("For:")));
    row->addWidget(m_minutes);
    row->addWidget(m_go);
    row->addStretch();
    v->addLayout(row);
    m_chart = new hardware::Chart;
    v->addWidget(m_chart, 1);
    m_stressText = new QLabel;
    m_stressText->setObjectName(QStringLiteral("stressText"));
    m_stressText->setWordWrap(true);
    v->addWidget(m_stressText);
    connect(m_go, &QPushButton::clicked, this, [this] {
        if (m_job) {
            m_job->cancel();
            return;
        }
        startStress(m_minutes->currentData().toInt());
    });
    return w;
}

void HardwareDialog::startStress(int seconds)
{
    if (m_job)
        return;
    m_chart->clear();
    m_go->setText(tr("Stop"));
    m_minutes->setEnabled(false);
    m_stressText->setText(tr("Working…"));
    auto *job = new hardware::Stress(seconds);
    m_job = job;
    connect(job, &hardware::Stress::sample, this, [this](const hardware::Sample &s) {
        m_chart->add(s);
        QStringList now;
        if (s.celsius > 0)
            now << tr("%1 °C").arg(qRound(s.celsius));
        if (s.mhz > 0)
            now << tr("%1 MHz").arg(qRound(s.mhz));
        m_stressText->setText(now.isEmpty() ? tr("Working… (this PC doesn't tell its processor's temperature or speed)")
                                            : tr("Working… %1").arg(now.join(QStringLiteral(", "))));
    });
    connect(job, &hardware::Stress::finished, this, &HardwareDialog::stressFinished);
    m_thread = startOnThread(this, job);
}

void HardwareDialog::stressFinished(const hardware::StressResult &r)
{
    m_thread->quit();
    m_thread->wait();
    m_thread = nullptr;
    m_job = nullptr;
    m_stress = r;
    m_go->setText(tr("Start"));
    m_minutes->setEnabled(true);
    m_stressText->setText((r.completed ? QString() : tr("Stopped early. ")) + hardware::describe(r));
    emit stressDone();
}

QWidget *HardwareDialog::keyboardTab()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->addWidget(wrappingLabel(tr("Click in the box, then press every key once: each one lights up. One that doesn't is broken (or "
                                  "the keyboard's cable or ribbon is). Keys the system keeps for itself, like the Windows key or Fn, may "
                                  "not show.")));
    m_keys = new hardware::Keys;
    v->addWidget(m_keys, 1);
    return w;
}

int HardwareDialog::keysPressed() const
{
    return m_keys ? m_keys->count() : 0;
}

QWidget *HardwareDialog::screenTab()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->addWidget(wrappingLabel(tr("The whole screen in one colour at a time: black, white, red, green, blue and grey. Look for dots that "
                                  "stay one colour (stuck or dead pixels), dark corners, lines and patches. Click or press a key for the "
                                  "next colour; Esc ends it.")));
    auto *show = new QPushButton(tr("Show the Colours"));
    show->setObjectName(QStringLiteral("showColours"));
    auto *row = new QHBoxLayout;
    row->addWidget(show);
    row->addStretch();
    v->addLayout(row);
    v->addStretch();
    connect(show, &QPushButton::clicked, this, [this] {
        auto *c = new Colours;
        c->setAttribute(Qt::WA_DeleteOnClose);
        c->showFullScreen();
        m_screenShown = true;
    });
    return w;
}

QWidget *HardwareDialog::cameraTab()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
#ifdef DISKANVIL_MEDIA
    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    if (cameras.isEmpty()) {
        v->addWidget(wrappingLabel(tr("No camera was found. A laptop's may be switched off with a key or a slider over the lens, or in "
                                      "its firmware settings.")));
        m_camera = tr("none found");
        v->addStretch();
        return w;
    }
    auto *row = new QHBoxLayout;
    auto *pick = new QComboBox;
    for (const QCameraDevice &c : cameras)
        pick->addItem(c.description());
    auto *start = new QPushButton(tr("Turn It On"));
    row->addWidget(pick, 1);
    row->addWidget(start);
    v->addLayout(row);
    auto *video = new QVideoWidget;
    video->setMinimumHeight(240);
    v->addWidget(video, 1);
    auto *session = new QMediaCaptureSession(w);
    session->setVideoOutput(video);
    connect(start, &QPushButton::clicked, w, [this, w, cameras, pick, session, start] {
        if (session->camera()) {
            session->camera()->stop();
            session->camera()->deleteLater();
            session->setCamera(nullptr);
            start->setText(tr("Turn It On"));
            return;
        }
        auto *camera = new QCamera(cameras.value(pick->currentIndex()), w);
        session->setCamera(camera);
        camera->start();
        m_camera = tr("%1 shown").arg(pick->currentText());
        start->setText(tr("Turn It Off"));
    });
#else
    v->addWidget(wrappingLabel(tr("This build of DiskAnvil has no video support, so it can't show the camera.")));
    m_camera = tr("not checked");
    v->addStretch();
#endif
    return w;
}

QString HardwareDialog::reportHtml() const
{
    QStringList other;
    other << tr("Keyboard") + QLatin1Char('\t') + (keysPressed() ? tr("%n key(s) pressed and seen", nullptr, keysPressed()) : tr("not checked"));
    other << tr("Screen") + QLatin1Char('\t') + (m_screenShown ? tr("colours shown") : tr("not checked"));
    if (!m_camera.isEmpty())
        other << tr("Camera") + QLatin1Char('\t') + m_camera;
    return hardware::html(m_summary, &m_stress, other);
}
