#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QPixmap>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QVector>

// Lists the subfolders and *.pdf files of a single directory (non-recursive).
// PDF thumbnails and page counts are loaded asynchronously in background
// threads and cached for the lifetime of this model. dataChanged is emitted
// for each entry as its thumbnail becomes available.
class FolderContentModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum class SortMode {
        NameAscending,
        NameDescending,
        SizeAscending,
        SizeDescending,
        DateAscending,
        DateDescending,
    };

    enum Roles {
        FilePathRole = Qt::UserRole + 1,
        IsDirRole,
        MetadataRole,
        PageCountRole,
    };

    explicit FolderContentModel(QObject *parent = nullptr);

    void setDirectory(const QString &path);
    QString directory() const { return m_directory; }

    void setThumbnailSize(int size);
    void setSortMode(SortMode mode);
    void setFoldersFirst(bool enabled);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

private:
    struct Entry {
        QString fileName;
        QString absolutePath;
        bool isDir = false;
        qint64 size = 0;
        QDateTime lastModified;
    };

    struct PdfInfo {
        QPixmap thumbnail;
        int pageCount = -1;
        bool failed = false;
        bool loaded = false;
    };

    struct LoadRequest {
        Entry entry;
        quint64 generation = 0;
    };

    void enqueueThumbnailLoad(const Entry &entry) const;
    void startNextLoad() const;
    void onThumbnailLoaded(const QString &path, quint64 generation, PdfInfo info);
    QString metadataFor(const Entry &entry) const;
    QString pagesTextFor(const Entry &entry) const;
    void sortEntries();

    QString m_directory;
    QVector<Entry> m_entries;
    int m_thumbnailSize = 96;
    SortMode m_sortMode = SortMode::NameAscending;
    bool m_foldersFirst = true;

    mutable QHash<QString, PdfInfo> m_pdfInfoCache;
    mutable QSet<QString> m_pendingPaths;
    mutable QQueue<LoadRequest> m_loadQueue;
    mutable int m_activeLoads = 0;
    quint64 m_generation = 0;

    static constexpr int kMaxConcurrentLoads = 2;
};
