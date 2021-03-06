#include "mmodel.h"
#include <mknown_apps.hpp>

#include <QTimer>
#include <QtConcurrentRun>
#include <QStandardPaths>
#include <QDirIterator>
#include <QSettings>
#include <QFileInfo>


qint64 getSize(const QString &path)
{
    qint64 res = 0;
    QFileInfo info(path);
    if (info.isDir())
    {
        QDirIterator it(path, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            it.next();
            res += it.fileInfo().size();
        }
    }
    else if (info.isFile())
    {
        res += info.size();
    }
    return res;
}

void processKnownPaths(QStringList &paths, qint64 &size, const QStringList &known_paths)
{
    for (const auto &p : known_paths)
    {
        if (QFileInfo(p).exists())
        {
            paths << p;
            size += getSize(p);
        }
    }
}


MModel::MModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_busy(false)
    , m_resetting(false)
    , m_unused_apps_count(0)
    , m_total_cache_size(0)
    , m_total_config_size(0)
    , m_total_localdata_size(0)
    , m_unused_config_size(0)
    , m_unused_cache_size(0)
    , m_unused_localdata_size(0)
{
    QTimer::singleShot(500, this, &MModel::reset);
}

bool MModel::busy() const
{
    return m_busy;
}

bool MModel::resetting() const
{
    return m_resetting;
}

qint64 MModel::totalConfigSize() const
{
    return m_total_config_size;
}

qint64 MModel::totalCacheSize() const
{
    return m_total_cache_size;
}

qint64 MModel::totalLocaldataSize() const
{
    return m_total_localdata_size;
}

int MModel::unusedAppsCount() const
{
    return m_unused_apps_count;
}

qint64 MModel::unusedConfigSize() const
{
    return m_unused_config_size;
}

qint64 MModel::unusedCacheSize() const
{
    return m_unused_cache_size;
}

qint64 MModel::unusedLocaldataSize() const
{
    return m_unused_localdata_size;
}

void MModel::reset()
{
    QtConcurrent::run(this, &MModel::resetImpl);
}

void MModel::deleteData(const QString &name, DataTypes types)
{
    QtConcurrent::run(this, &MModel::deleteDataImpl, name, types);
}

void MModel::deleteUnusedData(DataTypes types)
{
    QtConcurrent::run(this, &MModel::deleteUnusedDataImpl, types);
}

void MModel::setBusy(bool busy)
{
    m_busy = busy;
    emit this->busyChanged();
}

qint64 MModel::removePaths(const QStringList &paths)
{
    for (const auto &p : paths)
    {
        if (p.isEmpty())
        {
            qCritical("One of provided paths is empty");
            return 0;
        }
    }

    qint64 res = 0;
    for (const auto &p : paths)
    {
        auto size = getSize(p);

#ifndef SAFE_MODE
        QFileInfo info(p);
        bool ok = info.isDir()  ? QDir(p).removeRecursively() :
                  info.isFile() ? QFile::remove(p)            : false;

        if (ok)
        {
            qDebug("Deleted %lld bytes '%s'", size, qUtf8Printable(p));
            res += size;
        }
        else
        {
            qWarning("Error deleting '%s'", qUtf8Printable(p));
            emit this->deletionError(p);
        }
#else
        qDebug("SAFE MODE: Deleted %lld bytes '%s'", size, qUtf8Printable(p));
        res += size;
#endif
    }

    return res;
}

QVector<int> MModel::clearEntry(MEntry &entry, qint64 &deleted, DataTypes types)
{
    QVector<int> changed;

    if (types.testFlag(ConfigData) && entry.config_size > 0)
    {
        auto c = removePaths(entry.config_paths);
        if (c > 0)
        {
            entry.config_size = 0;
            deleted += c;
            changed << ConfigSizeRole;
        }
    }
    if (types.testFlag(CacheData) && entry.cache_size > 0)
    {
        auto c = removePaths(entry.cache_paths);
        if (c > 0)
        {
            entry.cache_size = 0;
            deleted += c;
            changed << CacheSizeRole;
        }
    }
    if (types.testFlag(LocalData) && entry.data_size > 0)
    {
        auto c = removePaths(entry.data_paths);
        if (c > 0)
        {
            entry.data_size = 0;
            deleted += c;
            changed << LocalDataSizeRole;
        }
    }

    return changed;
}

void MModel::resetImpl()
{
    this->setBusy(true);
    this->beginResetModel();
    m_resetting = true;
    emit this->resettingChanged();

    m_names.clear();
    m_entries.clear();

    // Process known apps
    for (const auto &app : knownApps())
    {
        MEntry e;
        processKnownPaths(e.config_paths, e.config_size, app.config);
        processKnownPaths(e.cache_paths, e.cache_size, app.cache);
        processKnownPaths(e.data_paths, e.data_size, app.local_data);
        if (e.exists())
        {
            qDebug("Found a known app '%s'", qUtf8Printable(app.name));
            m_names << app.name;
            m_entries.insert(app.name, e);
        }
    }

    // Search for other apps
    QStringList filters(QStringLiteral("harbour-*"));
    auto exclude = excludeDirs();
    auto check_excludes = !exclude.pattern().isEmpty();
    QStringList app_paths = {
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation),
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
    };

    for (int i = 0; i < 3; ++i)
    {
        QDirIterator it(app_paths[i], filters, QDir::Dirs | QDir::NoDotAndDotDot);
        while (it.hasNext())
        {
            auto dirpath = it.next();
            // Don't add paths of known apps
            if (check_excludes && exclude.match(dirpath).hasMatch())
            {
                continue;
            }
            auto size = getSize(dirpath);
            auto dirname = it.fileName();
            if (!m_entries.contains(dirname))
            {
                qDebug("Found a harbour app '%s'", qUtf8Printable(dirname));
                m_names << dirname;
            }
            auto &e = m_entries[dirname];
            switch (i)
            {
            case 0:
                e.config_paths << dirpath;
                e.config_size = size;
                break;
            case 1:
                e.cache_paths << dirpath;
                e.cache_size = size;
                break;
            case 2:
                e.data_paths << dirpath;
                e.data_size = size;
                break;
            default:
                Q_UNREACHABLE();
            }
        }
    }

    QString name_key(QStringLiteral("Desktop Entry/Name"));
    QString icon_key(QStringLiteral("Desktop Entry/Icon"));
    QString desktop_tmpl(QStringLiteral("%1/%2.desktop"));
    QStringList icon_tmpls = {
        QStringLiteral("/usr/share/icons/hicolor/86x86/apps/%1.png"),
        QStringLiteral("/usr/share/themes/sailfish-default/meegotouch/z1.0/icons/%1.png")
    };
    auto desktop_paths = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    for (const auto &name : m_entries.keys())
    {
        for (const auto &path : desktop_paths)
        {
            auto desktop_path = desktop_tmpl.arg(path, name);
            if (QFileInfo(desktop_path).isFile())
            {
                auto &e = m_entries[name];
                e.installed = true;
                QSettings desktop(desktop_path, QSettings::IniFormat);
                e.title = desktop.value(name_key).toString();
                auto icon_name = desktop.value(icon_key, name).toString();
                for (const auto &tmpl : icon_tmpls)
                {
                    auto icon = tmpl.arg(icon_name);
                    if (QFileInfo(icon).isFile())
                    {
                        e.icon = icon;
                        break;
                    }
                }
                break;
            }
        }
    }

    this->endResetModel();
    this->calculateTotal();
    this->setBusy(false);
    m_resetting = false;
    emit this->resettingChanged();
}

void MModel::deleteDataImpl(const QString &name, DataTypes types)
{
    if (!m_entries.contains(name))
    {
        qWarning("Model doesn't contain the '%s' entry", qUtf8Printable(name));
        return;
    }
    this->setBusy(true);

    qint64 deleted = 0;
    auto &e = m_entries[name];
    auto changed = clearEntry(e, deleted, types);
    int row = m_names.indexOf(name);

    if (!e.exists())
    {
        this->beginRemoveRows(QModelIndex(), row, row);
        m_names.removeOne(name);
        m_entries.remove(name);
        this->endRemoveRows();
    }
    else
    {
        auto ind = this->createIndex(row, 0);
        this->dataChanged(ind, ind, changed);
    }

    if (deleted > 0)
    {
        this->calculateTotal();
        emit this->dataDeleted(deleted);
    }
    this->setBusy(false);
}

void MModel::deleteUnusedDataImpl(DataTypes types)
{
    this->setBusy(true);

    qint64 deleted = 0;
    auto it = m_entries.begin();
    while (it != m_entries.end())
    {
        auto &e = it.value();
        if (e.installed)
        {
            ++it;
            continue;
        }

        auto changed = this->clearEntry(e, deleted, types);
        auto &name = it.key();
        int row = m_names.indexOf(name);

        if (!e.exists())
        {
            this->beginRemoveRows(QModelIndex(), row, row);
            m_names.removeOne(name);
            it = m_entries.erase(it);
            this->endRemoveRows();
        }
        else
        {
            auto ind = this->createIndex(row, 0);
            this->dataChanged(ind, ind, changed);
            ++it;
        }
    }

    if (deleted > 0)
    {
        this->calculateTotal();
        emit this->dataDeleted(deleted);
    }

    this->setBusy(false);
}

void MModel::calculateTotal()
{
    m_unused_apps_count = 0;
    m_total_localdata_size = 0;
    m_total_cache_size = 0;
    m_total_config_size = 0;
    m_unused_config_size = 0;
    m_unused_cache_size = 0;
    m_unused_localdata_size = 0;
    for (const auto &e : m_entries)
    {
        m_total_localdata_size += e.config_size;
        m_total_cache_size     += e.cache_size;
        m_total_config_size    += e.data_size;
        if (!e.installed)
        {
            ++m_unused_apps_count;
            m_unused_config_size    += e.config_size;
            m_unused_cache_size     += e.cache_size;
            m_unused_localdata_size += e.data_size;
        }
    }
    emit this->totalChanged();
}

int MModel::rowCount(const QModelIndex &parent) const
{
    return !parent.isValid() ? m_names.size() : 0;
}

QVariant MModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
    {
        return QVariant();
    }

    auto &name  = m_names[index.row()];
    auto &entry = m_entries[name];
    switch (role)
    {
    case NameRole:
        return name;
    case TitleRole:
        return entry.title.isEmpty() ? name : entry.title;
    case IconRole:
        return entry.icon;
    case InstalledRole:
        return entry.installed;
    case ConfigSizeRole:
        return entry.config_size;
    case CacheSizeRole:
        return entry.cache_size;
    case LocalDataSizeRole:
        return entry.data_size;
    case SortRole:
        return QString::number(int(!entry.installed))
                    .append(entry.title.isEmpty() ? name : entry.title);
    default:
        break;
    }

    return QVariant();
}

QHash<int, QByteArray> MModel::roleNames() const
{
    return {
        { NameRole,          "name" },
        { TitleRole,         "title" },
        { IconRole,          "icon" },
        { InstalledRole,     "installed" },
        { ConfigSizeRole,    "configSize" },
        { CacheSizeRole,     "cacheSize" },
        { LocalDataSizeRole, "localDataSize" }
    };
}
