// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Find Lost Files' viewer: a found file shown in full before it's saved. Pictures to zoom into,
// PDFs page by page, films and songs that play, documents' words, and the bytes of anything
// else. It runs as its own program (diskforge --viewer), in a sandbox on Linux, so a damaged
// file can't take DiskForge down. Find Lost Files hands it copies of found files (lookcopies.h)
// on its standard input, and it asks back on its standard output for the one before or after,
// or for this one to be saved or opened in its own program.

#include <QMainWindow>
#include <QMap>
#include <QPalette>
#include <QStringList>

class QIODevice;
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
};

// What the viewer asks Find Lost Files for.
// Shown: it has the file sent last on the screen (so Find Lost Files, and the tests, know it got it).
enum class Ask : qint32 { Ready = 0, Previous, Next, Save, Open, Shown };

// The messages between them, each a small header and then its body.
struct Message {
    // Edited: the viewer's changes to the bytes of the one shown, to save as a copy.
    enum Type : quint32 { Look = 1, ShowFile = 2, AskFor = 3, Facts = 4, Edited = 5 } type = ShowFile;
    QString style;    // Look: the widget style's name
    QPalette palette; // and the colours
    Show show;        // ShowFile; Facts: just show.facts, for the one shown
    Ask ask = Ask::Ready; // AskFor
    QMap<quint64, quint8> edits; // Edited: where, and what the byte there is now
};
QByteArray encode(const Message &message);
// One message off the front of `buffer` when a whole one is there (removed from it). False
// while it's still coming; `bad` when what's there isn't one.
bool decode(QByteArray &buffer, Message &message, bool *bad);

// The viewer program: diskforge --viewer.
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

class Window : public QMainWindow
{
    Q_OBJECT
public:
    explicit Window(QWidget *parent = nullptr);
    ~Window() override;
    void present(const Show &show);
    void setFacts(const QStringList &facts);
    // What's shown now (the tests look).
    Kind shownKind() const { return m_shown.kind; }
    QString problem() const; // why it can't be shown, when it can't
    QWidget *insideView() const; // the parts and the bytes (the tests look)

Q_SIGNALS:
    void asked(viewer::Ask ask);
    void saveEdited(const QMap<quint64, quint8> &edits);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void showFacts();

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
};

} // namespace viewer
