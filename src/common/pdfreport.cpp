// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pdfreport.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QTextDocument>

namespace pdfreport {

bool save(const QString &html, const QString &path, const QString &title, QString *error)
{
    QPdfWriter writer(path);
    writer.setTitle(title);
    writer.setCreator(QCoreApplication::applicationName() + QLatin1Char(' ') + QCoreApplication::applicationVersion());
    writer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Portrait, QMarginsF(18, 18, 18, 18), QPageLayout::Millimeter));
    writer.setResolution(96); // what the HTML's point sizes are laid out for (it's all vector anyway)
    QTextDocument doc;
    doc.setHtml(html);
    doc.setPageSize(writer.pageLayout().paintRectPixels(writer.resolution()).size());
    doc.print(&writer);
    if (QFileInfo(path).size() <= 0) {
        *error = QCoreApplication::translate("pdfreport", "It couldn't be written there.");
        return false;
    }
    return true;
}

bool saveAs(QWidget *parent, const QString &html, const QString &name, const QString &title)
{
    const QString path = QFileDialog::getSaveFileName(parent, title, QDir::home().filePath(name), QCoreApplication::translate("pdfreport", "PDF files (*.pdf)"));
    if (path.isEmpty())
        return false;
    QString error;
    if (!save(html, path, title, &error)) {
        QMessageBox::warning(parent, title, error);
        return false;
    }
    return true;
}

} // namespace pdfreport
