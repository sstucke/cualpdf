#pragma once

#include <QSettings>
#include <QStringList>

// Single place where cualpdf reads/writes persisted user preferences
// (backed by QSettings, so the actual storage location/format follows Qt's
// per-platform convention). Add new preferences here as they come up rather
// than reaching for QSettings directly elsewhere.
class AppSettings
{
public:
    enum class ContentViewMode {
        Thumbnails,
        Details,
        CompactList,
    };

    AppSettings();

    ContentViewMode contentViewMode() const;
    void setContentViewMode(ContentViewMode mode);

    QStringList recentFiles() const;
    void setRecentFiles(const QStringList &files);

private:
    QSettings m_settings;
};
