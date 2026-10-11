// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "translations.h"

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

void installTranslations()
{
    auto *qt = new QTranslator(QCoreApplication::instance());
    if (qt->load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(qt);
    // Falls back to English, which still supplies the plural forms.
    auto *app = new QTranslator(QCoreApplication::instance());
    if (app->load(QLocale(), QStringLiteral("diskforge"), QStringLiteral("_"), QStringLiteral(":/i18n"))
        || app->load(QStringLiteral("diskforge_en"), QStringLiteral(":/i18n")))
        QCoreApplication::installTranslator(app);
}
