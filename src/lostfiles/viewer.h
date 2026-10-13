// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Find Lost Files' viewer: a found file shown in full before it's saved. Pictures to zoom into,
// PDFs page by page, films and songs that play, documents' words, and the bytes of anything
// else. It runs as its own program (diskanvil --viewer), in a sandbox on Linux, so a damaged
// file can't take DiskAnvil down. Find Lost Files hands it copies of found files (lookcopies.h)
// on its standard input, and it asks back on its standard output for the one before or after,
// or for this one to be saved or opened in its own program.

#include "enhance.h"

#include <QMainWindow>
#include <QMap>
#include <QPalette>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <memory>

class QIODevice;
class QToolBar;
class QLabel;
class QPlainTextEdit;
class QScrollArea;
class QStackedWidget;

namespace viewer {

enum class Kind : qint32 { Picture = 1, Pdf, Film, Song, Words, Bytes };
// How a found file is shown, from its kind (a filetypes number) and name.
Kind kindFor(int type, const QString &name);

// A file to show.
struct Show {
    Kind kind = Kind::Bytes;
    int type = -1; // its filetypes number, -1 when not known
    QString path;  // the copy to show
    QString name;  // its own name
    QString where; // "3 of 120"
    QStringList facts; // "label\tvalue", what's known about it
    bool previous = false, next = false; // whether there's one before it and after it
    bool openable = false;               // safe to open in the program the computer has for it
    QString repairable;                  // what Repair does for it; empty when it can't be repaired
};

// What the viewer asks Find Lost Files for.
// Shown: it has the file sent last on the screen (so Find Lost Files, and the tests, know it got it).
// Repair: a repaired copy of it, shown next when it's made.
// Map: show where the one shown was taken on a map (the viewer can't go online; the window asks).
enum class Ask : qint32 { Ready = 0, Previous, Next, Save, Open, Shown, Repair, Map };

// What the viewer asks to be done with enhance profiles. The viewer can't reach the home folder
// (it's sandboxed), so Find Lost Files keeps them: saves one, removes one, brings ones in (picked,
// or dropped on the viewer), and saves one where the person says.
enum class ProfileDo : qint32 { Save = 0, Remove, Import, Export, Dropped };

// The messages between them, each a small header and then its body.
struct Message {
    // Edited: the viewer's changes to the bytes of the one shown, to save as a copy.
    // SavePicture: a picture made in the viewer (one enhanced, a film's frame), to save where the
    // person says. Find Lost Files only writes its bytes, it never reads them.
    // ExportFilm: a copy of the film shown at another speed, with the light tools, made by ffmpeg.
    // Profiles: the saved enhance profiles (their files), sent at the start and after each change,
    // with a line saying what happened. Profile: what the viewer asks done with one (ProfileDo).
    enum Type : quint32 { Look = 1, ShowFile = 2, AskFor = 3, Facts = 4, Edited = 5, SavePicture = 6, ExportFilm = 7, Profiles = 8, Profile = 9 } type = ShowFile;
    QString style;    // Look: the widget style's name
    QPalette palette; // and the colours
    Show show;        // ShowFile; Facts: just show.facts, for the one shown
    Ask ask = Ask::Ready; // AskFor
    QMap<quint64, quint8> edits; // Edited: where, and what the byte there is now
    QString pictureName;  // SavePicture: the name to suggest
    QByteArray picture;   // and the picture, a PNG
    double speed = 1;     // ExportFilm: how fast, 0.1 to 2
    enhance::Settings settings;
    qint64 durationMs = 0;
    QList<QByteArray> profiles; // Profiles
    QString note;               // and what happened
    ProfileDo profileDo = ProfileDo::Save; // Profile
    QString profileName;
    QByteArray profile;         // Save, Export: its file
    QStringList dropped;        // Dropped: the files
};
QByteArray encode(const Message &message);
// One message off the front of `buffer` when a whole one is there (removed from it). False
// while it's still coming; `bad` when what's there isn't one.
bool decode(QByteArray &buffer, Message &message, bool *bad);

// The viewer program: diskanvil --viewer.
int run();
// How it's started: this program with --viewer, in a bubblewrap sandbox on Linux when `sandbox`
// and bwrap is there (not in the Flatpak, a sandbox already). The sandbox has the screen, the
// sound and the graphics card, no network, nothing of the home folder, and `readable`: the
// folders the copies to show are in.
QStringList command(bool sandbox, const QStringList &readable);

class PictureView;
class PdfView;
class MediaView;
class InsideView;
class EnhancePanel;

class Window : public QMainWindow
{
    Q_OBJECT
public:
    explicit Window(QWidget *parent = nullptr);
    ~Window() override;
    void present(const Show &show);
    void setFacts(const QStringList &facts);
    // The saved enhance profiles, and what was just done with them.
    void setProfiles(const QList<QByteArray> &files, const QString &note);
    // What's shown now (the tests look).
    Kind shownKind() const { return m_shown.kind; }
    QString problem() const; // why it can't be shown, when it can't
    QWidget *insideView() const; // the parts and the bytes (the tests look)

    EnhancePanel *enhancePanel() const { return m_enhance; } // the tests

Q_SIGNALS:
    void asked(viewer::Ask ask);
    void saveEdited(const QMap<quint64, quint8> &edits);
    void savePicture(const QString &name, const QByteArray &png);
    // A copy of the film shown at another speed, with the Enhance panel's light tools.
    void exportFilm(double speed, const enhance::Settings &settings, qint64 durationMs);
    void profileAsked(viewer::ProfileDo what, const QString &name, const QByteArray &profile, const QStringList &dropped);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void showFacts();
    // The Enhance panel's settings, on the picture shown: quickly on a smaller copy first, then the
    // whole of it. Saving does the whole one, made bigger when asked.
    void enhanceSoon();
    void enhanceNow();
    void saveEnhanced(int factor);
    // A spot: picking one, showing it (in the picture's pixels), and going back to the whole.
    void pickSpot(bool on);
    void showSpot(QRect spot);
    void showWhole();
    bool freezeFrame(); // a film's frame on the screen made a still picture to work on
    void straightenSpot(bool on);
    void straightened(const QPolygonF &corners);
    void stackFrames(int count, int scale);

    Show m_shown;
    QStackedWidget *m_pages;
    PictureView *m_picture;
    PdfView *m_pdf;
    MediaView *m_media;
    QPlainTextEdit *m_words;
    QPlainTextEdit *m_bytes;
    InsideView *m_inside;
    QWidget *m_normalPage = nullptr; // what shows when Inside isn't on
    QLabel *m_problem;
    QLabel *m_title;
    QLabel *m_facts;
    QScrollArea *m_side;
    QAction *m_previous, *m_next, *m_zoomOut, *m_fit, *m_actual, *m_zoomIn, *m_details, *m_open, *m_save;
    QAction *m_insideAction;
    QAction *m_repair;
    QAction *m_enhanceAction;
    EnhancePanel *m_enhance;
    QToolBar *m_bar = nullptr;
    int m_barWidth = 0;              // the toolbar with words, to know when only icons fit
    bool m_detailsForEnhance = false; // Details was put away to make room for Enhance
    QImage m_whole;             // while a spot is picked or shown: the whole picture (or the film's frame)
    QRect m_spot;               // the spot shown, in its pixels; empty for the whole
    bool m_spotFromFilm = false; // the film's frame is being worked on as a still
    QString m_spotWhen;         // and where in the film it is
    qint64 m_spotAt = 0;        // (in ms)
    bool m_reshaped = false;    // straightened or stacked: no longer a piece of m_whole as it is
    QStringList m_steps;        // what was done since, for the note in what's saved
    QTimer *m_enhanceTimer;
    std::shared_ptr<std::atomic<int>> m_enhanceRound; // a newer request stops the ones before it
};

} // namespace viewer
