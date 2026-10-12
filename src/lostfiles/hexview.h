// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A file's bytes, 16 to a line: where they are, in hex, and as text. One part of the file can be
// marked (the viewer's list of its parts picks it), and with editing on, bytes are changed by
// typing over them in either column. The file itself is never written: changes are kept here,
// shown in their own colour, and can be undone; what's saved is the file with them put in.

#include <QAbstractScrollArea>
#include <QByteArray>
#include <QMap>
#include <QVector>

class QFile;

class HexView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit HexView(QWidget *parent = nullptr);
    ~HexView() override;

    // The file to show (a copy made to look at): mapped, not read into memory, when it can be.
    bool open(const QString &path, QString *why);
    void close();
    quint64 size() const { return m_size; }
    const uchar *bytes() const { return m_data; }

    // Marks bytes [at, at + size) and brings them into view.
    void mark(quint64 at, quint64 size);
    void setEditing(bool on);
    bool editing() const { return m_editing; }
    // The changes, by where they are, and taking the last one back.
    const QMap<quint64, quint8> &edits() const { return m_edits; }
    void undo();
    quint64 position() const { return m_cursor; }
    void moveTo(quint64 at);

Q_SIGNALS:
    void editsChanged(int count);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    quint8 byteAt(quint64 at) const;
    void layoutScroll();
    void reveal(quint64 at);
    int lineHeight() const;
    // x where each column starts, for the current font.
    int hexX(int column) const;
    int textX(int column) const;
    void write(quint8 value);

    QFile *m_file = nullptr;
    QByteArray m_memory; // when mapping isn't possible
    const uchar *m_data = nullptr;
    quint64 m_size = 0;
    quint64 m_markAt = 0, m_markSize = 0;
    bool m_editing = false;
    bool m_inText = false; // typing goes into the text column
    bool m_lowNibble = false;
    quint64 m_cursor = 0;
    QMap<quint64, quint8> m_edits;
    struct Undo {
        quint64 at;
        bool had;
        quint8 was;
    };
    QVector<Undo> m_undo;
};
