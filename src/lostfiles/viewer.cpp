// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later
#include "viewer.h"

#include "enhance.h"
#include "jpegexact.h"
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
#include <QCheckBox>
#include <QComboBox>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMovie>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStringDecoder>
#include <QStyle>
#include <QThread>
#include <QThreadPool>
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
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QVideoFrame>
#include <QVideoSink>
#endif
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

#include <cmath>
#include <cstdlib>
#include <numbers>

Q_DECLARE_METATYPE(viewer::Message)

namespace viewer {
namespace {

constexpr quint32 kMagic = 0x44465657; // "DFVW"
constexpr quint32 kMostMessage = 4 * 1024 * 1024;
constexpr quint32 kMostPicture = 512u * 1024 * 1024;

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
        s << m.style << m.palette << m.iconTheme << m.fallbackIconTheme;
        break;
    case Message::ShowFile:
        s << qint32(m.show.kind) << qint32(m.show.type) << m.show.path << m.show.name << m.show.where;
        preview::writeStrings(s, m.show.facts);
        s << m.show.previous << m.show.next << m.show.openable << m.show.repairable << m.show.from;
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
    case Message::SavePicture:
        s << m.pictureName << m.picture;
        break;
    case Message::ExportFilm:
        s << m.speed << m.durationMs << qint32(m.settings.brightness) << qint32(m.settings.contrast) << qint32(m.settings.shadows)
          << qint32(m.settings.sharpen) << m.settings.grey << m.settings.invert << qint32(m.settings.smooth) << m.settings.deinterlace
          << m.settings.steady << m.settings.fullRange << qint32(m.settings.field);
        break;
    case Message::Profiles:
        s << quint32(m.profiles.size());
        for (const QByteArray &file : m.profiles)
            s << file;
        s << m.note;
        break;
    case Message::Profile:
        s << qint32(m.profileDo) << m.profileName << m.profile;
        preview::writeStrings(s, m.dropped);
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
    // A picture to save can be big (a photo made four times bigger); everything else is small.
    const quint32 most = type == Message::SavePicture ? kMostPicture : kMostMessage;
    if (magic != kMagic || length > most || type < Message::Look || type > Message::Profile) {
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
        s >> m.style >> m.palette >> m.iconTheme >> m.fallbackIconTheme;
        break;
    case Message::ShowFile: {
        qint32 kind = 0, fileType = -1;
        s >> kind >> fileType >> m.show.path >> m.show.name >> m.show.where;
        preview::readStrings(s, m.show.facts);
        s >> m.show.previous >> m.show.next >> m.show.openable >> m.show.repairable >> m.show.from;
        m.show.kind = kind >= qint32(Kind::Picture) && kind <= qint32(Kind::Bytes) ? Kind(kind) : Kind::Bytes;
        m.show.type = fileType;
        break;
    }
    case Message::AskFor: {
        qint32 ask = 0;
        s >> ask;
        m.ask = ask >= 0 && ask <= qint32(Ask::Map) ? Ask(ask) : Ask::Ready;
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
    case Message::SavePicture:
        s >> m.pictureName >> m.picture;
        break;
    case Message::ExportFilm: {
        qint32 brightness = 0, contrast = 0, shadows = 0, sharpen = 0, smooth = 0;
        qint32 field = 0;
        s >> m.speed >> m.durationMs >> brightness >> contrast >> shadows >> sharpen >> m.settings.grey >> m.settings.invert >> smooth
          >> m.settings.deinterlace >> m.settings.steady >> m.settings.fullRange >> field;
        m.settings.field = std::clamp(field, 0, 2);
        m.settings.smooth = std::clamp(smooth, 0, 100);
        // From another process: kept to what the panel can set.
        m.speed = std::isfinite(m.speed) ? std::clamp(m.speed, 0.1, 2.0) : 1.0;
        m.settings.brightness = std::clamp(brightness, -100, 100);
        m.settings.contrast = std::clamp(contrast, -100, 100);
        m.settings.shadows = std::clamp(shadows, 0, 100);
        m.settings.sharpen = std::clamp(sharpen, 0, 100);
        break;
    }
    case Message::Profiles: {
        quint32 count = 0;
        s >> count;
        for (quint32 i = 0; i < count && i < 1000 && s.status() == QDataStream::Ok; ++i) {
            QByteArray file;
            s >> file;
            m.profiles << file;
        }
        s >> m.note;
        break;
    }
    case Message::Profile: {
        qint32 what = 0;
        s >> what >> m.profileName >> m.profile;
        preview::readStrings(s, m.dropped);
        m.profileDo = what >= 0 && what <= qint32(ProfileDo::Dropped) ? ProfileDo(what) : ProfileDo::Save;
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
        setObjectName(QStringLiteral("picture"));
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
        m_processed = QImage();
        m_line = QLineF();
        m_box = QRectF();
        m_spot = false;
        viewport()->update();
    }

    // A picture made here (a film's frame) shown the same way as one loaded.
    void showImage(const QImage &image)
    {
        clear();
        m_image = image;
        fit();
    }
    // A spot of `whole`, filling the window: shown bigger smoothly, nothing made up.
    void showSpot(const QImage &whole, const QRect &spot)
    {
        clear();
        m_image = whole.copy(spot);
        m_spot = true;
        fit();
    }
    // Picking a spot: a box dragged around it, in the picture's own pixels.
    void setPicking(bool on)
    {
        m_picking = on;
        m_box = QRectF();
        viewport()->setCursor(on ? Qt::CrossCursor : Qt::OpenHandCursor);
        viewport()->update();
    }
    std::function<void(const QRect &spot)> picked;
    // Straightening: four corners clicked on the picture, clockwise from the top left.
    void setCornering(bool on, int wanted = 4)
    {
        m_cornering = on;
        m_cornersWanted = wanted;
        m_corners.clear();
        viewport()->setCursor(on ? Qt::CrossCursor : Qt::OpenHandCursor);
        viewport()->update();
    }
    std::function<void(const QPolygonF &corners)> cornered;

    // Enhancing: the picture as it was found, and what's shown instead of it.
    const QImage &original() const { return m_image; }
    bool animated() const { return m_movie != nullptr; }
    void showProcessed(const QImage &image)
    {
        m_processed = image;
        viewport()->update();
    }
    // Holding Compare: the picture as it was found, for a moment.
    void setComparing(bool on)
    {
        m_comparing = on;
        viewport()->update();
    }
    // Measuring a blur streak: a line drawn along it says how far and which way it moved.
    void setMeasuring(bool on)
    {
        m_measuring = on;
        m_line = QLineF();
        viewport()->setCursor(on ? Qt::CrossCursor : Qt::OpenHandCursor);
        viewport()->update();
    }
    std::function<void(int length, int angle)> measured;

    QSize pictureSize() const { return m_image.size(); }
    // Each pixel as a square (exactly what's there) instead of smoothly made bigger.
    void setShowPixels(bool on) { m_showPixels = on; viewport()->update(); }
    // What's blown out or pure black, shown over it.
    void setShowClipped(bool on) { m_showClipped = on; viewport()->update(); }
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
        const QImage &shown = !m_comparing && !m_processed.isNull() ? m_processed : m_image;
        const QRectF at = target();
        if (m_showPixels) {
            // Its own pixels, as squares: what's really there, exactly.
            p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 1);
            p.drawImage(at, shown);
        } else if (m_zoom > 1.01 && at.width() * at.height() <= 6e6) {
            // Bigger than it is: made bigger properly (Lanczos), not as squares, once for this size.
            const QSize size = at.size().toSize();
            if (m_smoothFor != shown.cacheKey() || m_smooth.size() != size) {
                m_smooth = enhance::resize(shown, size);
                m_smoothFor = shown.cacheKey();
            }
            p.drawImage(at.topLeft(), m_smooth);
        } else {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(at, shown);
        }
        if (m_showClipped) {
            if (m_clippedFor != shown.cacheKey()) {
                m_clipped = enhance::clippedOverlay(shown);
                m_clippedFor = shown.cacheKey();
            }
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(at, m_clipped);
        }
        if (m_measuring && !m_line.isNull()) {
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(palette().color(QPalette::Highlight), 2));
            p.drawLine(m_line);
        }
        if (m_cornering && !m_corners.isEmpty()) {
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(palette().color(QPalette::Highlight), 2));
            QPolygonF shown;
            const QRectF at = target();
            for (const QPointF &c : std::as_const(m_corners))
                shown << at.topLeft() + c * m_zoom;
            p.drawPolyline(shown);
            for (const QPointF &c : std::as_const(shown))
                p.drawEllipse(c, 4, 4);
        }
        if (m_picking && !m_box.isNull()) {
            // The rest dimmed, the spot outlined.
            QPainterPath outside;
            outside.addRect(QRectF(viewport()->rect()));
            QPainterPath inside;
            inside.addRect(m_box);
            p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 120));
            p.setPen(QPen(palette().color(QPalette::Highlight), 2, Qt::DashLine));
            p.drawRect(m_box);
        }
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
        if (event->button() == Qt::LeftButton && m_measuring) {
            m_line = QLineF(event->position(), event->position());
            return;
        }
        if (event->button() == Qt::LeftButton && m_cornering) {
            const QPointF in = (event->position() - target().topLeft()) / m_zoom;
            m_corners << QPointF(std::clamp(in.x(), 0.0, double(m_image.width())), std::clamp(in.y(), 0.0, double(m_image.height())));
            viewport()->update();
            if (m_corners.size() == m_cornersWanted) {
                const QPolygonF corners = m_corners;
                setCornering(false);
                if (cornered)
                    cornered(corners);
            }
            return;
        }
        if (event->button() == Qt::LeftButton && m_picking) {
            m_boxFrom = event->position();
            m_box = QRectF(m_boxFrom, m_boxFrom);
            return;
        }
        if (event->button() == Qt::LeftButton) {
            m_drag = event->position().toPoint();
            viewport()->setCursor(Qt::ClosedHandCursor);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton))
            return;
        if (m_measuring) {
            m_line.setP2(event->position());
            viewport()->update();
            return;
        }
        if (m_picking) {
            m_box = QRectF(m_boxFrom, event->position()).normalized();
            viewport()->update();
            return;
        }
        const QPoint by = event->position().toPoint() - m_drag;
        m_drag = event->position().toPoint();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - by.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - by.y());
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (m_measuring) {
            // In the picture's own pixels: how long the streak is, and its direction (0° across,
            // 90° up and down).
            const double length = m_line.length() / m_zoom;
            double angle = std::atan2(-(m_line.dy()), m_line.dx()) * 180 / std::numbers::pi;
            if (angle < 0)
                angle += 180;
            if (angle >= 180)
                angle -= 180;
            if (length >= 2 && measured)
                measured(int(std::lround(length)), int(std::lround(angle)) % 180);
            return;
        }
        if (m_picking) {
            // In the picture's own pixels, kept inside it; a box too small to see is no spot.
            const QRectF at = target();
            const QRectF in((m_box.topLeft() - at.topLeft()) / m_zoom, m_box.size() / m_zoom);
            const QRect spot = in.toAlignedRect().intersected(m_image.rect());
            m_box = QRectF();
            viewport()->update();
            if (spot.width() >= 4 && spot.height() >= 4 && picked)
                picked(spot);
            return;
        }
        viewport()->setCursor(Qt::OpenHandCursor);
    }

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
        // A spot fills the window, up to 16 times bigger; a whole picture is never shown bigger than it is.
        return std::min({m_spot ? 16.0 : 1.0, room.width() / double(m_image.width()), room.height() / double(m_image.height())});
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
    QImage m_processed;
    QMovie *m_movie = nullptr;
    double m_zoom = 1;
    bool m_fit = true;
    bool m_comparing = false;
    bool m_measuring = false;
    QLineF m_line;
    bool m_picking = false;
    bool m_showPixels = false;
    bool m_showClipped = false;
    QImage m_clipped;          // the overlay, for the picture it was made from
    qint64 m_clippedFor = 0;
    QImage m_smooth;          // the picture made bigger for the screen, at the size shown
    qint64 m_smoothFor = 0;   // and which picture it was made from
    bool m_cornering = false;
    int m_cornersWanted = 4;
    QPolygonF m_corners;
    bool m_spot = false;
    QPointF m_boxFrom;
    QRectF m_box;
    QPoint m_drag;
};

// --- Enhance: the image tools (enhance.h), for a picture or the frame of a film on the screen.

class EnhancePanel : public QWidget
{
public:
    explicit EnhancePanel(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("enhance"));
        auto *layout = new QVBoxLayout(this);
        auto *note = new QLabel(viewer::tr("These bring out what's in the picture. They can't add detail the camera never caught, and they "
                                           "never make any up. The file found is never changed: what's saved is a new picture."));
        note->setWordWrap(true);
        layout->addWidget(note);
        // Profiles: settings kept under a name. DiskAnvil's own, then the person's.
        auto *profileRow = new QHBoxLayout;
        m_profiles = new QComboBox;
        m_profiles->setObjectName(QStringLiteral("profiles"));
        m_profiles->setToolTip(viewer::tr("Settings kept under a name: DiskAnvil's own, and ones you saved or were given. "
                                          "Drop a profile file (.%1) on this window to add it.").arg(QLatin1String(enhance::kProfileSuffix)));
        auto *more = new QToolButton;
        more->setText(viewer::tr("Profiles"));
        more->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(more);
        m_saveProfile = menu->addAction(viewer::tr("Save as a Profile…"));
        m_removeProfile = menu->addAction(viewer::tr("Remove This Profile"));
        menu->addSeparator();
        m_importProfile = menu->addAction(viewer::tr("Add Profiles From Files…"));
        m_exportProfile = menu->addAction(viewer::tr("Save This Profile as a File…"));
        more->setMenu(menu);
        profileRow->addWidget(m_profiles, 1);
        profileRow->addWidget(more);
        layout->addLayout(profileRow);
        m_profileNote = new QLabel;
        m_profileNote->setWordWrap(true);
        m_profileNote->setObjectName(QStringLiteral("profileNote"));
        layout->addWidget(m_profileNote);
        QObject::connect(m_profiles, &QComboBox::activated, this, [this](int row) { useProfile(row); });
        QObject::connect(m_saveProfile, &QAction::triggered, this, [this] { saveProfileAs(); });
        QObject::connect(m_removeProfile, &QAction::triggered, this, [this] { removeChosen(); });
        QObject::connect(m_importProfile, &QAction::triggered, this, [this] { if (profileAsked) profileAsked(viewer::ProfileDo::Import, {}, {}); });
        QObject::connect(m_exportProfile, &QAction::triggered, this, [this] { exportChosen(); });
        auto *form = new QFormLayout;
        const auto slider = [this, form](const QString &label, int low, int high, const QString &name) {
            auto *s = new QSlider(Qt::Horizontal);
            s->setRange(low, high);
            s->setObjectName(name);
            auto *value = new QLabel(QStringLiteral("0"));
            value->setMinimumWidth(32);
            auto *row = new QHBoxLayout;
            row->addWidget(s, 1);
            row->addWidget(value);
            form->addRow(label, row);
            QObject::connect(s, &QSlider::valueChanged, this, [this, value](int v) {
                value->setText(QString::number(v));
                touched();
            });
            m_rows << s;
            return s;
        };
        m_brightness = slider(viewer::tr("Lighter"), -100, 100, QStringLiteral("brightness"));
        m_contrast = slider(viewer::tr("Contrast"), -100, 100, QStringLiteral("contrast"));
        m_shadows = slider(viewer::tr("Dark parts"), 0, 100, QStringLiteral("shadows"));
        m_local = slider(viewer::tr("Local contrast"), 0, 100, QStringLiteral("localContrast"));
        m_local->setToolTip(viewer::tr("Detail brought out in the dark and the light parts each on its own (CLAHE), as forensic video tools do"));
        m_sharpen = slider(viewer::tr("Sharpen"), 0, 100, QStringLiteral("sharpen"));
        m_smooth = slider(viewer::tr("Smooth grain"), 0, 100, QStringLiteral("smooth"));
        m_deblock = slider(viewer::tr("JPEG squares"), 0, 100, QStringLiteral("deblock"));
        m_nlm = slider(viewer::tr("Grain (non-local)"), 0, 100, QStringLiteral("nlm"));
        m_nlm->setToolTip(viewer::tr("Grain taken out by non-local means: each point from the patches that look like it, so edges and "
                                     "texture stay (the best of the free ways; slow on a big picture)"));
        m_homomorphic = slider(viewer::tr("Uneven light"), 0, 100, QStringLiteral("homomorphic"));
        m_homomorphic->setToolTip(viewer::tr("The light falling on things turned down and what's lit turned up: a backlit face in a doorway, "
                                             "a street with one bright lamp (a homomorphic filter)"));
        m_retinex = slider(viewer::tr("Retinex"), 0, 100, QStringLiteral("retinex"));
        m_retinex->setToolTip(viewer::tr("Dark parts brought up the way an eye adjusts to them, against the light around them at three "
                                         "distances (multi-scale Retinex)"));
        m_deblock->setToolTip(viewer::tr("The 8 by 8 squares a hard-squeezed JPEG (a thumbnail, a picture sent by a phone) shows, evened out"));
        m_smooth->setToolTip(viewer::tr("Grain evened out while edges stay sharp (a bilateral filter)"));
        m_focus = slider(viewer::tr("Out of focus"), 0, 15, QStringLiteral("focus"));
        m_motion = slider(viewer::tr("Moved"), 0, 60, QStringLiteral("motion"));
        m_angle = slider(viewer::tr("Which way"), 0, 179, QStringLiteral("angle"));
        m_focus->setToolTip(viewer::tr("How blurred it is, in pixels: the blur is taken back out (slow on a big picture)"));
        m_motion->setToolTip(viewer::tr("How far it moved while it was taken, in pixels (or draw along a streak)"));
        m_angle->setToolTip(viewer::tr("Which way it moved: 0 across, 90 up and down"));
        m_fisheye = slider(viewer::tr("Wide lens bend"), 0, 100, QStringLiteral("fisheye"));
        m_fisheye->setToolTip(viewer::tr("A doorbell's or a wide camera's bend taken out, so straight things are straight again "
                                         "(or use Fit to a Straight Line)"));
        m_noise = slider(viewer::tr("Grain allowance"), 1, 100, QStringLiteral("noise"));
        m_noise->setToolTip(viewer::tr("For Wiener: how much grain to allow for. Low is sharpest but grainy; raise it if the picture fills with speckle or rings"));
        m_noise->setValue(30);
        m_blurShape = new QComboBox;
        m_blurShape->setObjectName(QStringLiteral("blurShape"));
        m_blurShape->addItems({viewer::tr("Out of focus: soft (Gaussian)"), viewer::tr("Out of focus: a lens's disc")});
        m_blurShape->setToolTip(viewer::tr("A camera's lens out of focus turns each point into a round spot (a disc): the truest shape for it"));
        m_deblurWay = new QComboBox;
        m_deblurWay->setObjectName(QStringLiteral("deblurWay"));
        m_deblurWay->addItems({viewer::tr("Taken out step by step (Richardson-Lucy)"), viewer::tr("Taken out in one go (Wiener)")});
        m_deblurWay->setToolTip(viewer::tr("Richardson-Lucy is careful and keeps grain down; Wiener handles big blurs best (set its grain allowance)"));
        form->addRow(QString(), m_blurShape);
        form->addRow(QString(), m_deblurWay);
        layout->addLayout(form);
        m_measure = new QPushButton(viewer::tr("Draw Along a Streak"));
        m_measure->setCheckable(true);
        m_measure->setToolTip(viewer::tr("Draw a line along a blur streak in the picture: how long it is and which way it goes set Moved and Which way"));
        layout->addWidget(m_measure);
        m_grain = new QComboBox;
        m_grain->addItems({viewer::tr("Grain left as it is"), viewer::tr("A little grain smoothed"), viewer::tr("More grain smoothed")});
        m_levels = new QCheckBox(viewer::tr("Stretch levels (darkest black, lightest white)"));
        m_grey = new QCheckBox(viewer::tr("Black and white"));
        m_invert = new QCheckBox(viewer::tr("Inverted (for a negative)"));
        m_deinterlace = new QCheckBox(viewer::tr("Deinterlace (comb lines from an older camera)"));
        m_deinterlace->setObjectName(QStringLiteral("deinterlace"));
        m_redecode = new QCheckBox(viewer::tr("Decode the JPEG again"));
        m_redecode->setObjectName(QStringLiteral("redecode"));
        m_redecode->setToolTip(viewer::tr("A JPEG keeps each 8 by 8 square as rounded numbers. Decoded again, it's the smoothest picture "
                                          "whose numbers round to exactly what the file has: the squares go, and nothing the file doesn't allow is made up"));
        m_redecode->hide();
        m_fullRange = new QCheckBox(viewer::tr("Video levels to full (washed-out CCTV)"));
        m_fullRange->setObjectName(QStringLiteral("fullRange"));
        m_fullRange->setToolTip(viewer::tr("Video often keeps black at 16 and white at 235: stretched to 0 and 255, the grey haze goes"));
        m_field = new QComboBox;
        m_field->setObjectName(QStringLiteral("field"));
        m_field->addItems({viewer::tr("Both half-pictures (fields)"), viewer::tr("Only the first field, lines doubled"), viewer::tr("Only the second field, lines doubled")});
        m_field->setToolTip(viewer::tr("An interlaced frame is two half-pictures taken a moment apart: one on its own has no comb lines"));
        m_channel = new QComboBox;
        m_channel->setObjectName(QStringLiteral("channel"));
        m_channel->addItems({viewer::tr("All the colour"), viewer::tr("Only red"), viewer::tr("Only green"), viewer::tr("Only blue"), viewer::tr("Only brightness (Y)"),
                             viewer::tr("Only blue difference (Cb)"), viewer::tr("Only red difference (Cr)"), viewer::tr("Only hue"), viewer::tr("Only saturation"),
                             viewer::tr("Only value (V)"), viewer::tr("Most contrast (from the picture)")});
        m_channel->setToolTip(viewer::tr("One part of the colour on its own, as grey: writing or a plate often stands out in one"));
        m_clipped = new QCheckBox(viewer::tr("Show what's blown out (red) or black (blue)"));
        m_clipped->setObjectName(QStringLiteral("clipped"));
        m_clipped->setToolTip(viewer::tr("Where the picture is pure white or pure black nothing is left to bring out: shown over it, not saved"));
        m_steady = new QCheckBox(viewer::tr("Steady it (in the copy saved)"));
        m_steady->setObjectName(QStringLiteral("steady"));
        m_steady->setToolTip(viewer::tr("A shaky film is steadied in the slowed-down copy (ffmpeg's deshake)"));
        m_steady->hide();
        for (QWidget *w : {static_cast<QWidget *>(m_grain), static_cast<QWidget *>(m_levels), static_cast<QWidget *>(m_grey), static_cast<QWidget *>(m_invert),
                           static_cast<QWidget *>(m_redecode), static_cast<QWidget *>(m_deinterlace), static_cast<QWidget *>(m_fullRange), static_cast<QWidget *>(m_field),
                           static_cast<QWidget *>(m_channel), static_cast<QWidget *>(m_clipped), static_cast<QWidget *>(m_steady)})
            layout->addWidget(w);
        QObject::connect(m_grain, &QComboBox::currentIndexChanged, this, [this] { touched(); });
        for (QCheckBox *box : {m_levels, m_grey, m_invert, m_deinterlace, m_steady, m_fullRange})
            QObject::connect(box, &QCheckBox::toggled, this, [this] { touched(); });
        for (QComboBox *box : {m_field, m_channel, m_blurShape, m_deblurWay})
            QObject::connect(box, &QComboBox::currentIndexChanged, this, [this] { touched(); });
        QObject::connect(m_clipped, &QCheckBox::toggled, this, [this](bool on) { if (showClipped) showClipped(on); });
        QObject::connect(m_redecode, &QCheckBox::toggled, this, [this](bool on) { if (redecode) redecode(on); });
        // A spot: a box drawn around part of the picture (a face, a number plate), which is then
        // shown filling the window and is what the tools work on and what's saved.
        auto *spotRow = new QHBoxLayout;
        m_pick = new QPushButton(viewer::tr("Pick a Spot"));
        m_pick->setObjectName(QStringLiteral("pickSpot"));
        m_pick->setCheckable(true);
        m_pick->setToolTip(viewer::tr("Draw a box around part of the picture, or of the film where it's paused: just that part is "
                                      "shown, bigger, to work on and save"));
        m_whole = new QPushButton(viewer::tr("Whole Picture"));
        m_whole->setObjectName(QStringLiteral("wholePicture"));
        m_whole->setEnabled(false);
        spotRow->addWidget(m_pick);
        spotRow->addWidget(m_whole);
        layout->addLayout(spotRow);
        m_straighten = new QPushButton(viewer::tr("Straighten"));
        m_straighten->setObjectName(QStringLiteral("straighten"));
        m_straighten->setCheckable(true);
        m_straighten->setToolTip(viewer::tr("Click the four corners of something seen at an angle (a number plate, a sign, a page), clockwise from "
                                            "its top left: it's made square-on"));
        layout->addWidget(m_straighten);
        auto *geometryRow = new QHBoxLayout;
        m_fitLine = new QPushButton(viewer::tr("Fit to a Straight Line"));
        m_fitLine->setObjectName(QStringLiteral("fitLine"));
        m_fitLine->setCheckable(true);
        m_fitLine->setToolTip(viewer::tr("Click three points along something that should be straight (a door frame, a kerb): the lens's "
                                         "bend is worked out from how far they're off a line"));
        m_shape = new QComboBox;
        m_shape->setObjectName(QStringLiteral("shape"));
        m_shape->addItems({viewer::tr("Shape as it's stored"), viewer::tr("Made 4:3"), viewer::tr("Made 16:9"), viewer::tr("Twice as tall"), viewer::tr("Twice as wide")});
        m_shape->setToolTip(viewer::tr("A camera that stores its picture squashed (people too thin or too wide, half-height fields): "
                                       "its proper shape"));
        geometryRow->addWidget(m_fitLine);
        geometryRow->addWidget(m_shape, 1);
        layout->addLayout(geometryRow);
        // Stacking: the frames around a paused one, lined up on the spot and put together.
        m_stackBox = new QWidget;
        auto *stackRow = new QHBoxLayout(m_stackBox);
        stackRow->setContentsMargins(0, 0, 0, 0);
        m_stackCount = new QSpinBox;
        m_stackCount->setObjectName(QStringLiteral("stackCount"));
        m_stackCount->setRange(3, 60);
        m_stackCount->setValue(15);
        m_stackCount->setSuffix(viewer::tr(" frames"));
        m_stackDouble = new QCheckBox(viewer::tr("Twice the size"));
        m_stackDouble->setObjectName(QStringLiteral("stackDouble"));
        m_stackDouble->setToolTip(viewer::tr("Made twice as big from what the frames hold between them (shift and add), not from one frame"));
        m_stack = new QPushButton(viewer::tr("Stack Frames"));
        m_stack->setObjectName(QStringLiteral("stackFrames"));
        m_stack->setToolTip(viewer::tr("The frames around this one, lined up on the spot and put together: the grain evens out, and what "
                                       "keeps still (a number plate, a face) shows more clearly than in any one frame"));
        m_stackHow = new QComboBox;
        m_stackHow->setObjectName(QStringLiteral("stackHow"));
        m_stackHow->addItems({viewer::tr("Average"), viewer::tr("Middle value"), viewer::tr("Average without outliers")});
        m_stackHow->setToolTip(viewer::tr("Average: the most grain taken out. Middle value, or the average without outliers: something "
                                          "in only a few of the frames (a passer-by, a car) is left out instead of smeared in"));
        stackRow->addWidget(m_stackCount);
        stackRow->addWidget(m_stackHow);
        m_sharpest = new QCheckBox(viewer::tr("Sharpest"));
        m_sharpest->setObjectName(QStringLiteral("sharpest"));
        m_sharpest->setToolTip(viewer::tr("Only the sharpest frames of those collected (blurred or badly squeezed ones left out)"));
        stackRow->addWidget(m_sharpest);
        stackRow->addWidget(m_stackDouble);
        stackRow->addWidget(m_stack, 1);
        layout->addWidget(m_stackBox);
        m_stackBox->hide();
        QObject::connect(m_pick, &QPushButton::toggled, this, [this](bool on) { if (pick) pick(on); });
        QObject::connect(m_whole, &QPushButton::clicked, this, [this] { if (whole) whole(); });
        QObject::connect(m_straighten, &QPushButton::toggled, this, [this](bool on) { if (straightenAsked) straightenAsked(on); });
        QObject::connect(m_stack, &QPushButton::clicked, this, [this] { if (stackAsked) stackAsked(m_stackCount->value(), m_stackDouble->isChecked() ? 2 : 1, m_stackHow->currentIndex() + (m_sharpest->isChecked() ? 10 : 0)); });
        QObject::connect(m_fitLine, &QPushButton::toggled, this, [this](bool on) { if (fitLine) fitLine(on); });
        QObject::connect(m_shape, &QComboBox::activated, this, [this](int index) { if (shape) shape(index); });
        auto *buttons = new QHBoxLayout;
        m_compare = new QPushButton(viewer::tr("Hold to Compare"));
        m_compare->setToolTip(viewer::tr("Shows the picture as it was found while it's held down"));
        auto *reset = new QPushButton(viewer::tr("Reset"));
        buttons->addWidget(m_compare);
        buttons->addWidget(reset);
        layout->addLayout(buttons);
        m_status = new QLabel;
        m_status->setWordWrap(true);
        layout->addWidget(m_status);
        m_bigger = new QComboBox;
        for (const auto &[factor, words] : {std::pair{1, viewer::tr("Its own size")}, std::pair{2, viewer::tr("Twice as big")}, std::pair{3, viewer::tr("Three times as big")},
                                            std::pair{4, viewer::tr("Four times as big")}, std::pair{6, viewer::tr("Six times as big")}, std::pair{8, viewer::tr("Eight times as big")}})
            m_bigger->addItem(words, factor);
        m_bigger->setToolTip(viewer::tr("Bigger, smoothly: the same picture larger, nothing made up"));
        m_save = new QPushButton(viewer::tr("Save as a New Picture…"));
        m_save->setObjectName(QStringLiteral("saveEnhanced"));
        m_export = new QPushButton(viewer::tr("Save a Slowed-Down Copy…"));
        m_export->setObjectName(QStringLiteral("exportFilm"));
        m_export->setToolTip(viewer::tr("The film at the speed chosen below it, with what's set here, saved as a new MP4 (needs ffmpeg)"));
        m_export->hide();
        layout->addWidget(m_bigger);
        layout->addWidget(m_save);
        layout->addWidget(m_export);
        layout->addStretch();
        QObject::connect(m_compare, &QPushButton::pressed, this, [this] { if (compare) compare(true); });
        QObject::connect(m_compare, &QPushButton::released, this, [this] { if (compare) compare(false); });
        QObject::connect(reset, &QPushButton::clicked, this, [this] { this->reset(); if (changed) changed(); });
        QObject::connect(m_measure, &QPushButton::toggled, this, [this](bool on) { if (measure) measure(on); });
        QObject::connect(m_save, &QPushButton::clicked, this, [this] { if (save) save(m_bigger->currentData().toInt()); });
        QObject::connect(m_export, &QPushButton::clicked, this, [this] { if (exportFilm) exportFilm(); });
        fillProfiles({});
    }

    enhance::Settings settings() const
    {
        enhance::Settings s;
        s.brightness = m_brightness->value();
        s.contrast = m_contrast->value();
        s.shadows = m_shadows->value();
        s.sharpen = m_sharpen->value();
        s.focus = m_focus->isEnabled() ? m_focus->value() : 0;
        s.motion = m_motion->isEnabled() ? m_motion->value() : 0;
        s.motionAngle = m_angle->value();
        s.denoise = m_grain->isEnabled() ? m_grain->currentIndex() : 0;
        s.smooth = m_smooth->isEnabled() ? m_smooth->value() : 0;
        s.deblock = m_deblock->value();
        s.fisheye = m_fisheye->value();
        s.nlm = m_nlm->isEnabled() ? m_nlm->value() : 0;
        s.noise = m_noise->value();
        s.focusDisc = m_blurShape->currentIndex() == 1;
        s.wiener = m_deblurWay->currentIndex() == 1;
        s.homomorphic = m_homomorphic->value();
        s.retinex = m_retinex->value();
        s.fullRange = m_fullRange->isChecked();
        s.field = m_field->currentIndex();
        s.channel = m_channel->currentIndex();
        s.localContrast = m_local->value();
        s.deinterlace = m_deinterlace->isChecked();
        s.steady = m_steady->isVisible() && m_steady->isChecked();
        s.autoLevels = m_levels->isChecked();
        s.grey = m_grey->isChecked();
        s.invert = m_invert->isChecked();
        return s;
    }

    void setSettings(const enhance::Settings &s)
    {
        m_quiet = true;
        m_brightness->setValue(s.brightness);
        m_contrast->setValue(s.contrast);
        m_shadows->setValue(s.shadows);
        m_sharpen->setValue(s.sharpen);
        m_focus->setValue(s.focus);
        m_motion->setValue(s.motion);
        m_angle->setValue(s.motionAngle);
        m_grain->setCurrentIndex(std::clamp(s.denoise, 0, 2));
        m_levels->setChecked(s.autoLevels);
        m_grey->setChecked(s.grey);
        m_invert->setChecked(s.invert);
        m_smooth->setValue(s.smooth);
        m_deblock->setValue(s.deblock);
        m_fisheye->setValue(s.fisheye);
        m_nlm->setValue(s.nlm);
        m_noise->setValue(std::clamp(s.noise, 1, 100));
        m_blurShape->setCurrentIndex(s.focusDisc ? 1 : 0);
        m_deblurWay->setCurrentIndex(s.wiener ? 1 : 0);
        m_homomorphic->setValue(s.homomorphic);
        m_retinex->setValue(s.retinex);
        m_fullRange->setChecked(s.fullRange);
        m_field->setCurrentIndex(std::clamp(s.field, 0, 2));
        m_channel->setCurrentIndex(std::clamp(s.channel, 0, int(enhance::Settings::MostContrast)));
        m_local->setValue(s.localContrast);
        m_deinterlace->setChecked(s.deinterlace);
        m_steady->setChecked(s.steady);
        m_quiet = false;
        if (changed)
            changed();
    }

    // The saved profiles, from Find Lost Files (as their files), and what was just done.
    void setSaved(const QList<QByteArray> &files, const QString &note)
    {
        const QString chosen = m_profiles->currentIndex() > 0 ? m_profiles->currentText() : QString();
        m_saved.clear();
        for (const QByteArray &file : files) {
            enhance::Profile p;
            QString why;
            if (enhance::profileFromFile(file, &p, &why))
                m_saved << p;
        }
        fillProfiles(chosen);
        m_profileNote->setText(note);
    }
    QStringList profileNames() const // the tests
    {
        QStringList out;
        for (int i = 1; i < m_profiles->count(); ++i)
            if (!m_profiles->itemText(i).isEmpty())
                out << m_profiles->itemText(i);
        return out;
    }
    // Chooses one by name, as picking it in the list does (the tests).
    bool chooseProfile(const QString &name)
    {
        const int row = m_profiles->findText(name);
        if (row <= 0)
            return false;
        m_profiles->setCurrentIndex(row);
        useProfile(row);
        return true;
    }

    // Whether a spot is shown (Whole Picture goes back), and whether picking is on.
    void setSpotShown(bool shown)
    {
        m_whole->setEnabled(shown);
        const QSignalBlocker quiet(m_pick);
        m_pick->setChecked(false);
        const QSignalBlocker quietToo(m_straighten);
        m_straighten->setChecked(false);
    }
    // Stacking is for a spot in a paused film's frame.
    void setStackable(bool can) { m_stackBox->setVisible(can); }
    void setStacking(bool busy) { m_stack->setEnabled(!busy); }

    void reset()
    {
        m_quiet = true;
        m_profiles->setCurrentIndex(0);
        for (QSlider *s : std::as_const(m_rows))
            s->setValue(0);
        m_grain->setCurrentIndex(0);
        for (QCheckBox *box : {m_levels, m_grey, m_invert, m_deinterlace, m_steady, m_fullRange, m_clipped})
            box->setChecked(false);
        m_field->setCurrentIndex(0);
        m_channel->setCurrentIndex(0);
        m_blurShape->setCurrentIndex(0);
        m_deblurWay->setCurrentIndex(0);
        m_noise->setValue(30);
        m_bigger->setCurrentIndex(0);
        m_measure->setChecked(false);
        m_quiet = false;
        setStatus(QString());
        setSpotShown(false);
    }

    // Bigger when saved: a spot is usually wanted bigger.
    void setBigger(int factor)
    {
        int at = 0;
        for (int i = 0; i < m_bigger->count(); ++i)
            if (m_bigger->itemData(i).toInt() <= factor)
                at = i;
        m_bigger->setCurrentIndex(at);
    }

    void setMotion(int length, int angle)
    {
        m_quiet = true;
        m_motion->setValue(std::clamp(length, 2, 60));
        m_quiet = false;
        m_angle->setValue(angle); // the last one asks for the new picture
        m_measure->setChecked(false);
    }

    // A film's frame: the slow ones (blur taken out, grain) and enlarging are for still pictures.
    void setForFilm(bool film)
    {
        for (QWidget *w : {static_cast<QWidget *>(m_focus), static_cast<QWidget *>(m_motion), static_cast<QWidget *>(m_angle),
                           static_cast<QWidget *>(m_grain), static_cast<QWidget *>(m_measure), static_cast<QWidget *>(m_bigger),
                           static_cast<QWidget *>(m_smooth), static_cast<QWidget *>(m_straighten), static_cast<QWidget *>(m_nlm),
                           static_cast<QWidget *>(m_noise), static_cast<QWidget *>(m_blurShape), static_cast<QWidget *>(m_deblurWay)})
            w->setEnabled(!film);
        m_steady->setVisible(film);
        m_save->setText(film ? viewer::tr("Save This Frame…") : viewer::tr("Save as a New Picture…"));
        m_export->setVisible(film);
        m_pick->setText(film ? viewer::tr("Pick a Spot in This Frame") : viewer::tr("Pick a Spot"));
        m_whole->setText(film ? viewer::tr("Back to the Film") : viewer::tr("Whole Picture"));
    }

    void setStatus(const QString &text) { m_status->setText(text); }

    std::function<void()> changed;
    std::function<void(int factor)> save;
    std::function<void(bool on)> compare;
    std::function<void(bool on)> measure;
    std::function<void()> exportFilm;
    std::function<void(bool on)> pick;
    std::function<void()> whole;
    std::function<void(bool on)> straightenAsked;
    std::function<void(int frames, int scale, int how)> stackAsked;
    std::function<void(bool on)> showClipped;
    std::function<void(bool on)> redecode;
    std::function<void(bool on)> fitLine;
    std::function<void(int index)> shape;
    void setFisheye(int value)
    {
        const QSignalBlocker quiet(m_fitLine);
        m_fitLine->setChecked(false);
        m_fisheye->setValue(value);
    }
    void setStackLabel(const QString &text) { m_stack->setText(text); }
    void resetShape() { m_shape->setCurrentIndex(0); }
    // Offered for a JPEG (a picture as found, not a film's frame); unticked when another is shown.
    void setRedecodable(bool can)
    {
        const QSignalBlocker quiet(m_redecode);
        m_redecode->setChecked(false);
        m_redecode->setVisible(can && jpegexact::available());
    }
    void setRedecodeBusy(bool busy) { m_redecode->setEnabled(!busy); }
    std::function<void(viewer::ProfileDo what, const QString &name, const QByteArray &file)> profileAsked;

private:
    // A slider or box changed by hand: the settings are no longer the profile's.
    void touched()
    {
        if (m_quiet)
            return;
        m_profiles->setCurrentIndex(0);
        updateProfileActions();
        if (changed)
            changed();
    }

    // The list: "your own settings", DiskAnvil's, then the saved ones.
    void fillProfiles(const QString &chosen)
    {
        m_builtIn = enhance::builtInProfiles();
        const QSignalBlocker quiet(m_profiles);
        m_profiles->clear();
        m_profiles->addItem(viewer::tr("Your own settings"));
        for (int i = 0; i < m_builtIn.size(); ++i) {
            m_profiles->addItem(m_builtIn[i].name, i);
            m_profiles->setItemData(m_profiles->count() - 1, m_builtIn[i].description, Qt::ToolTipRole);
        }
        if (!m_saved.isEmpty())
            m_profiles->insertSeparator(m_profiles->count());
        for (int i = 0; i < m_saved.size(); ++i) {
            m_profiles->addItem(m_saved[i].name, 1000 + i);
            m_profiles->setItemData(m_profiles->count() - 1, m_saved[i].description.isEmpty() ? viewer::tr("Yours") : m_saved[i].description,
                                    Qt::ToolTipRole);
        }
        const int at = chosen.isEmpty() ? 0 : m_profiles->findText(chosen);
        m_profiles->setCurrentIndex(std::max(0, at));
        updateProfileActions();
    }

    const enhance::Profile *chosenProfile() const
    {
        const QVariant id = m_profiles->currentData();
        if (!id.isValid())
            return nullptr;
        const int i = id.toInt();
        if (i >= 1000)
            return i - 1000 < m_saved.size() ? &m_saved[i - 1000] : nullptr;
        return i < m_builtIn.size() ? &m_builtIn[i] : nullptr;
    }

    void useProfile(int row)
    {
        Q_UNUSED(row)
        if (const enhance::Profile *p = chosenProfile()) {
            setSettings(p->settings);
            m_profileNote->setText(p->description);
        } else {
            m_profileNote->clear();
        }
        updateProfileActions();
    }

    void updateProfileActions()
    {
        const enhance::Profile *p = chosenProfile();
        m_removeProfile->setEnabled(p && !p->builtIn);
        m_exportProfile->setEnabled(true);
    }

    void saveProfileAs()
    {
        const enhance::Profile *p = chosenProfile();
        bool ok = false;
        const QString name = QInputDialog::getText(this, viewer::tr("Save as a Profile"), viewer::tr("Its name:"), QLineEdit::Normal,
                                                   p && !p->builtIn ? p->name : QString(), &ok)
                                 .simplified();
        if (!ok || name.isEmpty())
            return;
        enhance::Profile made;
        made.name = name.left(60);
        made.settings = settings();
        made.description = made.settings.describe().join(QStringLiteral(", "));
        if (profileAsked)
            profileAsked(viewer::ProfileDo::Save, made.name, enhance::profileToFile(made));
    }

    void removeChosen()
    {
        const enhance::Profile *p = chosenProfile();
        if (!p || p->builtIn)
            return;
        if (QMessageBox::question(this, viewer::tr("Remove This Profile"), viewer::tr("Remove the profile \"%1\"?").arg(p->name)) != QMessageBox::Yes)
            return;
        if (profileAsked)
            profileAsked(viewer::ProfileDo::Remove, p->name, {});
    }

    // The chosen profile, or the settings as they are (named then), as a file to pass on.
    void exportChosen()
    {
        enhance::Profile out;
        if (const enhance::Profile *p = chosenProfile()) {
            out = *p;
        } else {
            bool ok = false;
            out.name = QInputDialog::getText(this, viewer::tr("Save as a File"), viewer::tr("A name for these settings:"), QLineEdit::Normal, QString(), &ok)
                           .simplified()
                           .left(60);
            if (!ok || out.name.isEmpty())
                return;
            out.settings = settings();
            out.description = out.settings.describe().join(QStringLiteral(", "));
        }
        out.builtIn = false;
        if (profileAsked)
            profileAsked(viewer::ProfileDo::Export, out.name, enhance::profileToFile(out));
    }

    QComboBox *m_profiles;
    QLabel *m_profileNote;
    QAction *m_saveProfile, *m_removeProfile, *m_importProfile, *m_exportProfile;
    QVector<enhance::Profile> m_builtIn, m_saved;
    QPushButton *m_pick, *m_whole, *m_straighten, *m_stack;
    QSlider *m_local, *m_smooth, *m_deblock, *m_homomorphic, *m_retinex, *m_nlm, *m_noise;
    QComboBox *m_blurShape, *m_deblurWay, *m_shape;
    QSlider *m_fisheye;
    QPushButton *m_fitLine;
    QCheckBox *m_sharpest;
    QCheckBox *m_fullRange, *m_clipped, *m_redecode;
    QComboBox *m_field, *m_channel, *m_stackHow;
    QCheckBox *m_deinterlace, *m_steady, *m_stackDouble;
    QSpinBox *m_stackCount;
    QWidget *m_stackBox;
    QVector<QSlider *> m_rows;
    QSlider *m_brightness, *m_contrast, *m_shadows, *m_sharpen, *m_focus, *m_motion, *m_angle;
    QComboBox *m_grain, *m_bigger;
    QCheckBox *m_levels, *m_grey, *m_invert;
    QPushButton *m_measure, *m_compare, *m_save, *m_export;
    QLabel *m_status;
    bool m_quiet = false;
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

// A film's frames, drawn here so they can be looked into while it plays: the wheel zooms in on a
// part, dragging moves it, a double-click shows all of it again.
class FrameView : public QWidget
{
public:
    explicit FrameView(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("frames"));
        setCursor(Qt::OpenHandCursor);
        setMinimumSize(160, 120);
    }

    void setFrame(const QImage &frame)
    {
        m_frame = frame;
        update();
    }
    void clear()
    {
        m_frame = QImage();
        m_zoom = 1;
        m_centre = QPointF(0.5, 0.5);
        update();
    }
    double zoom() const { return m_zoom; }
    // The size frames are worth working on at: what's on the screen, zoom included.
    QSize workingSize() const { return size() * std::max(1.0, m_zoom) * devicePixelRatioF(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (m_frame.isNull())
            return;
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(target(), m_frame);
    }
    void wheelEvent(QWheelEvent *event) override
    {
        const QRectF before = target();
        const QPointF spot((event->position().x() - before.left()) / before.width(), (event->position().y() - before.top()) / before.height());
        m_zoom = std::clamp(m_zoom * std::pow(1.25, event->angleDelta().y() / 120.0), 1.0, 16.0);
        // The point under the pointer stays under it.
        const QRectF after = target();
        m_centre += QPointF(spot.x() - (event->position().x() - after.left()) / after.width(), spot.y() - (event->position().y() - after.top()) / after.height());
        clampCentre();
        update();
        event->accept();
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        m_drag = event->position();
        setCursor(Qt::ClosedHandCursor);
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton))
            return;
        const QRectF t = target();
        m_centre -= QPointF((event->position().x() - m_drag.x()) / t.width(), (event->position().y() - m_drag.y()) / t.height());
        m_drag = event->position();
        clampCentre();
        update();
    }
    void mouseReleaseEvent(QMouseEvent *) override { setCursor(Qt::OpenHandCursor); }
    void mouseDoubleClickEvent(QMouseEvent *) override
    {
        m_zoom = 1;
        m_centre = QPointF(0.5, 0.5);
        update();
    }

private:
    QRectF target() const
    {
        if (m_frame.isNull())
            return {};
        const QSizeF fitted = QSizeF(m_frame.size()).scaled(QSizeF(size()), Qt::KeepAspectRatio) * m_zoom;
        return QRectF(width() / 2.0 - m_centre.x() * fitted.width(), height() / 2.0 - m_centre.y() * fitted.height(), fitted.width(), fitted.height());
    }
    void clampCentre()
    {
        const double half = 0.5 / m_zoom;
        m_centre.setX(std::clamp(m_centre.x(), half, 1 - half));
        m_centre.setY(std::clamp(m_centre.y(), half, 1 - half));
    }

    QImage m_frame;
    double m_zoom = 1;
    QPointF m_centre{0.5, 0.5}; // the point of the frame in the middle, 0 to 1 across and down
    QPointF m_drag;
};

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
        , m_back(new QToolButton)
        , m_forward(new QToolButton)
        , m_speed(new QComboBox)
    {
        auto *layout = new QVBoxLayout(this);
        m_cover->setAlignment(Qt::AlignCenter);
        m_message->setWordWrap(true);
        m_message->setAlignment(Qt::AlignCenter);
        m_message->hide();
#ifdef DISKANVIL_MEDIA
        m_player = new QMediaPlayer(this);
        m_audio = new QAudioOutput(this);
        // The tests play films without a sound (DISKANVIL_MUTE): nobody wants a test beeping at them.
        m_audio->setMuted(qEnvironmentVariableIsSet("DISKANVIL_MUTE"));
        m_video = new FrameView;
        m_sink = new QVideoSink(this);
        m_player->setAudioOutput(m_audio);
        m_player->setVideoSink(m_sink);
        m_screen->addWidget(m_video);
        // Each frame as it comes, worked on at the size it's shown when it's being enhanced.
        QObject::connect(m_sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
            m_last = frame;
            if (m_collecting && frame.isValid()) {
                const qint64 at = frame.startTime() / 1000;
                if (m_collectedAt.isEmpty() || at > m_collectedAt.last()) {
                    m_collected << QVideoFrame(frame).toImage();
                    m_collectedAt << at;
                }
                if (m_collected.size() >= m_collectCount)
                    finishCollecting();
            }
            showFrame();
        });
        QObject::connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this] { showState(); });
        // At the end it goes back to the start, paused, showing the first picture (not an empty screen).
        QObject::connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
            if (status == QMediaPlayer::EndOfMedia) {
                if (m_collecting) {
                    finishCollecting();
                    return;
                }
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
        QObject::connect(m_back, &QToolButton::clicked, this, [this] { step(-1); });
        QObject::connect(m_forward, &QToolButton::clicked, this, [this] { step(1); });
        QObject::connect(m_speed, &QComboBox::currentIndexChanged, this, [this] { m_player->setPlaybackRate(speed()); });
#else
        m_message->setText(viewer::tr("Films and songs can't be played here: this DiskAnvil was built without Qt Multimedia. What's known about it is "
                              "on the right."));
        m_message->show();
        m_play->setEnabled(false);
        m_position->setEnabled(false);
        m_back->setEnabled(false);
        m_forward->setEnabled(false);
        m_speed->setEnabled(false);
#endif
        m_screen->addWidget(m_cover);
        layout->addWidget(m_screen, 1);
        layout->addWidget(m_message);
        auto *controls = new QHBoxLayout;
        m_play->setIcon(icon("media-playback-start"));
        m_play->setToolTip(viewer::tr("Play (Space)"));
        controls->addWidget(m_play);
        m_back->setIcon(icon("media-skip-backward"));
        m_back->setToolTip(viewer::tr("The frame before (,)"));
        m_back->setObjectName(QStringLiteral("frameBack"));
        m_forward->setIcon(icon("media-skip-forward"));
        m_forward->setToolTip(viewer::tr("The next frame (.)"));
        m_forward->setObjectName(QStringLiteral("frameForward"));
        controls->addWidget(m_back);
        controls->addWidget(m_forward);
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
        m_speed->setObjectName(QStringLiteral("speed"));
        m_speed->setToolTip(viewer::tr("How fast it plays: slower to see what happens in a moment"));
        for (const double rate : {0.1, 0.25, 0.5, 1.0, 1.5, 2.0})
            m_speed->addItem(rate == 1 ? viewer::tr("Normal speed") : viewer::tr("%1× speed").arg(QLocale().toString(rate)), rate);
        m_speed->setCurrentIndex(3);
        controls->addWidget(m_speed);
        layout->addLayout(controls);
    }

    double speed() const { return m_speed->currentData().toDouble(); }
    void setSpeed(double rate)
    {
        const int i = m_speed->findData(rate);
        if (i >= 0)
            m_speed->setCurrentIndex(i);
    }
    qint64 position() const
    {
#ifdef DISKANVIL_MEDIA
        return m_player->position();
#else
        return 0;
#endif
    }
    qint64 duration() const
    {
#ifdef DISKANVIL_MEDIA
        return m_player->duration();
#else
        return 0;
#endif
    }

    // A frame forward or back: paused, by one frame's time.
    void step(int frames)
    {
#ifdef DISKANVIL_MEDIA
        if (m_player->playbackState() == QMediaPlayer::PlayingState)
            m_player->pause();
        double rate = m_player->metaData().value(QMediaMetaData::VideoFrameRate).toDouble();
        if (rate < 1 || rate > 1000)
            rate = 30;
        m_player->setPosition(std::clamp<qint64>(m_player->position() + qint64(std::lround(frames * 1000.0 / rate)), 0, m_player->duration()));
#else
        Q_UNUSED(frames)
#endif
    }

    // The Enhance panel's light tools, on each frame as it plays.
    void setEnhance(const enhance::Settings &settings)
    {
        m_settings = settings;
#ifdef DISKANVIL_MEDIA
        showFrame();
#endif
    }

    // `count` frames of the film one after another around `around` (ms), as it has them, for
    // stacking: played through from a little before, each new frame kept. `done` hears them and
    // which one is nearest `around`; it's left paused there again.
    void collectFrames(qint64 around, int count, const std::function<void(const QVector<QImage> &, int)> &done)
    {
#ifdef DISKANVIL_MEDIA
        double rate = m_player->metaData().value(QMediaMetaData::VideoFrameRate).toDouble();
        if (rate < 1 || rate > 1000)
            rate = 30;
        m_collected.clear();
        m_collectedAt.clear();
        m_collectAround = around;
        m_collectCount = count;
        m_collectDone = done;
        m_collectRate = m_player->playbackRate();
        m_collecting = true;
        m_player->setPlaybackRate(0.5); // slower than it plays, so no frame is skipped
        m_player->setPosition(std::max<qint64>(0, around - qint64(std::lround(count / 2 * 1000.0 / rate))));
        m_player->play();
        QTimer::singleShot(60000, this, [this] { finishCollecting(); });
#else
        Q_UNUSED(around)
        Q_UNUSED(count)
        done({}, -1);
#endif
    }
    bool collecting() const { return m_collecting; }

    // The frame on the screen now as the film has it, nothing applied.
    QImage rawFrame() const
    {
#ifdef DISKANVIL_MEDIA
        return QVideoFrame(m_last).toImage();
#else
        return {};
#endif
    }

    // The frame on the screen now, whole and with what's set applied.
    QImage frame() const
    {
#ifdef DISKANVIL_MEDIA
        return enhance::apply(QVideoFrame(m_last).toImage(), m_settings);
#else
        return {};
#endif
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
        m_video->clear();
        m_last = QVideoFrame();
        m_settings = enhance::Settings();
        setSpeed(1.0);
        m_back->setEnabled(film);
        m_forward->setEnabled(film);
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

    void finishCollecting()
    {
        if (!m_collecting)
            return;
        m_collecting = false;
        m_player->pause();
        m_player->setPlaybackRate(m_collectRate);
        m_player->setPosition(m_collectAround);
        int nearest = -1;
        for (int i = 0; i < m_collectedAt.size(); ++i) {
            if (nearest < 0 || std::abs(m_collectedAt[i] - m_collectAround) < std::abs(m_collectedAt[nearest] - m_collectAround))
                nearest = i;
        }
        const auto done = std::move(m_collectDone);
        const QVector<QImage> frames = std::move(m_collected);
        m_collected.clear();
        m_collectedAt.clear();
        if (done)
            done(frames, nearest);
    }

    void showFrame()
    {
        if (!m_last.isValid())
            return;
        QImage image = QVideoFrame(m_last).toImage();
        if (!m_settings.isDefault()) {
            // Worked on at the size it's shown, so it keeps up while it plays.
            const QSize room = m_video->workingSize();
            if (image.width() > room.width() || image.height() > room.height())
                image = image.scaled(room, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            image = enhance::apply(image, m_settings);
        }
        m_video->setFrame(image);
    }

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QVideoSink *m_sink = nullptr;
    FrameView *m_video = nullptr;
    QVideoFrame m_last;
    QVector<QImage> m_collected;
    QVector<qint64> m_collectedAt;
    qint64 m_collectAround = 0;
    int m_collectCount = 0;
    double m_collectRate = 1;
    std::function<void(const QVector<QImage> &, int)> m_collectDone;
#endif
    bool m_collecting = false;
    enhance::Settings m_settings;
    QStackedWidget *m_screen;
    QLabel *m_cover;
    QToolButton *m_play;
    QSlider *m_position;
    QLabel *m_time;
    QSlider *m_volume;
    QLabel *m_message;
    QToolButton *m_back, *m_forward;
    QComboBox *m_speed;
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
    , m_enhance(new EnhancePanel)
    , m_enhanceTimer(new QTimer(this))
    , m_enhanceRound(std::make_shared<std::atomic<int>>(0))
{
    setWindowTitle(tr("Find Lost Files: Viewer"));
    setObjectName(QStringLiteral("viewer"));
    auto *bar = addToolBar(tr("Viewer"));
    m_bar = bar;
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
    m_pixels = bar->addAction(icon("view-grid"), tr("Pixels"));
    m_pixels->setObjectName(QStringLiteral("pixelsAction"));
    m_pixels->setCheckable(true);
    m_pixels->setToolTip(tr("Show each pixel as a square, exactly as the picture has it (otherwise a picture shown bigger is made bigger smoothly)"));
    for (QAction *a : {m_zoomOut, m_fit, m_actual, m_zoomIn, m_pixels})
        bar->widgetForAction(a)->setProperty("zoom", true);
    bar->addSeparator();
    m_details = bar->addAction(icon("documentinfo"), tr("Details"));
    m_details->setCheckable(true);
    m_details->setChecked(true);
    m_insideAction = bar->addAction(icon("view-list-tree"), tr("Inside"));
    m_insideAction->setCheckable(true);
    m_insideAction->setObjectName(QStringLiteral("insideAction"));
    m_insideAction->setToolTip(tr("What the file is made of, part by part, and its bytes (damage shows in red)"));
    m_enhanceAction = bar->addAction(icon("color-management"), tr("Enhance"));
    m_enhanceAction->setCheckable(true);
    m_enhanceAction->setObjectName(QStringLiteral("enhanceAction"));
    m_enhanceAction->setToolTip(tr("Lighter, sharper, blur taken out: a closer look, saved as a new picture"));
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
    m_facts->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    m_facts->setOpenExternalLinks(false);
    connect(m_facts, &QLabel::linkActivated, this, [this](const QString &link) {
        if (link.startsWith(QLatin1String("geo:")))
            Q_EMIT asked(Ask::Map);
    });
    m_side->setWidget(m_facts);
    m_side->setWidgetResizable(true);
    m_side->setMinimumWidth(220);

    auto *middle = new QWidget;
    auto *middleLayout = new QVBoxLayout(middle);
    middleLayout->setContentsMargins(0, 0, 0, 0);
    middleLayout->addWidget(m_problem);
    middleLayout->addWidget(m_pages, 1);
    auto *enhanceScroll = new QScrollArea;
    enhanceScroll->setWidget(m_enhance);
    enhanceScroll->setWidgetResizable(true);
    enhanceScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    enhanceScroll->hide();
    // As wide as the panel needs (worked out when it's shown: some of it shows only for some files).
    m_fitEnhance = [this, enhanceScroll] {
        m_enhance->adjustSize();
        enhanceScroll->setMinimumWidth(m_enhance->minimumSizeHint().width() + style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 8);
    };
    m_fitEnhance();
    auto *split = new QSplitter;
    split->addWidget(middle);
    split->addWidget(enhanceScroll);
    split->addWidget(m_side);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 1);
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
    connect(m_pixels, &QAction::toggled, this, [this](bool on) { m_picture->setShowPixels(on); });
    connect(m_zoomOut, &QAction::triggered, this, [this] { m_shown.kind == Kind::Pdf ? m_pdf->zoomBy(0.8) : m_picture->zoomBy(0.8); });
    connect(m_fit, &QAction::triggered, this, [this] { m_shown.kind == Kind::Pdf ? m_pdf->fit() : m_picture->fit(); });
    connect(m_actual, &QAction::triggered, this, [this] { m_shown.kind == Kind::Pdf ? m_pdf->actualSize() : m_picture->actualSize(); });

    m_enhanceTimer->setSingleShot(true);
    m_enhanceTimer->setInterval(180);
    connect(m_enhanceTimer, &QTimer::timeout, this, &Window::enhanceNow);
    connect(m_enhanceAction, &QAction::toggled, this, [this, enhanceScroll](bool on) {
        if (on)
            m_fitEnhance();
        enhanceScroll->setVisible(on);
        // On a screen without room for both, Details makes way while Enhance is open.
        if (on && m_details->isChecked() && width() < 1500) {
            m_detailsForEnhance = true;
            m_details->setChecked(false);
        } else if (!on && m_detailsForEnhance) {
            m_detailsForEnhance = false;
            m_details->setChecked(true);
        }
        if (!on) {
            if (!m_whole.isNull() || m_spotFromFilm)
                showWhole();
            m_enhance->reset();
            ++*m_enhanceRound;
            m_picture->showProcessed(QImage());
            m_picture->setMeasuring(false);
        }
    });
    // A film's frames are enhanced as they're shown (the light tools only); a picture a moment later.
    m_enhance->changed = [this] {
        if (m_shown.kind == Kind::Film && !m_spotFromFilm)
            m_media->setEnhance(m_enhance->settings());
        else
            enhanceSoon();
    };
    m_enhance->compare = [this](bool on) {
        if (m_shown.kind == Kind::Film && !m_spotFromFilm)
            m_media->setEnhance(on ? enhance::Settings() : m_enhance->settings());
        else
            m_picture->setComparing(on);
    };
    m_enhance->exportFilm = [this] {
        Q_EMIT exportFilm(m_media->speed(), m_enhance->settings(), m_media->duration());
    };
    m_enhance->measure = [this](bool on) { m_picture->setMeasuring(on); };
    m_picture->measured = [this](int length, int angle) {
        m_picture->setMeasuring(false);
        m_enhance->setMotion(length, angle);
    };
    m_enhance->save = [this](int factor) { saveEnhanced(factor); };
    m_enhance->profileAsked = [this](ProfileDo what, const QString &name, const QByteArray &file) {
        Q_EMIT profileAsked(what, name, file, {});
    };
    m_enhance->pick = [this](bool on) { pickSpot(on); };
    m_enhance->whole = [this] { showWhole(); };
    m_picture->picked = [this](const QRect &spot) { showSpot(spot); };
    m_enhance->straightenAsked = [this](bool on) {
        if (on && m_shown.kind == Kind::Film && !m_spotFromFilm && !freezeFrame())
            return;
        straightenSpot(on);
    };
    m_picture->cornered = [this](const QPolygonF &corners) {
        if (m_fitting) {
            m_fitting = false;
            const int found = enhance::fisheyeFromLine(QVector<QPointF>(corners.begin(), corners.end()), m_picture->original().size());
            m_enhance->setFisheye(std::max(0, found));
            m_enhance->setStatus(found > 0 ? tr("The lens's bend from that line: %1.").arg(found) : tr("That line is straight already: no bend to take out."));
            return;
        }
        straightened(corners);
    };
    m_enhance->stackAsked = [this](int frames, int scale, int how) { stackFrames(frames, scale, how); };
    m_enhance->showClipped = [this](bool on) { m_picture->setShowClipped(on); };
    m_enhance->redecode = [this](bool on) { redecodeJpeg(on); };
    m_enhance->fitLine = [this](bool on) {
        m_fitting = on;
        m_picture->setPicking(false);
        m_picture->setCornering(on, 3);
        m_enhance->setStatus(on ? tr("Click three points along something that should be straight: one end, the middle, the other end.") : QString());
    };
    m_enhance->shape = [this](int index) { reshape(index); };
    setAcceptDrops(true);
}

void Window::setProfiles(const QList<QByteArray> &files, const QString &note)
{
    m_enhance->setSaved(files, note);
}

namespace {
// The profile files among what's dragged here.
QStringList profileFiles(const QMimeData *data)
{
    QStringList out;
    if (!data || !data->hasUrls())
        return out;
    for (const QUrl &url : data->urls()) {
        const QString path = url.toLocalFile();
        if (path.endsWith(QLatin1Char('.') + QLatin1String(enhance::kProfileSuffix), Qt::CaseInsensitive)
            || path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
            out << path;
    }
    return out;
}
} // namespace

void Window::dragEnterEvent(QDragEnterEvent *event)
{
    if (!profileFiles(event->mimeData()).isEmpty())
        event->acceptProposedAction();
}

void Window::dropEvent(QDropEvent *event)
{
    // The viewer can't open them (it can't see the home folder): Find Lost Files reads and checks them.
    const QStringList files = profileFiles(event->mimeData());
    if (files.isEmpty())
        return;
    event->acceptProposedAction();
    m_enhanceAction->setChecked(m_enhanceAction->isVisible() || m_enhanceAction->isChecked());
    Q_EMIT profileAsked(ProfileDo::Dropped, {}, {}, files.mid(0, 50));
}

bool Window::freezeFrame()
{
    // The frame on the screen, paused, becomes a still picture to work on (and the slow tools work on it).
    if (m_media->playing())
        m_media->playPause();
    const QImage frame = m_media->rawFrame();
    if (frame.isNull()) {
        m_enhance->setStatus(tr("Play the film to the moment you want, then pick a spot in it."));
        m_enhance->setSpotShown(false);
        return false;
    }
    m_spotAt = m_media->position();
    m_spotWhen = QStringLiteral("%1m%2s%3").arg(m_spotAt / 60000).arg(m_spotAt / 1000 % 60, 2, 10, QLatin1Char('0')).arg(m_spotAt % 1000, 3, 10, QLatin1Char('0'));
    m_spotFromFilm = true;
    m_whole = frame;
    m_picture->showImage(frame);
    m_pages->setCurrentWidget(m_picture);
    m_enhance->setForFilm(false);
    m_enhance->setSpotShown(true);
    m_enhance->setStackable(true);
    enhanceSoon();
    return true;
}

void Window::pickSpot(bool on)
{
    m_picture->setCornering(false);
    if (!on) {
        m_picture->setPicking(false);
        return;
    }
    if (m_shown.kind == Kind::Film && !m_spotFromFilm && !freezeFrame())
        return;
    if (m_whole.isNull())
        m_whole = m_picture->original();
    m_picture->setPicking(true);
    m_enhance->setStatus(tr("Drag a box around the spot."));
}

void Window::showSpot(QRect spot)
{
    if (m_whole.isNull())
        return;
    m_picture->setPicking(false);
    if (m_reshaped) {
        // After straightening or stacking: a spot of that.
        const QImage now = m_picture->original();
        spot = spot.intersected(now.rect());
        m_steps << tr("then the spot of %1 × %2 pixels from %3, %4 of that").arg(spot.width()).arg(spot.height()).arg(spot.x()).arg(spot.y());
        m_picture->showSpot(now, spot);
    } else {
        // A spot picked inside a spot is part of the one before.
        if (!m_spot.isNull())
            spot.translate(m_spot.topLeft());
        m_spot = spot.intersected(m_whole.rect());
        spot = m_spot;
        m_picture->showSpot(m_whole, m_spot);
    }
    m_enhance->setSpotShown(true);
    // Saved bigger: as big as fits in 4 times, about as big as the screen shows it.
    const int longest = std::max(spot.width(), spot.height());
    m_enhance->setBigger(longest >= 900 ? 1 : longest >= 450 ? 2 : longest >= 300 ? 3 : 4);
    m_enhance->setStatus(tr("The spot: %1 × %2 pixels, from %3, %4 in. It's shown bigger, smoothly: what's there, nothing made up.")
                             .arg(spot.width()).arg(spot.height()).arg(spot.x()).arg(spot.y())
                         + (m_spotFromFilm && !m_reshaped ? QLatin1Char(' ') + tr("Stack Frames puts the frames around it together.") : QString()));
    enhanceSoon();
}

void Window::straightenSpot(bool on)
{
    m_picture->setPicking(false);
    if (!on) {
        m_picture->setCornering(false);
        return;
    }
    if (m_whole.isNull())
        m_whole = m_picture->original();
    m_picture->setCornering(true);
    m_enhance->setStatus(tr("Click its four corners: top left, top right, bottom right, bottom left."));
}

void Window::straightened(const QPolygonF &corners)
{
    const QImage flat = enhance::straighten(m_picture->original(), corners);
    m_enhance->setSpotShown(true);
    if (flat.isNull()) {
        m_enhance->setStatus(tr("Those corners don't make a shape that can be straightened: click them again, going round it."));
        return;
    }
    // In a film's frame, the corners in the frame's own pixels: Stack Frames can follow them.
    m_frameCorners.clear();
    if (m_spotFromFilm && !m_reshaped) {
        const QPointF offset = m_spot.isNull() ? QPointF() : QPointF(m_spot.topLeft());
        for (const QPointF &p : corners)
            m_frameCorners << p + offset;
        m_straightSize = flat.size();
    }
    m_reshaped = true;
    m_enhance->setStackable(!m_frameCorners.isEmpty());
    if (!m_frameCorners.isEmpty())
        m_enhance->setStackLabel(tr("Stack Frames (Following It)"));
    m_steps << tr("straightened (four corners made square-on)");
    m_picture->showSpot(flat, flat.rect());
    m_enhance->setStatus(tr("Straightened: %1 × %2 pixels, square-on.").arg(flat.width()).arg(flat.height()));
    enhanceSoon();
}

void Window::stackFrames(int count, int scale, int how)
{
    // A stack of the spot; or, once it's straightened, of the flat thing followed frame to frame.
    const bool follow = !m_frameCorners.isEmpty();
    if (!m_spotFromFilm || (m_reshaped && !follow) || m_media->collecting())
        return;
    const bool sharpestOnly = how >= 10;
    how %= 10;
    const QRect region = m_spot.isNull() ? m_whole.rect() : m_spot;
    const QPolygonF corners = m_frameCorners;
    const QSize flatSize = m_straightSize;
    m_enhance->setStacking(true);
    m_enhance->setStatus(tr("Collecting %n frame(s) around this one…", nullptr, count));
    QPointer<Window> self(this);
    m_media->collectFrames(m_spotAt, count, [self, region, scale, count, how, follow, corners, flatSize, sharpestOnly](const QVector<QImage> &frames, int nearest) {
        if (!self)
            return;
        if (frames.size() < 2 || nearest < 0) {
            self->m_enhance->setStacking(false);
            self->m_enhance->setStatus(Window::tr("The frames around it couldn't be read."));
            return;
        }
        QVector<QImage> spots;
        if (follow) {
            const QVector<QPolygonF> followed = enhance::followCorners(frames, nearest, corners);
            for (int i = 0; i < frames.size(); ++i)
                spots << enhance::straighten(frames[i], followed[i], flatSize);
        } else {
            for (const QImage &f : frames)
                spots << f.copy(region);
        }
        int dropped = 0;
        if (sharpestOnly && spots.size() > 3) {
            // The sharpest 60% (at least three), the one paused on always among them.
            QVector<QPair<double, int>> scores;
            for (int i = 0; i < spots.size(); ++i)
                scores << qMakePair(i == nearest ? 1e300 : enhance::sharpness(spots[i]), i);
            std::sort(scores.begin(), scores.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            const int keep = std::max(3, int(spots.size() * 6 / 10));
            QVector<int> kept;
            for (int i = 0; i < keep; ++i)
                kept << scores[i].second;
            std::sort(kept.begin(), kept.end());
            QVector<QImage> best;
            for (int i : kept)
                best << spots[i];
            nearest = int(kept.indexOf(nearest));
            dropped = int(spots.size() - best.size());
            spots = best;
        }
        self->m_enhance->setStatus(Window::tr("Lining up %n frame(s) and putting them together…", nullptr, int(spots.size())));
        const std::shared_ptr<std::atomic<int>> current = self->m_enhanceRound;
        const int round = current->load();
        QThreadPool::globalInstance()->start([self, spots, nearest, scale, current, round, count, how, follow, dropped] {
            QVector<QPointF> moved;
            const QImage stacked = enhance::stack(spots, nearest, scale, enhance::Combine(std::clamp(how, 0, 2)), &moved,
                                                  [current, round](int) { return current->load() == round; });
            QMetaObject::invokeMethod(qApp, [self, stacked, used = int(spots.size()), scale, count, how, follow, dropped] {
                if (!self)
                    return;
                self->m_enhance->setStacking(false);
                if (stacked.isNull()) {
                    self->m_enhance->setStatus(QString());
                    return;
                }
                self->m_reshaped = true;
                self->m_enhance->setStackable(false);
                const QString combined = how == 1 ? Window::tr("each point's middle value") : how == 2 ? Window::tr("averaged without outliers") : Window::tr("averaged");
                QString how2 = scale > 1 ? Window::tr("%1 frames around it lined up and put together (%2), twice the size (shift and add)").arg(used).arg(combined)
                                         : Window::tr("%1 frames around it lined up and %2").arg(used).arg(combined);
                if (follow)
                    how2 += Window::tr(", its four corners followed and each frame straightened");
                if (dropped > 0)
                    how2 += Window::tr(", the %n least sharp left out", nullptr, dropped);
                self->m_steps << how2;
                self->m_frameCorners.clear();
                self->m_picture->showSpot(stacked, stacked.rect());
                self->m_enhance->setBigger(scale > 1 ? 2 : 4);
                self->m_enhance->setStatus(used < count ? Window::tr("Stacked %1 frames (the film had no more there).").arg(used)
                                                        : Window::tr("Stacked %1 frames: the grain evened out.").arg(used));
                self->enhanceSoon();
            }, Qt::QueuedConnection);
        });
    });
}

void Window::reshape(int index)
{
    // Its proper shape: the picture as it is now, made that shape (smoothly), as a step.
    const QImage now = m_picture->original();
    if (now.isNull() || index <= 0)
        return;
    QSize to = now.size();
    QString words;
    switch (index) {
    case 1: to.setWidth(now.height() * 4 / 3); words = tr("made 4:3"); break;
    case 2: to.setWidth(now.height() * 16 / 9); words = tr("made 16:9"); break;
    case 3: to.setHeight(now.height() * 2); words = tr("made twice as tall"); break;
    case 4: to.setWidth(now.width() * 2); words = tr("made twice as wide"); break;
    default: return;
    }
    const QImage shaped = enhance::resize(now, to);
    if (shaped.isNull())
        return;
    if (m_whole.isNull())
        m_whole = now;
    m_reshaped = true;
    m_enhance->setStackable(false);
    m_steps << words;
    m_picture->showSpot(shaped, shaped.rect());
    m_enhance->setSpotShown(true);
    m_enhance->setStatus(tr("Its shape: %1 × %2 pixels.").arg(shaped.width()).arg(shaped.height()));
    enhanceSoon();
}

void Window::redecodeJpeg(bool on)
{
    if (!on) {
        // Back to the picture as the file decodes normally.
        if (!m_asFound.isNull()) {
            m_picture->showImage(m_asFound);
            m_asFound = QImage();
        }
        m_steps.removeAll(m_redecodeStep);
        enhanceSoon();
        return;
    }
    QFile file(m_shown.path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QByteArray bytes = file.read(64 * 1024 * 1024);
    m_enhance->setRedecodeBusy(true);
    m_enhance->setStatus(tr("Decoding it again within what it stores…"));
    QPointer<Window> self(this);
    const QString path = m_shown.path;
    QThreadPool::globalInstance()->start([self, bytes, path] {
        QString why;
        const QImage decoded = jpegexact::decode(bytes, 30, &why);
        QMetaObject::invokeMethod(qApp, [self, decoded, why, path] {
            if (!self || self->m_shown.path != path)
                return;
            self->m_enhance->setRedecodeBusy(false);
            if (decoded.isNull()) {
                self->m_enhance->setStatus(why);
                self->m_enhance->setRedecodable(true);
                return;
            }
            self->m_asFound = self->m_picture->original();
            self->m_picture->showImage(decoded);
            self->m_redecodeStep = Window::tr("decoded again within what the JPEG stores (30 rounds)");
            self->m_steps.prepend(self->m_redecodeStep);
            self->m_enhance->setStatus(Window::tr("Decoded again: the squares are smoothed, within the numbers the file keeps."));
            self->enhanceSoon();
        }, Qt::QueuedConnection);
    });
}

void Window::showWhole()
{
    ++*m_enhanceRound;
    m_picture->setPicking(false);
    m_picture->setCornering(false);
    m_reshaped = false;
    m_steps.clear();
    m_frameCorners.clear();
    m_enhance->setStackable(false);
    m_enhance->setStackLabel(tr("Stack Frames"));
    m_enhance->resetShape();
    if (m_spotFromFilm) {
        // Back to the film where it was paused.
        m_spotFromFilm = false;
        m_whole = QImage();
        m_spot = QRect();
        m_picture->clear();
        m_pages->setCurrentWidget(m_media);
        m_enhance->setForFilm(true);
        m_enhance->setSpotShown(false);
        m_enhance->setBigger(1);
        m_enhance->setStatus(QString());
        m_media->setEnhance(m_enhance->settings());
        return;
    }
    if (!m_whole.isNull())
        m_picture->showImage(m_whole);
    m_whole = QImage();
    m_spot = QRect();
    m_enhance->setSpotShown(false);
    m_enhance->setBigger(1);
    m_enhance->setStatus(QString());
    enhanceSoon();
}

void Window::enhanceSoon()
{
    m_enhanceTimer->start();
}

void Window::enhanceNow()
{
    const int round = ++*m_enhanceRound;
    const enhance::Settings settings = m_enhance->settings();
    const QImage original = m_picture->original();
    if (original.isNull() || settings.isDefault()) {
        m_picture->showProcessed(QImage());
        m_enhance->setStatus(QString());
        return;
    }
    // A quick look on a copy no bigger than the screen, then the whole picture.
    const int longest = std::max(original.width(), original.height());
    const QImage quick = longest > 1400 ? original.scaled(1400, 1400, Qt::KeepAspectRatio, Qt::SmoothTransformation) : original;
    m_enhance->setStatus(tr("Working…"));
    const std::shared_ptr<std::atomic<int>> current = m_enhanceRound;
    QPointer<Window> self(this);
    QThreadPool::globalInstance()->start([self, current, round, settings, quick, original, longest] {
        const auto still = [current, round](int) { return current->load() == round; };
        QImage shown = enhance::apply(quick, settings, still);
        if (shown.isNull() || current->load() != round)
            return;
        QMetaObject::invokeMethod(qApp, [self, current, round, shown, more = longest > 1400] {
            if (!self || current->load() != round)
                return;
            self->m_picture->showProcessed(shown);
            self->m_enhance->setStatus(more ? Window::tr("Working on the whole picture…") : QString());
        }, Qt::QueuedConnection);
        if (longest <= 1400)
            return;
        const QImage whole = enhance::apply(original, settings, still);
        if (whole.isNull() || current->load() != round)
            return;
        QMetaObject::invokeMethod(qApp, [self, current, round, whole] {
            if (!self || current->load() != round)
                return;
            self->m_picture->showProcessed(whole);
            self->m_enhance->setStatus(QString());
        }, Qt::QueuedConnection);
    });
}

void Window::saveEnhanced(int factor)
{
    if (m_shown.kind == Kind::Film && !m_spotFromFilm) {
        // The frame on the screen, whole, with the tools applied: named after where in the film it is.
        const QImage frame = m_media->frame();
        if (frame.isNull())
            return;
        const qint64 at = m_media->position();
        QImage picture = frame;
        const QString when = QStringLiteral("%1m%2s%3").arg(at / 60000).arg(at / 1000 % 60, 2, 10, QLatin1Char('0')).arg(at % 1000, 3, 10, QLatin1Char('0'));
        const QStringList done = m_enhance->settings().describe();
        picture.setText(QStringLiteral("Description"), tr("A frame of \"%1\", %2 in, saved by DiskAnvil's viewer%3.")
                                                           .arg(m_shown.name, when, done.isEmpty() ? QString() : QStringLiteral(": ") + done.join(QStringLiteral(", "))));
        picture.setText(QStringLiteral("Software"), QStringLiteral("DiskAnvil"));
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        picture.save(&buffer, "PNG");
        Q_EMIT savePicture(tr("%1 frame %2.png").arg(QFileInfo(m_shown.name).completeBaseName(), when), png);
        return;
    }
    const QImage original = m_picture->original();
    if (original.isNull())
        return;
    const enhance::Settings settings = m_enhance->settings();
    // Four times as big is 16 times the pixels: kept under 200 million of them.
    while (factor > 1 && qint64(original.width()) * factor * original.height() * factor > 200000000LL)
        --factor;
    m_enhance->setStatus(tr("Making the picture to save…"));
    const QString base = QFileInfo(m_shown.name).completeBaseName() + (m_spotFromFilm ? tr(" frame %1").arg(m_spotWhen) : QString());
    const QString name = base + (m_spot.isNull() && m_steps.isEmpty() ? QString() : tr(" (spot)")) + (settings.isDefault() ? QString() : tr(" (enhanced)"))
                       + (factor > 1 ? tr(" (x%1)").arg(factor) : QString()) + QStringLiteral(".png");
    QStringList done;
    if (m_spotFromFilm)
        done << tr("the frame %1 in").arg(m_spotWhen);
    if (!m_spot.isNull())
        done << tr("only the spot of %1 × %2 pixels from %3, %4").arg(m_spot.width()).arg(m_spot.height()).arg(m_spot.x()).arg(m_spot.y());
    done << m_steps;
    done << settings.describe();
    if (factor > 1)
        done << tr("made %1 times as big (Lanczos)").arg(factor);
    const QString note = tr("Made by DiskAnvil's viewer from \"%1\": %2.").arg(m_shown.name, done.isEmpty() ? tr("nothing changed") : done.join(QStringLiteral(", ")));
    QPointer<Window> self(this);
    QThreadPool::globalInstance()->start([self, original, settings, factor, name, note] {
        QImage picture = enhance::enlarge(enhance::apply(original, settings), factor);
        // What was done goes in the picture itself, so a copy always says it isn't the original.
        picture.setText(QStringLiteral("Description"), note);
        picture.setText(QStringLiteral("Software"), QStringLiteral("DiskAnvil"));
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        picture.save(&buffer, "PNG");
        QMetaObject::invokeMethod(qApp, [self, name, png] {
            if (!self)
                return;
            self->m_enhance->setStatus(QString());
            Q_EMIT self->savePicture(name, png);
        }, Qt::QueuedConnection);
    });
}

Window::~Window()
{
    ++*m_enhanceRound; // whatever's being worked on stops
    QThreadPool::globalInstance()->waitForDone();
}

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
    m_whole = QImage();
    m_spot = QRect();
    m_spotFromFilm = false;
    m_reshaped = false;
    m_steps.clear();
    m_enhance->setStackable(false);
    ++*m_enhanceRound;
    m_enhance->reset();
    m_picture->clear();
    m_pdf->clear();
    m_problem->hide();
    m_title->setText(show.where.isEmpty() ? show.name : show.name + QStringLiteral(" · ") + show.where);
    setWindowTitle(QStringLiteral("%1 – %2").arg(show.name, show.from.isEmpty() ? tr("Find Lost Files") : show.from));
    m_previous->setEnabled(show.previous);
    m_next->setEnabled(show.next);
    m_open->setEnabled(show.openable);
    m_repair->setVisible(!show.repairable.isEmpty());
    if (m_fitEnhance)
        m_fitEnhance();
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
        } else if (type && type->category == filetypes::Category::Document && type->id != QLatin1String("txt")) {
            text = officetext::text(show.type, data, 2000000);
        }
        if (text.isEmpty() && type && (type->category == filetypes::Category::Archive || type->id == QLatin1String("epub"))) {
            QStringList lines;
            for (const officetext::Entry &e : officetext::list(data, 20000))
                lines << (e.folder ? e.path : QStringLiteral("%1  (%2)").arg(e.path, formatSize(e.size)));
            text = lines.join(QLatin1Char('\n'));
        }
        if (text.isEmpty() && (!type || type->id == QLatin1String("txt"))) {
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
    // A small picture says so: made bigger it's smooth, but it can't show what it never had.
    if (kind == Kind::Picture) {
        const QSize size = m_picture->pictureSize();
        if (!size.isEmpty() && std::max(size.width(), size.height()) < 320) {
            m_shown.facts << tr("Small") + QLatin1Char('\t')
                                 + tr("Only %1 × %2 pixels: probably a thumbnail (a small copy a camera, phone or Windows keeps). Shown bigger it's "
                                      "smoothed; no tool can add the detail it never had. The full photo may be among the files found too: "
                                      "look for one from the same time, or sort by size.")
                                       .arg(size.width()).arg(size.height());
            showFacts();
        }
    }
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
    m_pixels->setVisible(kind == Kind::Picture);
    // Enhancing is for still pictures (an animated one shows its frames as they come) and films.
    const bool canEnhance = (kind == Kind::Picture && !m_picture->animated()) || kind == Kind::Film;
    m_enhanceAction->setVisible(canEnhance);
    m_enhance->setForFilm(kind == Kind::Film);
    m_asFound = QImage();
    m_enhance->setRedecodable(kind == Kind::Picture && show.type >= 0 && filetypes::types()[show.type].id == QLatin1String("jpg"));
    if (!canEnhance)
        m_enhanceAction->setChecked(false);
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
            // Where it was taken opens on a map, from Find Lost Files' window (which asks first).
            const QString shown = value.startsWith(QLatin1String("geo:"))
                                      ? QStringLiteral("<a href=\"%1\">%2</a>").arg(value.toHtmlEscaped(), tr("Open on OpenStreetMap…").toHtmlEscaped())
                                      : value.toHtmlEscaped();
            html += QStringLiteral("<tr><td style=\"color:%1; padding-right:10px\">%2</td><td>%3</td></tr>").arg(quiet, label.toHtmlEscaped(), shown);
        }
        html += QStringLiteral("</table>");
    }
    m_facts->setText(html);
}

void Window::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    // Words beside the icons when they fit; only icons (their tooltips say what they are) when they don't.
    if (!m_bar)
        return;
    // What it takes with words: each button's icon and words, and the other parts as they are.
    int needed = 0;
    const QFontMetrics metrics(m_bar->font());
    for (QAction *a : m_bar->actions()) {
        if (!a->isVisible())
            continue;
        QWidget *w = m_bar->widgetForAction(a);
        if (a->isSeparator())
            needed += 12;
        else if (qobject_cast<QToolButton *>(w))
            needed += m_bar->iconSize().width() + metrics.horizontalAdvance(a->iconText()) + 24;
        else if (w)
            needed += w->sizeHint().width();
    }
    m_barWidth = needed;
    const Qt::ToolButtonStyle fits = width() >= m_barWidth ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly;
    if (m_bar->toolButtonStyle() != fits)
        m_bar->setToolButtonStyle(fits);
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
    case Qt::Key_Comma:
    case Qt::Key_Period:
        if (m_shown.kind == Kind::Film) {
            m_media->step(event->key() == Qt::Key_Period ? 1 : -1);
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
    QObject::connect(&window, &Window::exportFilm, &window, [&out](double speed, const enhance::Settings &settings, qint64 durationMs) {
        Message m;
        m.type = Message::ExportFilm;
        m.speed = speed;
        m.settings = settings;
        m.durationMs = durationMs;
        out.write(encode(m));
        out.flush();
    });
    QObject::connect(&window, &Window::savePicture, &window, [&out](const QString &name, const QByteArray &png) {
        Message m;
        m.type = Message::SavePicture;
        m.pictureName = name;
        m.picture = png;
        out.write(encode(m));
        out.flush();
    });
    QObject::connect(&window, &Window::profileAsked, &window,
                     [&out](ProfileDo what, const QString &name, const QByteArray &file, const QStringList &dropped) {
                         Message m;
                         m.type = Message::Profile;
                         m.profileDo = what;
                         m.profileName = name;
                         m.profile = file;
                         m.dropped = dropped;
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
            // Inside the sandbox the desktop's own choice (KDE keeps it in the home folder) can't be
            // read, and the default light icons are dark on a dark theme: DiskAnvil's own are used.
            if (!m.iconTheme.isEmpty())
                QIcon::setThemeName(m.iconTheme);
            if (!m.fallbackIconTheme.isEmpty())
                QIcon::setFallbackThemeName(m.fallbackIconTheme);
        } else if (m.type == Message::ShowFile) {
            window.present(m.show);
            window.raise();
            window.activateWindow();
            Q_EMIT window.asked(Ask::Shown);
        } else if (m.type == Message::Facts) {
            window.setFacts(m.show.facts);
        } else if (m.type == Message::Profiles) {
            window.setProfiles(m.profiles, m.note);
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
