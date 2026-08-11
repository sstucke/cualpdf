#include "appsettings.h"

namespace {
constexpr auto kContentViewModeKey = "contentViewMode";
constexpr auto kRecentFilesKey = "recentFiles";
constexpr auto kDefaultContentViewMode = static_cast<int>(AppSettings::ContentViewMode::Thumbnails);
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
