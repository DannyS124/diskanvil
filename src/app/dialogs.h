// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "udisks.h"

#include <QDialog>

class EncryptionFields;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;

// Shared pieces for the dialogs here and in tools.cpp.
QString redText(const QString &text);
// The same in the theme's warning colour, for "this may not work" rather than "this erases".
QString warningText(const QString &text);
QLabel *wrappingLabel(const QString &html);
// Cancel stays the default; *action gets the button that accepts the dialog.
QDialogButtonBox *dialogButtons(QDialog *dialog, const QString &actionText, QPushButton **action);

// e.g. "RESCUE DISK (sdb1), 32.00 GB vfat"
QString describeVolume(const Volume &v);
// Non-empty for disks where changes have side effects (Ventoy).
QString diskWarning(const Disk &d);

// Message boxes for text with names from drives, files or add-ons in it. It's shown as
// plain text, so a name like "<b>" stays just that.
bool askPlain(QWidget *parent, const QString &title, const QString &text);
void warnPlain(QWidget *parent, const QString &title, const QString &text);

class FsPicker
{
public:
    FsPicker(const QVector<FsType> &filesystems, const Disk &disk, QComboBox *combo, QLineEdit *label);
    QString fsType() const;

private:
    void updateLabelLimit();
    QVector<FsType> m_filesystems;
    QComboBox *m_combo;
    QLineEdit *m_label;
};

class FormatDialog : public QDialog
{
    Q_OBJECT
public:
    FormatDialog(const Disk &disk, const Volume &volume, const QVector<FsType> &filesystems, QWidget *parent = nullptr);
    QString fsType() const;
    QString label() const;
    QString passphrase() const; // empty = not encrypted

private:
    QComboBox *m_fs;
    QLineEdit *m_label;
    FsPicker m_picker;
    EncryptionFields *m_encryption;
};

class NewPartitionDialog : public QDialog
{
    Q_OBJECT
public:
    NewPartitionDialog(const Disk &disk, const Span &free, const QVector<FsType> &filesystems, QWidget *parent = nullptr);
    quint64 sizeBytes() const;
    QString fsType() const;
    QString label() const;
    QString passphrase() const; // empty = not encrypted

    // Over-provisioning: on an SSD, the end of the drive left unused, as spare room for the
    // drive's own housekeeping. kSparePercent is the usual suggestion.
    static constexpr int kSparePercent = 10;

private:
    QSpinBox *m_size;
    QComboBox *m_fs;
    QLineEdit *m_label;
    FsPicker m_picker;
    EncryptionFields *m_encryption;
};

// Asks for the device name before wiping a non-empty disk.
class PartitionTableDialog : public QDialog
{
    Q_OBJECT
public:
    PartitionTableDialog(const Disk &disk, int diskNumber, QWidget *parent = nullptr);
    QString tableType() const;

private:
    QRadioButton *m_gpt;
    QLineEdit *m_confirm = nullptr;
    QPushButton *m_ok;
};

class ResizeDialog : public QDialog
{
    Q_OBJECT
public:
    // remountShrink/remountGrow: mount state has to change first
    ResizeDialog(const Disk &disk, const Volume &volume, const ResizeLimits &limits,
                 bool remountShrink, bool remountGrow, QWidget *parent = nullptr);
    quint64 newSize() const;

private:
    void update();

    Volume m_volume;
    bool m_remountShrink, m_remountGrow;
    QSpinBox *m_size;
    QSlider *m_slider;
    QLabel *m_effect;
    QPushButton *m_ok;
};
