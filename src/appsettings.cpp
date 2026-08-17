#include "appsettings.h"

namespace {
constexpr auto kContentViewModeKey = "contentViewMode";
constexpr auto kRecentFilesKey = "recentFiles";
constexpr auto kFavoriteFoldersKey = "favoriteFolders";
constexpr auto kCreateTimestampedBackupsKey = "createTimestampedBackups";
constexpr auto kBackupVersionLimitKey = "backupVersionLimit";
constexpr auto kShowTipsAtStartupKey = "showTipsAtStartup";
constexpr auto kAutoCloseTipsKey = "autoCloseTips";
constexpr auto kDefaultContentViewMode = static_cast<int>(AppSettings::ContentViewMode::Thumbnails);
constexpr int kDefaultBackupVersionLimit = 5;
}

AppSettings::AppSettings() = default;

AppSettings::ContentViewMode AppSettings::contentViewMode() const
{
    const int stored = m_settings.value(kContentViewModeKey, kDefaultContentViewMode).toInt();
    if (stored < static_cast<int>(ContentViewMode::Thumbnails)
        || stored > static_cast<int>(ContentViewMode::CompactList))
        return ContentViewMode::Thumbnails;
    return static_cast<ContentViewMode>(stored);
}

void AppSettings::setContentViewMode(ContentViewMode mode)
{
    m_settings.setValue(kContentViewModeKey, static_cast<int>(mode));
}

QStringList AppSettings::recentFiles() const
{
    return m_settings.value(kRecentFilesKey).toStringList();
}

void AppSettings::setRecentFiles(const QStringList &files)
{
    m_settings.setValue(kRecentFilesKey, files);
}

QStringList AppSettings::favoriteFolders() const
{
    return m_settings.value(kFavoriteFoldersKey).toStringList();
}

void AppSettings::setFavoriteFolders(const QStringList &folders)
{
    m_settings.setValue(kFavoriteFoldersKey, folders);
}

bool AppSettings::createTimestampedBackups() const
{
    return m_settings.value(kCreateTimestampedBackupsKey, true).toBool();
}

void AppSettings::setCreateTimestampedBackups(bool enabled)
{
    m_settings.setValue(kCreateTimestampedBackupsKey, enabled);
}

int AppSettings::backupVersionLimit() const
{
    return qBound(1, m_settings.value(kBackupVersionLimitKey,
                                      kDefaultBackupVersionLimit).toInt(), 99);
}

void AppSettings::setBackupVersionLimit(int limit)
{
    m_settings.setValue(kBackupVersionLimitKey, qBound(1, limit, 99));
}

bool AppSettings::showTipsAtStartup() const
{
    return m_settings.value(kShowTipsAtStartupKey, true).toBool();
}

void AppSettings::setShowTipsAtStartup(bool enabled)
{
    m_settings.setValue(kShowTipsAtStartupKey, enabled);
}

bool AppSettings::autoCloseTips() const
{
    return m_settings.value(kAutoCloseTipsKey, true).toBool();
}

void AppSettings::setAutoCloseTips(bool enabled)
{
    m_settings.setValue(kAutoCloseTipsKey, enabled);
}
