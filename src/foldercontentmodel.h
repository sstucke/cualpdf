#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QVector>

// Lists the subfolders and *.pdf files of a single directory (non-recursive).
// PDF thumbnails are rendered on demand, the first time a row's
// Qt::DecorationRole is actually requested by a view, and cached in memory
// for the lifetime of this model.
class FolderContentModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        FilePathRole = Qt::UserRole + 1,
        IsDirRole,
    };

    explicit FolderContentModel(QObject *parent = nullptr);

    void setDirectory(const QString &path);
    QString directory() const { return m_directory; }

    void setThumbnailSize(int size);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

private:
    struct Entry {
        QString fileName;
        QString absolutePath;
        bool isDir = false;
    };

    QPixmap thumbnailFor(const Entry &entry) const;

    QString m_directory;
    QVector<Entry> m_entries;
    int m_thumbnailSize = 96;
    mutable QHash<QString, QPixmap> m_thumbnailCache;
    mutable QSet<QString> m_failedThumbnails;
};
