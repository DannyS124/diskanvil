// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later
#include "viewer.h"

#include "filetypes.h"
#include "format.h"
#include "gpt.h"
#include "hostprocess.h"
#include "mediainfo.h"
#include "officetext.h"
#include "ole.h"
#include "pictures.h"
#include "preview.h"
#include "textkinds.h"
#include "hexview.h"
#include "structure.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMovie>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStringDecoder>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#ifdef DISKANVIL_PDF
#include <poppler-qt6.h>
#endif
#ifdef DISKANVIL_MEDIA
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QVideoWidget>
#endif
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

#include <cmath>
#include <cstdlib>

Q_DECLARE_METATYPE(viewer::Message)

namespace viewer {
namespace {

constexpr quint32 kMagic = 0x44465657; // "DFVW"
constexpr quint32 kMostMessage = 4 * 1024 * 1024;

QString tr(const char *text)
{
    return QCoreApplication::translate("viewer", text);
}

QString themeIconName(const char *name)
{
    return QString::fromLatin1(name);
}

QIcon icon(const char *name)
{
    return QIcon::fromTheme(themeIconName(name));
}

} // namespace

Kind kindFor(int type, const QString &name)
{
    static const QStringList words = {QStringLiteral("txt"),  QStringLiteral("csv"),  QStringLiteral("md"),   QStringLiteral("log"),
                                      QStringLiteral("json"), QStringLiteral("xml"),  QStringLiteral("ini"),  QStringLiteral("cfg"),
                                      QStringLiteral("conf"), QStringLiteral("html"), QStringLiteral("htm"),  QStringLiteral("srt"),
                                      QStringLiteral("eml"),  QStringLiteral("rtf"),  QStringLiteral("tex"),  QStringLiteral("yml"),
                                      QStringLiteral("yaml"), QStringLiteral("sql"),  QStringLiteral("py"),   QStringLiteral("cpp"),
                                      QStringLiteral("c"),    QStringLiteral("h"),    QStringLiteral("js"),   QStringLiteral("sh")};
    if (type >= 0 && type < filetypes::types().size()) {
        const filetypes::Type &t = filetypes::types()[type];
        if (t.id == QLatin1String("pdf"))
            return Kind::Pdf;
        if (t.id == QLatin1String("ole") || t.id == QLatin1String("xml"))
            return Kind::Words;
        switch (t.category) {
        case filetypes::Category::Picture:
            return Kind::Picture;
        case filetypes::Category::Video:
            return Kind::Film;
        case filetypes::Category::Music:
            return Kind::Song;
        case filetypes::Category::Document:
        case filetypes::Category::Archive:
            return Kind::Words;
        default:
            break;
        }
    }
    return words.contains(name.section(QLatin1Char('.'), -1).toLower()) ? Kind::Words : Kind::Bytes;
}

// --- The messages

QByteArray encode(const Message &m)
{
    QByteArray body;
    QDataStream s(&body, QIODevice::WriteOnly);
    switch (m.type) {
    case Message::Look:
        s << m.style << m.palette;
        break;
    case Message::ShowFile:
        s << qint32(m.show.kind) << qint32(m.show.type) << m.show.path << m.show.name << m.show.where;
        preview::writeStrings(s, m.show.facts);
        s << m.show.previous << m.show.next << m.show.openable << m.show.repairable;
        break;
    case Message::AskFor:
        s << qint32(m.ask);
        break;
    case Message::Facts:
        preview::writeStrings(s, m.show.facts);
        break;
    case Message::Edited:
        s << quint32(m.edits.size());
        for (auto it = m.edits.cbegin(); it != m.edits.cend(); ++it)
            s << it.key() << it.value();
        break;
    }
    QByteArray out;
    QDataStream header(&out, QIODevice::WriteOnly);
    header << kMagic << quint32(m.type) << quint32(body.size());
    return out + body;
}

bool decode(QByteArray &buffer, Message &m, bool *bad)
{
    *bad = false;
    if (buffer.size() < 12)
        return false;
    QDataStream header(buffer);
    quint32 magic = 0, type = 0, length = 0;
    header >> magic >> type >> length;
    if (magic != kMagic || length > kMostMessage || type < Message::Look || type > Message::Edited) {
        *bad = true;
        return false;
    }
    if (quint32(buffer.size()) < 12 + length)
        return false;
    QDataStream s(buffer.mid(12, length));
    m = Message();
    m.type = Message::Type(type);
    switch (m.type) {
    case Message::Look:
        s >> m.style >> m.palette;
        break;
    case Message::ShowFile: {
        qint32 kind = 0, fileType = -1;
        s >> kind >> fileType >> m.show.path >> m.show.name >> m.show.where;
        preview::readStrings(s, m.show.facts);
        s >> m.show.previous >> m.show.next >> m.show.openable >> m.show.repairable;
        m.show.kind = kind >= qint32(Kind::Picture) && kind <= qint32(Kind::Bytes) ? Kind(kind) : Kind::Bytes;
        m.show.type = fileType;
        break;
    }
    case Message::AskFor: {
        qint32 ask = 0;
        s >> ask;
        m.ask = ask >= 0 && ask <= qint32(Ask::Repair) ? Ask(ask) : Ask::Ready;
        break;
    }
    case Message::Facts:
        preview::readStrings(s, m.show.facts);
        break;
    case Message::Edited: {
        quint32 count = 0;
        s >> count;
        // Nine bytes each: a made-up count can't ask for more than the message holds.
        for (quint32 i = 0; i < count && i < length / 9 && s.status() == QDataStream::Ok; ++i) {
            quint64 at = 0;
            quint8 value = 0;
            s >> at >> value;
            m.edits.insert(at, value);
        }
        break;
    }
    }
    buffer.remove(0, 12 + length);
    if (s.status() != QDataStream::Ok) {
        *bad = true;
        return false;
    }
    return true;
}

// --- Pictures: fitted to the window at first, then zoomed with the wheel or the buttons and
// moved by dragging. Double-click goes between fitted and actual size.

class PictureView : public QAbstractScrollArea
{
public:
    explicit PictureView(QWidget *parent = nullptr)
        : QAbstractScrollArea(parent)
    {
        setFrameShape(QFrame::NoFrame);
        viewport()->setCursor(Qt::OpenHandCursor);
    }

    bool load(const QString &path, int type, QString *why)
    {
        clear();
        // Raw photos and Photoshop files: our own reading of them (pictures.h), the same as the previews.
        const QString id = type >= 0 && type < filetypes::types().size() ? filetypes::types()[type].id : QString();
        if (pictures::isRaw(id) || id == QLatin1String("psd")) {
            QFile file(path);
            QString problem;
            m_image = file.open(QIODevice::ReadOnly) ? pictures::decode(file.read(512ll * 1024 * 1024), type, 0, &problem) : QImage();
            if (m_image.isNull()) {
                *why = viewer::tr("This picture can't be shown: %1.").arg(problem.isEmpty() ? file.errorString() : problem);
                return false;
            }
            fit();
            return true;
        }
        QImageReader reader(path, pictures::readerFormat(id));
        reader.setAutoTransform(true); // the way the camera was held
        if (reader.supportsAnimation() && reader.imageCount() > 1) {
            m_movie = new QMovie(path, QByteArray(), this);
            if (m_movie->isValid()) {
                QObject::connect(m_movie, &QMovie::frameChanged, this, [this] {
                    m_image = m_movie->currentImage();
                    viewport()->update();
                });
                m_movie->start();
                m_image = m_movie->currentImage();
                fit();
                return true;
            }
            delete m_movie;
            m_movie = nullptr;
        }
        m_image = reader.read();
        if (m_image.isNull()) {
            *why = viewer::tr("This picture can't be shown: it may be damaged, or be a kind this computer can't show (%1).").arg(reader.errorString());
            return false;
        }
        fit();
        return true;
    }

    void clear()
    {
        if (m_movie) {
            m_movie->stop();
            m_movie->deleteLater();
            m_movie = nullptr;
        }
        m_image = QImage();
        viewport()->update();
    }

    QSize pictureSize() const { return m_image.size(); }
    double zoom() const { return m_zoom; }

    void fit()
    {
        m_fit = true;
        m_zoom = fitZoom();
        updateBars();
        viewport()->update();
    }

    void actualSize() { zoomTo(1.0, viewport()->rect().center()); }

    void zoomBy(double factor) { zoomTo(m_zoom * factor, viewport()->rect().center()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(viewport());
        p.fillRect(viewport()->rect(), palette().color(QPalette::Window).darker(115));
        if (m_image.isNull())
            return;
        p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 4);
        p.drawImage(target(), m_image);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QAbstractScrollArea::resizeEvent(event);
        if (m_fit)
            m_zoom = fitZoom();
        updateBars();
    }

    void scrollContentsBy(int, int) override { viewport()->update(); }

    void wheelEvent(QWheelEvent *event) override
    {
        if (m_image.isNull())
            return;
        const double steps = event->angleDelta().y() / 120.0;
        zoomTo(m_zoom * std::pow(1.25, steps), event->position().toPoint());
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_drag = event->position().toPoint();
            viewport()->setCursor(Qt::ClosedHandCursor);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton))
            return;
        const QPoint by = event->position().toPoint() - m_drag;
        m_drag = event->position().toPoint();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - by.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - by.y());
    }

    void mouseReleaseEvent(QMouseEvent *) override { viewport()->setCursor(Qt::OpenHandCursor); }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (m_fit && std::abs(m_zoom - 1.0) > 0.01)
            zoomTo(1.0, event->position().toPoint());
        else
            fit();
    }

private:
    double fitZoom() const
    {
        if (m_image.isNull())
            return 1;
        const QSize room = viewport()->size();
        return std::min({1.0, room.width() / double(m_image.width()), room.height() / double(m_image.height())});
    }

    QSizeF shownSize() const { return QSizeF(m_image.size()) * m_zoom; }

    QRectF target() const
    {
        const QSizeF size = shownSize();
        const QSize room = viewport()->size();
        const double x = size.width() < room.width() ? (room.width() - size.width()) / 2 : -horizontalScrollBar()->value();
        const double y = size.height() < room.height() ? (room.height() - size.height()) / 2 : -verticalScrollBar()->value();
        return QRectF(QPointF(x, y), size);
    }

    void updateBars()
    {
        const QSizeF size = shownSize();
        const QSize room = viewport()->size();
        horizontalScrollBar()->setRange(0, std::max(0, int(size.width()) - room.width()));
        verticalScrollBar()->setRange(0, std::max(0, int(size.height()) - room.height()));
        horizontalScrollBar()->setPageStep(room.width());
        verticalScrollBar()->setPageStep(room.height());
    }

    // The point of the picture under `around` stays there.
    void zoomTo(double zoom, QPoint around)
    {
        if (m_image.isNull())
            return;
        zoom = std::clamp(zoom, std::min(fitZoom(), 0.05), 16.0);
        const QRectF before = target();
        const QPointF spot = (QPointF(around) - before.topLeft()) / m_zoom;
        m_zoom = zoom;
        m_fit = false;
        updateBars();
        const QSizeF size = shownSize();
        horizontalScrollBar()->setValue(int(spot.x() * zoom - around.x()));
        verticalScrollBar()->setValue(int(spot.y() * zoom - around.y()));
        Q_UNUSED(size)
        viewport()->update();
    }

    QImage m_image;
    QMovie *m_movie = nullptr;
    double m_zoom = 1;
    bool m_fit = true;
    QPoint m_drag;
};

// --- PDFs: every page under the one before, fitted to the window's width, drawn as they come
// into view.

class PdfView : public QScrollArea
{
public:
    explicit PdfView(QWidget *parent = nullptr)
        : QScrollArea(parent)
        , m_column(new QWidget)
        , m_layout(new QVBoxLayout(m_column))
    {
        setWidgetResizable(true);
        setWidget(m_column);
        m_layout->setSpacing(12);
        m_layout->setAlignment(Qt::AlignHCenter);
        m_draw.setSingleShot(true);
        m_draw.setInterval(60);
        QObject::connect(&m_draw, &QTimer::timeout, this, [this] { drawVisible(); });
        QObject::connect(verticalScrollBar(), &QScrollBar::valueChanged, &m_draw, qOverload<>(&QTimer::start));
    }

    bool load(const QString &path, QString *why)
    {
        clear();
#ifdef DISKANVIL_PDF
        m_document = Poppler::Document::load(path);
        if (!m_document) {
            *why = viewer::tr("This PDF can't be shown: it may be damaged.");
            return false;
        }
        if (m_document->isLocked()) {
            *why = viewer::tr("This PDF is locked with a password, so its pages can't be shown.");
            m_document.reset();
            return false;
        }
        m_document->setRenderHint(Poppler::Document::Antialiasing);
        m_document->setRenderHint(Poppler::Document::TextAntialiasing);
        const int pages = std::min(m_document->numPages(), 2000);
        for (int i = 0; i < pages; ++i) {
            auto *page = new QLabel;
            page->setAlignment(Qt::AlignCenter);
            page->setAutoFillBackground(true);
            QPalette white = page->palette();
            white.setColor(QPalette::Window, Qt::white);
            page->setPalette(white);
            m_pages << page;
            m_layout->addWidget(page);
        }
        m_zoom = 0; // fitted to the width once there's a width
        relayout();
        return !m_pages.isEmpty();
#else
        Q_UNUSED(path)
        *why = viewer::tr("PDF pages can't be shown: this DiskAnvil was built without poppler.");
        return false;
#endif
    }

    void clear()
    {
        for (QLabel *page : std::as_const(m_pages))
            delete page;
        m_pages.clear();
#ifdef DISKANVIL_PDF
        m_document.reset();
#endif
    }

    int pages() const { return int(m_pages.size()); }
    void zoomBy(double factor) { setZoom(currentZoom() * factor); }
    void fit() { setZoom(0); }
    void actualSize() { setZoom(1.0); }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QScrollArea::resizeEvent(event);
        if (m_zoom == 0)
            relayout();
    }

private:
    // Points to pixels: 1.0 is the page's printed size on a 96-dpi screen.
    double currentZoom() const
    {
#ifdef DISKANVIL_PDF
        if (m_zoom > 0 || !m_document || m_document->numPages() == 0)
            return m_zoom > 0 ? m_zoom : 1.0;
        std::unique_ptr<Poppler::Page> first(m_document->page(0));
        const double width = first ? first->pageSizeF().width() * 96 / 72 : 800;
        return std::max(0.1, (viewport()->width() - 40) / width);
#else
        return 1.0;
#endif
    }

    void setZoom(double zoom)
    {
        m_zoom = zoom == 0 ? 0 : std::clamp(zoom, 0.1, 8.0);
        relayout();
    }

    void relayout()
    {
#ifdef DISKANVIL_PDF
        if (!m_document)
            return;
        const double zoom = currentZoom();
        for (int i = 0; i < m_pages.size(); ++i) {
            std::unique_ptr<Poppler::Page> page(m_document->page(i));
            const QSizeF points = page ? page->pageSizeF() : QSizeF(595, 842);
            m_pages[i]->setFixedSize((points * 96 / 72 * zoom).toSize());
            m_pages[i]->setPixmap(QPixmap());
            m_pages[i]->setProperty("drawnAt", 0.0);
        }
        m_draw.start();
#endif
    }

    void drawVisible()
    {
#ifdef DISKANVIL_PDF
        if (!m_document)
            return;
        const double zoom = currentZoom();
        const qreal ratio = devicePixelRatioF();
        const QRect seen(QPoint(0, verticalScrollBar()->value()), viewport()->size());
        for (int i = 0; i < m_pages.size(); ++i) {
            QLabel *label = m_pages[i];
            const bool near = label->geometry().intersects(seen.adjusted(0, -seen.height(), 0, seen.height()));
            if (!near) {
                // Far away: let go of its picture, so a long PDF doesn't fill the memory.
                if (!label->pixmap().isNull()) {
                    label->setPixmap(QPixmap());
                    label->setProperty("drawnAt", 0.0);
                }
                continue;
            }
            if (label->property("drawnAt").toDouble() == zoom)
                continue;
            std::unique_ptr<Poppler::Page> page(m_document->page(i));
            if (!page)
                continue;
            const double dpi = 96 * zoom * ratio;
            QImage image = page->renderToImage(dpi, dpi);
            QPixmap pixmap = QPixmap::fromImage(image);
            pixmap.setDevicePixelRatio(ratio);
            label->setPixmap(pixmap);
            label->setProperty("drawnAt", zoom);
        }
#endif
    }

    QWidget *m_column;
    QVBoxLayout *m_layout;
    QVector<QLabel *> m_pages;
    double m_zoom = 0; // 0: fitted to the width
    QTimer m_draw;
#ifdef DISKANVIL_PDF
    std::unique_ptr<Poppler::Document> m_document;
#endif
};

// --- Films and songs: played, with the usual buttons. A song shows its cover.

class MediaView : public QWidget
{
public:
    explicit MediaView(QWidget *parent = nullptr)
        : QWidget(parent)
        , m_screen(new QStackedWidget)
        , m_cover(new QLabel)
        , m_play(new QToolButton)
        , m_position(new QSlider(Qt::Horizontal))
        , m_time(new QLabel)
        , m_volume(new QSlider(Qt::Horizontal))
        , m_message(new QLabel)
    {
        auto *layout = new QVBoxLayout(this);
        m_cover->setAlignment(Qt::AlignCenter);
        m_message->setWordWrap(true);
        m_message->setAlignment(Qt::AlignCenter);
        m_message->hide();
#ifdef DISKANVIL_MEDIA
        m_player = new QMediaPlayer(this);
        m_audio = new QAudioOutput(this);
        m_video = new QVideoWidget;
        m_player->setAudioOutput(m_audio);
        m_player->setVideoOutput(m_video);
        m_screen->addWidget(m_video);
        QObject::connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this] { showState(); });
        // At the end it goes back to the start, paused, showing the first picture (not an empty screen).
        QObject::connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
            if (status == QMediaPlayer::EndOfMedia) {
                m_player->setPosition(0);
                m_player->pause();
            }
        });
        QObject::connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 ms) {
            m_position->setRange(0, int(ms / 100));
            showTime();
        });
        QObject::connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
            if (!m_position->isSliderDown())
                m_position->setValue(int(ms / 100));
            showTime();
        });
        QObject::connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &text) {
            m_message->setText(viewer::tr("It can't be played: %1. It may be damaged, or be a kind this computer has nothing to play with.").arg(text));
            m_message->show();
        });
        QObject::connect(m_position, &QSlider::sliderMoved, this, [this](int tenths) { m_player->setPosition(qint64(tenths) * 100); });
        QObject::connect(m_play, &QToolButton::clicked, this, [this] { playPause(); });
        QObject::connect(m_volume, &QSlider::valueChanged, this, [this](int v) { m_audio->setVolume(v / 100.0f); });
#else
        m_message->setText(viewer::tr("Films and songs can't be played here: this DiskAnvil was built without Qt Multimedia. What's known about it is "
                              "on the right."));
        m_message->show();
        m_play->setEnabled(false);
        m_position->setEnabled(false);
#endif
        m_screen->addWidget(m_cover);
        layout->addWidget(m_screen, 1);
        layout->addWidget(m_message);
        auto *controls = new QHBoxLayout;
        m_play->setIcon(icon("media-playback-start"));
        m_play->setToolTip(viewer::tr("Play (Space)"));
        controls->addWidget(m_play);
        controls->addWidget(m_position, 1);
        m_time->setObjectName(QStringLiteral("time"));
        controls->addWidget(m_time);
        auto *speaker = new QLabel;
        speaker->setPixmap(icon("audio-volume-high").pixmap(16, 16));
        controls->addWidget(speaker);
        m_volume->setRange(0, 100);
        m_volume->setValue(80);
        m_volume->setMaximumWidth(120);
        controls->addWidget(m_volume);
        layout->addLayout(controls);
    }

    void load(const QString &path, bool film)
    {
        stop();
        m_message->hide();
        // A song shows its cover, from its own tags.
        if (!film) {
            QFile file(path);
            QByteArray head;
            if (file.open(QIODevice::ReadOnly))
                head = file.read(16 * 1024 * 1024);
            const mediainfo::Info info = mediainfo::read(filetypes::typeForName(path), head, quint64(file.size()));
            QImage cover;
            cover.loadFromData(info.cover);
            m_cover->setPixmap(cover.isNull() ? icon("audio-x-generic").pixmap(160, 160)
                                              : QPixmap::fromImage(cover.scaled(480, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            m_screen->setCurrentWidget(m_cover);
        }
#ifdef DISKANVIL_MEDIA
        if (film)
            m_screen->setCurrentWidget(m_video);
        m_player->setSource(QUrl::fromLocalFile(path));
        m_player->play();
#else
        if (film) {
            m_cover->setPixmap(icon("video-x-generic").pixmap(160, 160));
            m_screen->setCurrentWidget(m_cover);
        }
#endif
    }

    void stop()
    {
#ifdef DISKANVIL_MEDIA
        m_player->stop();
        m_player->setSource(QUrl());
#endif
    }

    void playPause()
    {
#ifdef DISKANVIL_MEDIA
        if (m_player->playbackState() == QMediaPlayer::PlayingState)
            m_player->pause();
        else
            m_player->play();
#endif
    }

    bool playing() const
    {
#ifdef DISKANVIL_MEDIA
        return m_player->playbackState() == QMediaPlayer::PlayingState;
#else
        return false;
#endif
    }

    QString problem() const { return m_message->isVisible() ? m_message->text() : QString(); }

private:
#ifdef DISKANVIL_MEDIA
    void showState()
    {
        const bool playing = m_player->playbackState() == QMediaPlayer::PlayingState;
        m_play->setIcon(icon(playing ? "media-playback-pause" : "media-playback-start"));
        m_play->setToolTip(playing ? viewer::tr("Pause (Space)") : viewer::tr("Play (Space)"));
    }

    void showTime()
    {
        const auto clock = [](qint64 ms) {
            const qint64 s = ms / 1000;
            return s >= 3600 ? QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'))
                             : QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
        };
        m_time->setText(clock(m_player->position()) + QStringLiteral(" / ") + clock(m_player->duration()));
    }

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QVideoWidget *m_video = nullptr;
#endif
    QStackedWidget *m_screen;
    QLabel *m_cover;
    QToolButton *m_play;
    QSlider *m_position;
    QLabel *m_time;
    QSlider *m_volume;
    QLabel *m_message;
};

// --- Inside: what the file is made of, part by part, next to its bytes, which can be changed in
// a copy (the file shown is never written; Find Lost Files saves the changed copy).

class InsideView : public QWidget
{
    Q_OBJECT
public:
    explicit InsideView(QWidget *parent = nullptr)
        : QWidget(parent)
        , m_parts(new QTreeWidget)
        , m_hex(new HexView)
        , m_edit(new QPushButton(viewer::tr("Change Bytes")))
        , m_undo(new QPushButton(viewer::tr("Undo")))
        , m_saveEdited(new QPushButton(viewer::tr("Save the Changed Copy…")))
        , m_changed(new QLabel)
        , m_note(new QLabel)
    {
        setObjectName(QStringLiteral("inside"));
        m_parts->setObjectName(QStringLiteral("parts"));
        m_parts->setHeaderLabels({viewer::tr("Part"), viewer::tr("Where"), viewer::tr("Size")});
        m_parts->setRootIsDecorated(true);
        m_parts->setUniformRowHeights(true);
        m_parts->header()->setStretchLastSection(false);
        m_parts->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_hex->setObjectName(QStringLiteral("hex"));
        m_edit->setCheckable(true);
        m_edit->setObjectName(QStringLiteral("changeBytes"));
        m_edit->setToolTip(viewer::tr("Type over the bytes, in hex or as text (Tab goes between them). Only a copy is changed."));
        m_saveEdited->setObjectName(QStringLiteral("saveEdited"));
        m_undo->setEnabled(false);
        m_saveEdited->setEnabled(false);
        m_note->setWordWrap(true);
        m_note->setObjectName(QStringLiteral("insideNote"));
        auto *buttons = new QHBoxLayout;
        buttons->addWidget(m_note, 1);
        buttons->addWidget(m_changed);
        buttons->addWidget(m_edit);
        buttons->addWidget(m_undo);
        buttons->addWidget(m_saveEdited);
        auto *split = new QSplitter;
        split->addWidget(m_parts);
        split->addWidget(m_hex);
        split->setStretchFactor(0, 2);
        split->setStretchFactor(1, 3);
        split->setChildrenCollapsible(false);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addLayout(buttons);
        layout->addWidget(split, 1);
        connect(m_parts, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
            if (item)
                m_hex->mark(item->data(0, Qt::UserRole).toULongLong(), item->data(0, Qt::UserRole + 1).toULongLong());
        });
        connect(m_edit, &QPushButton::toggled, m_hex, &HexView::setEditing);
        connect(m_undo, &QPushButton::clicked, m_hex, &HexView::undo);
        connect(m_hex, &HexView::editsChanged, this, [this](int count) {
            m_changed->setText(count ? QCoreApplication::translate("viewer", "%n byte(s) changed", nullptr, count) : QString());
            m_undo->setEnabled(count > 0);
            m_saveEdited->setEnabled(count > 0);
        });
        connect(m_saveEdited, &QPushButton::clicked, this, [this] { Q_EMIT saveEdited(); });
    }

    void load(const QString &path, int type)
    {
        ++m_generation;
        m_parts->clear();
        m_edit->setChecked(false);
        QString why;
        if (!m_hex->open(path, &why)) {
            m_note->setText(viewer::tr("Its bytes can't be read: %1").arg(why));
            return;
        }
        m_note->setText(viewer::tr("Taking it apart…"));
        // Taken apart on a thread of its own (a big file takes a moment), from a mapping of its own.
        const int generation = m_generation;
        QPointer<InsideView> self(this);
        QThread *worker = QThread::create([self, path, type, generation] {
            QFile file(path);
            QVector<structure::Part> parts;
            if (file.open(QIODevice::ReadOnly)) {
                const uchar *data = file.size() ? file.map(0, file.size()) : nullptr;
                QByteArray memory;
                if (!data && file.size()) {
                    memory = file.read(256 * 1024 * 1024);
                    data = reinterpret_cast<const uchar *>(memory.constData());
                }
                parts = structure::parts(type, data, memory.isEmpty() ? quint64(file.size()) : quint64(memory.size()));
            }
            QMetaObject::invokeMethod(qApp, [self, parts, generation] {
                if (self && self->m_generation == generation)
                    self->showParts(parts);
            }, Qt::QueuedConnection);
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    }

    QMap<quint64, quint8> edits() const { return m_hex->edits(); }
    int partCount() const { return m_parts->topLevelItemCount(); }
    HexView *hex() const { return m_hex; }

Q_SIGNALS:
    void saveEdited();

private:
    void showParts(const QVector<structure::Part> &parts)
    {
        m_parts->clear();
        QVector<QTreeWidgetItem *> parents;
        int damaged = 0;
        const QColor bad(0xff, 0x33, 0x55);
        for (const structure::Part &p : parts) {
            const QStringList columns = {p.name, QString::number(p.at), formatSize(p.size)};
            QTreeWidgetItem *item = p.depth > 0 && p.depth <= parents.size() ? new QTreeWidgetItem(parents[p.depth - 1], columns)
                                                                                : new QTreeWidgetItem(m_parts, columns);
            parents.resize(p.depth);
            parents << item;
            item->setData(0, Qt::UserRole, p.at);
            item->setData(0, Qt::UserRole + 1, p.size);
            QString tip = p.detail;
            if (!p.problem.isEmpty()) {
                ++damaged;
                tip += (tip.isEmpty() ? QString() : QStringLiteral("\n")) + p.problem;
                for (int c = 0; c < 3; ++c)
                    item->setForeground(c, bad);
                item->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-warning")));
            }
            item->setToolTip(0, tip);
            if (!p.detail.isEmpty())
                item->setText(0, p.name + QStringLiteral(" · ") + p.detail);
        }
        m_parts->expandToDepth(1);
        m_parts->resizeColumnToContents(1);
        m_parts->resizeColumnToContents(2);
        m_note->setText(damaged == 0 ? viewer::tr("Every part of it checks out.")
                                     : QCoreApplication::translate("viewer", "%n part(s) damaged, in red: pick one to see where it is.", nullptr, damaged));
        if (parts.size() >= structure::kMostParts)
            m_note->setText(m_note->text() + QLatin1Char(' ') + viewer::tr("(Only its first %1 parts are listed.)").arg(structure::kMostParts));
    }

    QTreeWidget *m_parts;
    HexView *m_hex;
    QPushButton *m_edit, *m_undo, *m_saveEdited;
    QLabel *m_changed, *m_note;
    int m_generation = 0;
};

// --- The window

Window::Window(QWidget *parent)
    : QMainWindow(parent)
    , m_pages(new QStackedWidget)
    , m_picture(new PictureView)
    , m_pdf(new PdfView)
    , m_media(new MediaView)
    , m_words(new QPlainTextEdit)
    , m_bytes(new QPlainTextEdit)
    , m_inside(new InsideView)
    , m_problem(new QLabel)
    , m_title(new QLabel)
    , m_facts(new QLabel)
    , m_side(new QScrollArea)
{
    setWindowTitle(tr("Find Lost Files: Viewer"));
    setObjectName(QStringLiteral("viewer"));
    auto *bar = addToolBar(tr("Viewer"));
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_previous = bar->addAction(icon("go-previous"), tr("Previous"));
    m_previous->setToolTip(tr("The one before (Left)"));
    m_next = bar->addAction(icon("go-next"), tr("Next"));
    m_next->setToolTip(tr("The one after (Right)"));
    m_title->setObjectName(QStringLiteral("title"));
    m_title->setContentsMargins(12, 0, 12, 0);
    m_title->setTextFormat(Qt::PlainText);
    bar->addWidget(m_title);
    auto *gap = new QWidget;
    gap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(gap);
    m_zoomOut = bar->addAction(icon("zoom-out"), tr("Smaller"));
    m_fit = bar->addAction(icon("zoom-fit-best"), tr("Fit"));
    m_actual = bar->addAction(icon("zoom-original"), tr("Actual Size"));
    m_zoomIn = bar->addAction(icon("zoom-in"), tr("Bigger"));
    for (QAction *a : {m_zoomOut, m_fit, m_actual, m_zoomIn})
        bar->widgetForAction(a)->setProperty("zoom", true);
    bar->addSeparator();
    m_details = bar->addAction(icon("documentinfo"), tr("Details"));
    m_details->setCheckable(true);
    m_details->setChecked(true);
    m_insideAction = bar->addAction(icon("view-list-tree"), tr("Inside"));
    m_insideAction->setCheckable(true);
    m_insideAction->setObjectName(QStringLiteral("insideAction"));
    m_insideAction->setToolTip(tr("What the file is made of, part by part, and its bytes (damage shows in red)"));
    m_repair = bar->addAction(icon("tools-wizard"), tr("Repair"));
    m_repair->setObjectName(QStringLiteral("repairAction"));
    m_open = bar->addAction(icon("document-open"), tr("Open in Its Own Program"));
    m_save = bar->addAction(icon("document-save"), tr("Save…"));

    m_problem->setWordWrap(true);
    m_problem->setObjectName(QStringLiteral("problem"));
    m_problem->hide();
    for (QPlainTextEdit *text : {m_words, m_bytes}) {
        text->setReadOnly(true);
        text->setLineWrapMode(text == m_words ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    }
    m_bytes->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    for (QWidget *page : {static_cast<QWidget *>(m_picture), static_cast<QWidget *>(m_pdf), static_cast<QWidget *>(m_media),
                          static_cast<QWidget *>(m_words), static_cast<QWidget *>(m_bytes), static_cast<QWidget *>(m_inside)})
        m_pages->addWidget(page);

    m_facts->setWordWrap(true);
    m_facts->setTextFormat(Qt::RichText);
    m_facts->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_facts->setContentsMargins(10, 10, 10, 10);
    m_facts->setObjectName(QStringLiteral("facts"));
    m_side->setWidget(m_facts);
    m_side->setWidgetResizable(true);
    m_side->setMinimumWidth(220);

    auto *middle = new QWidget;
    auto *middleLayout = new QVBoxLayout(middle);
    middleLayout->setContentsMargins(0, 0, 0, 0);
    middleLayout->addWidget(m_problem);
    middleLayout->addWidget(m_pages, 1);
    auto *split = new QSplitter;
    split->addWidget(middle);
    split->addWidget(m_side);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    split->setChildrenCollapsible(false);
    setCentralWidget(split);

    connect(m_previous, &QAction::triggered, this, [this] { Q_EMIT asked(Ask::Previous); });
    connect(m_next, &QAction::triggered, this, [this] { Q_EMIT asked(Ask::Next); });
    connect(m_open, &QAction::triggered, this, [this] { Q_EMIT asked(Ask::Open); });
    connect(m_save, &QAction::triggered, this, [this] { Q_EMIT asked(Ask::Save); });
    connect(m_repair, &QAction::triggered, this, [this] { Q_EMIT asked(Ask::Repair); });
    connect(m_details, &QAction::toggled, m_side, &QWidget::setVisible);
    connect(m_insideAction, &QAction::toggled, this, [this](bool on) {
        if (on) {
            m_inside->load(m_shown.path, m_shown.type);
            m_pages->setCurrentWidget(m_inside);
        } else {
            m_pages->setCurrentWidget(m_normalPage ? m_normalPage : m_bytes);
        }
        for (QAction *a : {m_zoomOut, m_fit, m_actual, m_zoomIn})
            a->setVisible(!on && (m_shown.kind == Kind::Picture || m_shown.kind == Kind::Pdf));
    });
    connect(m_inside, &InsideView::saveEdited, this, [this] { Q_EMIT saveEdited(m_inside->edits()); });
    connect(m_zoomIn, &QAction::triggered, this, [this] { m_shown.kind == Kind::Pdf ? m_pdf->zoomBy(1.25) : m_picture->zoomBy(1.25); });
    connect(m_zoomOut, &QAction::triggered, this, [this] { m_shown.kind == Kind::Pdf ? m_pdf->zoomBy(0.8) : m_picture->zoomBy(0.8); });
    connect(m_fit, &QAction::triggered, this, [this] { m_shown.kind == Kind::Pdf ? m_pdf->fit() : m_picture->fit(); });
    connect(m_actual, &QAction::triggered, this, [this] { m_shown.kind == Kind::Pdf ? m_pdf->actualSize() : m_picture->actualSize(); });
}

Window::~Window() = default;

QString Window::problem() const
{
    if (m_problem->isVisible())
        return m_problem->text();
    return m_pages->currentWidget() == m_media ? m_media->problem() : QString();
}

void Window::present(const Show &show)
{
    m_shown = show;
    m_media->stop();
    m_picture->clear();
    m_pdf->clear();
    m_problem->hide();
    m_title->setText(show.where.isEmpty() ? show.name : show.name + QStringLiteral(" · ") + show.where);
    setWindowTitle(tr("%1 – Find Lost Files").arg(show.name));
    m_previous->setEnabled(show.previous);
    m_next->setEnabled(show.next);
    m_open->setEnabled(show.openable);
    m_repair->setVisible(!show.repairable.isEmpty());
    m_repair->setToolTip(show.repairable + tr(" (on a copy, which then shows here)"));
    m_open->setToolTip(show.openable ? tr("Open it in the program this computer has for it (a copy)")
                                     : tr("Programs and scripts aren't opened: they could run"));
    showFacts();

    QString why;
    Kind kind = show.kind;
    if (kind == Kind::Picture && !m_picture->load(show.path, show.type, &why))
        kind = Kind::Bytes;
    else if (kind == Kind::Pdf && !m_pdf->load(show.path, &why))
        kind = Kind::Bytes;
    else if (kind == Kind::Film || kind == Kind::Song)
        m_media->load(show.path, kind == Kind::Film);

    QFile file(show.path);
    if (kind == Kind::Words) {
        QByteArray data = file.open(QIODevice::ReadOnly) ? file.read(64 * 1024 * 1024) : QByteArray();
        QString text;
        const filetypes::Type *type = show.type >= 0 && show.type < filetypes::types().size() ? &filetypes::types()[show.type] : nullptr;
        if (type && QStringList{QStringLiteral("doc"), QStringLiteral("xls"), QStringLiteral("ppt"), QStringLiteral("msg"), QStringLiteral("ole")}.contains(type->id)) {
            const ole::File compound(data);
            text = ole::text(compound, 2000000);
            if (text.isEmpty() && ole::locked(compound))
                why = tr("It's locked with a password, so its words can't be shown.");
        } else if (type && QStringList{QStringLiteral("eml"), QStringLiteral("mbox"), QStringLiteral("rtf"), QStringLiteral("html"), QStringLiteral("xml"),
                                       QStringLiteral("ics"), QStringLiteral("vcf")}
                               .contains(type->id)) {
            text = texts::text(show.type, data, 2000000);
        } else if (type && type->category == filetypes::Category::Document) {
            text = officetext::text(show.type, data, 2000000);
        }
        if (text.isEmpty() && type && (type->category == filetypes::Category::Archive || type->id == QLatin1String("epub"))) {
            QStringList lines;
            for (const officetext::Entry &e : officetext::list(data, 20000))
                lines << (e.folder ? e.path : QStringLiteral("%1  (%2)").arg(e.path, formatSize(e.size)));
            text = lines.join(QLatin1Char('\n'));
        }
        if (text.isEmpty() && !type) {
            // A text file: UTF-8, or UTF-16 when it starts saying so, else the old 8-bit kind.
            data.truncate(8 * 1024 * 1024);
            auto encoding = QStringConverter::encodingForData(data);
            QStringDecoder decode(encoding.value_or(QStringConverter::Utf8));
            text = decode(data);
            if (decode.hasError())
                text = QString::fromLatin1(data);
        }
        if (text.isEmpty()) {
            if (why.isEmpty())
                why = tr("No words could be read from it: it may be damaged.");
            kind = Kind::Bytes;
        } else {
            m_words->setPlainText(text);
        }
    }
    if (kind == Kind::Bytes) {
        // Its first bytes, the way the drive has them.
        constexpr qint64 kMost = 256 * 1024;
        const QByteArray data = file.isOpen() || file.open(QIODevice::ReadOnly) ? (file.seek(0), file.read(kMost)) : QByteArray();
        QString dump = gpt::hexDump(data, 0);
        if (file.size() > kMost)
            dump += QLatin1Char('\n') + tr("…the first %1 of %2.").arg(formatSize(kMost), formatSize(quint64(file.size())));
        m_bytes->setPlainText(dump);
    }
    if (!why.isEmpty()) {
        m_problem->setText(why);
        m_problem->show();
    }
    m_shown.kind = kind;
    QWidget *page = kind == Kind::Picture ? static_cast<QWidget *>(m_picture)
                  : kind == Kind::Pdf     ? static_cast<QWidget *>(m_pdf)
                  : kind == Kind::Film || kind == Kind::Song ? static_cast<QWidget *>(m_media)
                  : kind == Kind::Words   ? static_cast<QWidget *>(m_words)
                                          : static_cast<QWidget *>(m_bytes);
    m_normalPage = page;
    {
        const QSignalBlocker quiet(m_insideAction);
        m_insideAction->setChecked(kind == Kind::Bytes);
    }
    if (kind == Kind::Bytes) { // nothing else to show: what's inside it
        m_inside->load(show.path, show.type);
        page = m_inside;
    }
    m_pages->setCurrentWidget(page);
    for (QAction *a : {m_zoomOut, m_fit, m_actual, m_zoomIn})
        a->setVisible(kind == Kind::Picture || kind == Kind::Pdf);
}

QWidget *Window::insideView() const
{
    return m_inside;
}

void Window::setFacts(const QStringList &facts)
{
    m_shown.facts = facts;
    showFacts();
}

void Window::showFacts()
{
    QString html = QStringLiteral("<b>%1</b>").arg(m_shown.name.toHtmlEscaped());
    const QString quiet = palette().color(QPalette::PlaceholderText).name();
    if (!m_shown.facts.isEmpty()) {
        html += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"2\">");
        for (const QString &fact : std::as_const(m_shown.facts)) {
            const QString label = fact.section(QLatin1Char('\t'), 0, 0), value = fact.section(QLatin1Char('\t'), 1);
            if (label.isEmpty() && value.isEmpty())
                continue;
            html += QStringLiteral("<tr><td style=\"color:%1; padding-right:10px\">%2</td><td>%3</td></tr>")
                        .arg(quiet, label.toHtmlEscaped(), value.toHtmlEscaped());
        }
        html += QStringLiteral("</table>");
    }
    m_facts->setText(html);
}

void Window::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Left:
        if (m_previous->isEnabled() && m_shown.kind != Kind::Words && m_shown.kind != Kind::Bytes)
            Q_EMIT asked(Ask::Previous);
        return;
    case Qt::Key_Right:
        if (m_next->isEnabled() && m_shown.kind != Kind::Words && m_shown.kind != Kind::Bytes)
            Q_EMIT asked(Ask::Next);
        return;
    case Qt::Key_Space:
        if (m_shown.kind == Kind::Film || m_shown.kind == Kind::Song) {
            m_media->playPause();
            return;
        }
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        m_zoomIn->trigger();
        return;
    case Qt::Key_Minus:
        m_zoomOut->trigger();
        return;
    case Qt::Key_0:
        m_fit->trigger();
        return;
    case Qt::Key_F11:
        isFullScreen() ? showNormal() : showFullScreen();
        return;
    case Qt::Key_Escape:
        if (isFullScreen()) {
            showNormal();
            return;
        }
        close();
        return;
    default:
        break;
    }
    QMainWindow::keyPressEvent(event);
}

// --- The program

QStringList command(bool sandbox, const QStringList &readable)
{
    const QString app = QCoreApplication::applicationFilePath();
#ifdef Q_OS_LINUX
    const QString bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    if (sandbox && !bwrap.isEmpty() && !host::inFlatpak()) {
        QStringList c = {bwrap,
                         QStringLiteral("--ro-bind"), QStringLiteral("/"), QStringLiteral("/"),
                         QStringLiteral("--dev"), QStringLiteral("/dev"),
                         QStringLiteral("--proc"), QStringLiteral("/proc"),
                         QStringLiteral("--tmpfs"), QStringLiteral("/tmp"),
                         QStringLiteral("--tmpfs"), QStringLiteral("/run"),
                         QStringLiteral("--tmpfs"), QStringLiteral("/home"),
                         QStringLiteral("--tmpfs"), QStringLiteral("/root"),
                         QStringLiteral("--unshare-all"),
                         QStringLiteral("--die-with-parent"),
                         QStringLiteral("--new-session"),
                         QStringLiteral("--cap-drop"), QStringLiteral("ALL")};
        const auto bind = [&c](const char *how, const QString &path) {
            if (!path.isEmpty() && QFileInfo::exists(path))
                c << QString::fromLatin1(how) << path << path;
        };
        // What showing and playing needs: the screen (Wayland or X11), the sound (PipeWire or
        // PulseAudio) and the graphics card.
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
        if (!runtime.isEmpty()) {
            c << QStringLiteral("--perms") << QStringLiteral("0700") << QStringLiteral("--dir") << runtime;
            const QString wayland = qEnvironmentVariable("WAYLAND_DISPLAY");
            if (!wayland.isEmpty())
                bind("--bind", wayland.startsWith(QLatin1Char('/')) ? wayland : runtime + QLatin1Char('/') + wayland);
            bind("--bind", runtime + QStringLiteral("/pipewire-0"));
            bind("--bind", runtime + QStringLiteral("/pulse"));
        }
        bind("--bind", QStringLiteral("/tmp/.X11-unix"));
        bind("--ro-bind", qEnvironmentVariable("XAUTHORITY", QDir::homePath() + QStringLiteral("/.Xauthority")));
        bind("--dev-bind", QStringLiteral("/dev/dri"));
        for (const QString &nvidia : QDir(QStringLiteral("/dev")).entryList({QStringLiteral("nvidia*")}, QDir::System | QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot))
            bind("--dev-bind", QStringLiteral("/dev/") + nvidia);
        // The copies to show, and the program itself (it can be in the home folder, or an
        // AppImage mounted in /tmp).
        for (const QString &folder : readable)
            bind("--ro-bind", folder);
        bind("--ro-bind", qEnvironmentVariable("APPDIR", QFileInfo(app).absolutePath()));
        c << QStringLiteral("--") << app << QStringLiteral("--viewer");
        return c;
    }
#else
    Q_UNUSED(sandbox)
    Q_UNUSED(readable)
#endif
    return {app, QStringLiteral("--viewer")};
}

namespace {

// Standard input is read on a thread of its own: a pipe can't be watched from the event loop
// the same way on every system.
class Reader : public QObject
{
    Q_OBJECT
public:
    void run()
    {
        QFile in;
        if (!in.open(0, QIODevice::ReadOnly | QIODevice::Unbuffered)) {
            Q_EMIT closed();
            return;
        }
        // Exactly a header, then exactly its body: a read on a pipe that asks for more waits for
        // more, and the message would only come when the next one did.
        const auto exactly = [&in](qint64 n) {
            QByteArray out(n, Qt::Uninitialized);
            qint64 got = 0;
            while (got < n) {
                const qint64 r = in.read(out.data() + got, n - got);
                if (r <= 0)
                    return QByteArray();
                got += r;
            }
            return out;
        };
        for (;;) {
            QByteArray buffer = exactly(12);
            if (buffer.isEmpty())
                break;
            QDataStream header(buffer);
            quint32 magic = 0, type = 0, length = 0;
            header >> magic >> type >> length;
            if (magic != kMagic || length > kMostMessage)
                break;
            if (length > 0) {
                const QByteArray body = exactly(length);
                if (body.isEmpty())
                    break;
                buffer += body;
            }
            Message m;
            bool bad = false;
            if (!decode(buffer, m, &bad))
                break;
            Q_EMIT message(m);
        }
        Q_EMIT closed();
    }

Q_SIGNALS:
    void message(const viewer::Message &m);
    void closed();
};

} // namespace

int run()
{
#ifdef Q_OS_WIN
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
#endif
    qRegisterMetaType<viewer::Message>();
    QImageReader::setAllocationLimit(1024); // big photos, up to about 16000 by 16000 pixels
    QFile out;
    if (!out.open(1, QIODevice::WriteOnly | QIODevice::Unbuffered))
        return 1;
    Window window;
    QObject::connect(&window, &Window::asked, &window, [&out](Ask ask) {
        Message m;
        m.type = Message::AskFor;
        m.ask = ask;
        out.write(encode(m));
        out.flush();
    });
    QObject::connect(&window, &Window::saveEdited, &window, [&out](const QMap<quint64, quint8> &edits) {
        Message m;
        m.type = Message::Edited;
        m.edits = edits;
        out.write(encode(m));
        out.flush();
    });
    auto *reader = new Reader;
    QThread thread;
    reader->moveToThread(&thread);
    QObject::connect(&thread, &QThread::started, reader, &Reader::run);
    QObject::connect(reader, &Reader::closed, qApp, &QApplication::quit, Qt::QueuedConnection);
    QObject::connect(reader, &Reader::message, &window, [&window](const Message &m) {
        if (m.type == Message::Look) {
            if (!m.style.isEmpty())
                QApplication::setStyle(m.style);
            QApplication::setPalette(m.palette);
        } else if (m.type == Message::ShowFile) {
            window.present(m.show);
            window.raise();
            window.activateWindow();
            Q_EMIT window.asked(Ask::Shown);
        } else if (m.type == Message::Facts) {
            window.setFacts(m.show.facts);
        }
    }, Qt::QueuedConnection);
    // Most of the screen, the first time.
    if (const QScreen *screen = window.screen()) {
        const QRect room = screen->availableGeometry();
        window.resize(room.width() * 3 / 4, room.height() * 4 / 5);
        window.move(room.center() - window.rect().center());
    }
    window.show();
    thread.start();
    Q_EMIT window.asked(Ask::Ready);
    const int result = QApplication::exec();
    out.flush();
    // The reader is still waiting on standard input; there's nothing left to tidy.
    std::_Exit(result);
}

} // namespace viewer

#include "viewer.moc"
