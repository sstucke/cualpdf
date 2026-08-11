#include "foldercontentmodel.h"

#include "pdfdocument.h"

#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QIcon>

FolderContentModel::FolderContentModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void FolderContentModel::setDirectory(const QString &path)
{
    beginResetModel();

    m_directory = path;
    m_entries.clear();

    const QDir dir(path);
    const QFileInfoList infos = dir.entryInfoList(
        {QStringLiteral("*.pdf")},
        QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot,
        QDir::Name | QDir::DirsFirst | QDir::IgnoreCase);

    m_entries.reserve(infos.size());
    for (const QFileInfo &info : infos) {
        Entry entry;
        entry.fileName = info.fileName();
        entry.absolutePath = info.absoluteFilePath();
        entry.isDir = info.isDir();
        m_entries.append(entry);
    }

    endResetModel();
}

void FolderContentModel::setThumbnailSize(int size)
{
    if (m_thumbnailSize == size)
        return;

    m_thumbnailSize = size;
    m_thumbnailCache.clear();
    m_failedThumbnails.clear();

    if (!m_entries.isEmpty())
        emit dataChanged(index(0), index(m_entries.size() - 1), {Qt::DecorationRole});
}

int FolderContentModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;

    return m_entries.size();
}

QVariant FolderContentModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};

    const Entry &entry = m_entries.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
        return entry.fileName;
    case Qt::DecorationRole: {
        if (!entry.isDir) {
            const QPixmap thumbnail = thumbnailFor(entry);
            if (!thumbnail.isNull())
                return QIcon(thumbnail);
        }
        static QFileIconProvider iconProvider;
        return iconProvider.icon(QFileInfo(entry.absolutePath));
    }
    case FilePathRole:
        return entry.absolutePath;
    case IsDirRole:
        return entry.isDir;
    default:
        return {};
    }
}

QPixmap FolderContentModel::thumbnailFor(const Entry &entry) const
{
    const auto cached = m_thumbnailCache.constFind(entry.absolutePath);
    if (cached != m_thumbnailCache.constEnd())
        return cached.value();

    if (m_failedThumbnails.contains(entry.absolutePath))
        return {};

    const PdfDocument document(entry.absolutePath);
    if (!document.isValid() || document.pageCount() <= 0) {
        m_failedThumbnails.insert(entry.absolutePath);
        return {};
    }

    const QImage image = document.renderPage(0, m_thumbnailSize);
    if (image.isNull()) {
        m_failedThumbnails.insert(entry.absolutePath);
        return {};
    }

    const QPixmap pixmap = QPixmap::fromImage(image);
    m_thumbnailCache.insert(entry.absolutePath, pixmap);
    return pixmap;
}
