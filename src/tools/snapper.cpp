// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapper.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QFile>

namespace snapper {

namespace {

const QString kService = QStringLiteral("org.opensuse.Snapper");
const QString kPath = QStringLiteral("/org/opensuse/Snapper");

QString unescape(const QByteArray &field)
{
    // mountinfo writes spaces, tabs, newlines and backslashes as octal escapes.
    QByteArray out;
    for (int i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size()) {
            bool ok = false;
            const int c = field.mid(i + 1, 3).toInt(&ok, 8);
            if (ok) {
                out += char(c);
                i += 3;
                continue;
            }
        }
        out += field[i];
    }
    return QString::fromLocal8Bit(out);
}

QMap<QString, QString> readStringMap(const QDBusArgument &arg)
{
    QMap<QString, QString> map;
    arg.beginMap();
    while (!arg.atEnd()) {
        QString key, value;
        arg.beginMapEntry();
        arg >> key >> value;
        arg.endMapEntry();
        map.insert(key, value);
    }
    arg.endMap();
    return map;
}

QDBusMessage callSnapper(const QString &method, const QVariantList &args, QString *error)
{
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kService, method);
    call.setArguments(args);
    const QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, 10000);
    if (reply.type() != QDBusMessage::ReplyMessage && error) {
        const QString name = reply.errorName();
        // snapper says "error.no_permissions" when the user isn't in ALLOW_USERS/ALLOW_GROUPS.
        *error = name.contains(QLatin1String("permission"))
                     ? QObject::tr("Snapper doesn't let you look. Add your user to ALLOW_USERS in its config (/etc/snapper/configs/).")
                     : reply.errorMessage().isEmpty() ? name : reply.errorMessage();
    }
    return reply;
}

} // namespace

QVector<Subvolume> mountedSubvolumes(const QByteArray &mountinfo)
{
    QByteArray text = mountinfo;
    if (text.isEmpty()) {
        QFile f(QStringLiteral("/proc/self/mountinfo"));
        if (f.open(QIODevice::ReadOnly))
            text = f.readAll();
    }
    QVector<Subvolume> list;
    for (const QByteArray &line : text.split('\n')) {
        // id parent major:minor root mountpoint options [optional...] - fstype source superoptions
        const QList<QByteArray> fields = line.split(' ');
        const int dash = fields.indexOf("-");
        if (dash < 6 || dash + 3 >= fields.size() || fields[dash + 1] != "btrfs")
            continue;
        Subvolume s;
        s.path = unescape(fields[3]);
        s.mountPoint = unescape(fields[4]);
        s.device = unescape(fields[dash + 2]);
        s.options = QString::fromLocal8Bit(fields[5] + ',' + fields[dash + 3]);
        for (const QString &o : s.options.split(QLatin1Char(','))) {
            if (o.startsWith(QLatin1String("subvolid=")))
                s.id = o.mid(9).toULongLong();
        }
        list.append(s);
    }
    return list;
}

bool available()
{
    const auto bus = QDBusConnection::systemBus().interface();
    if (!bus)
        return false;
    // snapperd starts on demand, so it counts if it can be activated.
    return bus->isServiceRegistered(kService) || bus->activatableServiceNames().value().contains(kService);
}

QVector<SnapperConfig> configs(QString *error)
{
    QVector<SnapperConfig> list;
    const QDBusMessage reply = callSnapper(QStringLiteral("ListConfigs"), {}, error);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return list;
    const QDBusArgument arg = reply.arguments().constFirst().value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        SnapperConfig c;
        arg.beginStructure();
        arg >> c.name >> c.subvolume;
        c.attributes = readStringMap(arg);
        arg.endStructure();
        list.append(c);
    }
    arg.endArray();
    return list;
}

QVector<Snapshot> snapshots(const QString &config, QString *error)
{
    QVector<Snapshot> list;
    const QDBusMessage reply = callSnapper(QStringLiteral("ListSnapshots"), {config}, error);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return list;
    const QDBusArgument arg = reply.arguments().constFirst().value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        Snapshot s;
        ushort type = 0;
        qlonglong date = 0;
        arg.beginStructure();
        arg >> s.number >> type >> s.preNumber >> date >> s.uid >> s.description >> s.cleanup;
        s.userdata = readStringMap(arg);
        arg.endStructure();
        s.type = type == 1 ? Snapshot::Type::Pre : type == 2 ? Snapshot::Type::Post : Snapshot::Type::Single;
        // Snapshot 0 is "current", the live system; its date is -1.
        s.date = date > 0 ? QDateTime::fromSecsSinceEpoch(date) : QDateTime();
        list.append(s);
    }
    arg.endArray();
    return list;
}

} // namespace snapper
