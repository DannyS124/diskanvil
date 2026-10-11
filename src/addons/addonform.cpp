// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "addonform.h"

#include "dialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>

namespace {

QLabel *plain(const QString &text)
{
    auto *label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

} // namespace

AddonFormWidget::AddonFormWidget(const QVector<AddonField> &fields, const QMap<QString, QString> &values, QWidget *parent)
    : QWidget(parent)
{
    auto *form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    for (const AddonField &f : fields) {
        const QString value = values.value(f.id);
        QWidget *widget = nullptr;
        std::function<QString()> read;
        switch (f.type) {
        case AddonField::Type::Number: {
            auto *spin = new QSpinBox;
            spin->setRange(int(std::clamp<qint64>(f.min, 0, INT_MAX)), int(std::clamp<qint64>(f.max, 0, INT_MAX)));
            spin->setValue(value.toInt());
            widget = spin;
            read = [spin] { return QString::number(spin->value()); };
            break;
        }
        case AddonField::Type::Choice: {
            auto *combo = new QComboBox;
            for (const auto &choice : f.choices)
                combo->addItem(choice.first.isEmpty() ? choice.second : choice.first, choice.second);
            combo->setCurrentIndex(qMax(0, combo->findData(value)));
            widget = combo;
            read = [combo] { return combo->currentData().toString(); };
            break;
        }
        case AddonField::Type::Check: {
            auto *box = new QCheckBox;
            box->setChecked(!f.on.isEmpty() ? value == f.on : value != f.off);
            widget = box;
            const QString on = f.on, off = f.off;
            read = [box, on, off] { return box->isChecked() ? on : off; };
            break;
        }
        case AddonField::Type::Folder:
        case AddonField::Type::File: {
            auto *row = new QWidget;
            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            auto *edit = new QLineEdit(value);
            auto *browse = new QPushButton(tr("Browse…"));
            layout->addWidget(edit, 1);
            layout->addWidget(browse);
            const bool folder = f.type == AddonField::Type::Folder;
            connect(browse, &QPushButton::clicked, this, [this, edit, folder] {
                const QString start = edit->text().isEmpty() ? QDir::homePath() : edit->text();
                const QString picked = folder ? QFileDialog::getExistingDirectory(this, QString(), start)
                                              : QFileDialog::getOpenFileName(this, QString(), start);
                if (!picked.isEmpty())
                    edit->setText(picked);
            });
            widget = row;
            read = [edit] { return edit->text(); };
            break;
        }
        case AddonField::Type::Text: {
            auto *edit = new QLineEdit(value);
            widget = edit;
            read = [edit] { return edit->text(); };
            break;
        }
        }
        form->addRow(plain(f.label), widget);
        m_readers.push_back({f.id, read});
    }
}

QMap<QString, QString> AddonFormWidget::values() const
{
    QMap<QString, QString> out;
    for (const auto &[id, read] : m_readers)
        out.insert(id, read());
    return out;
}

AddonFormDialog::AddonFormDialog(const QString &title, const QString &intro, const QVector<AddonField> &fields,
                                 const QMap<QString, QString> &values, const QString &okText, QWidget *parent)
    : QDialog(parent)
    , m_form(new AddonFormWidget(fields, values))
{
    setWindowTitle(title);
    auto *layout = new QVBoxLayout(this);
    if (!intro.isEmpty())
        layout->addWidget(plain(intro));
    layout->addWidget(m_form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *ok = buttons->addButton(okText, QDialogButtonBox::AcceptRole);
    ok->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    setMinimumWidth(460);
}
