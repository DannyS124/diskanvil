// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QPointer>
#include <QStringList>

class QLabel;
class QListWidget;
class QPushButton;
class QThread;
class PreviewLoader;
class ViewerHost;
namespace viewer {
enum class Ask : qint32;
}

// Look Closer: any picture or film on the PC (or a drive, or a stick) in the viewer, with all of
// its tools: Enhance and its profiles, Pick a Spot, Straighten, Stack Frames, a film at any
// speed. The same viewer as Find Lost Files, in its sandbox; the files are only read.
class LookCloserDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LookCloserDialog(QWidget *parent = nullptr);
    ~LookCloserDialog() override;

    // Files, or folders (their pictures and films, not those in folders inside them). Returns how
    // many were added; what isn't a picture or a film is left out.
    int add(const QStringList &paths);
    // Shows that one in the viewer.
    void look(int row);

    // The tests.
    int count() const { return int(m_files.size()); }
    ViewerHost *host() const { return m_host; }
    int shown() const { return m_shown; }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void asked(viewer::Ask ask);
    void factsFor(int row);
    QStringList readableFolders() const;

    QListWidget *m_list;
    QLabel *m_note;
    QPushButton *m_look;
    ViewerHost *m_host;
    QStringList m_files;
    QStringList m_baseFacts; // the shown one's size, folder and dates
    QStringList m_shownFacts; // and all it's known to say
    int m_shown = -1;
    QPointer<PreviewLoader> m_facts; // what the shown one says about itself, from the preview helper
    QPointer<QThread> m_factsThread;
};
