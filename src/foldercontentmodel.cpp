#include "foldercontentmodel.h"

#include "pdfdocument.h"

#include <QDateTime>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QIcon>
#include <QLocale>

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
    m_pdfInfoCache.clear();

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
    case Qt::ToolTipRole:
        return entry.fileName;
    case Qt::DecorationRole: {
        if (!entry.isDir) {
            const PdfInfo &info = pdfInfoFor(entry);
            if (!info.thumbnail.isNull())
                return QIcon(info.thumbnail);
        }
        static QFileIconProvider iconProvider;
        return iconProvider.icon(QFileInfo(entry.absolutePath));
    }
    case FilePathRole:
        return entry.absolutePath;
    case IsDirRole:
        return entry.isDir;
    case MetadataRole:
        return entry.isDir ? QVariant() : QVariant(metadataFor(entry));
    default:
        return {};
    }
}

const FolderContentModel::PdfInfo &FolderContentModel::pdfInfoFor(const Entry &entry) const
{
    const auto cached = m_pdfInfoCache.constFind(entry.absolutePath);
    if (cached != m_pdfInfoCache.constEnd())
        return cached.value();

    PdfInfo info;
    const PdfDocument document(entry.absolutePath);
    if (document.isValid() && document.pageCount() > 0) {
        info.pageCount = document.pageCount();
        const QImage image = document.renderPage(0, m_thumbnailSize);
        if (!image.isNull())
            info.thumbnail = QPixmap::fromImage(image);
    } else {
        info.failed = true;
    }

    return m_pdfInfoCache.insert(entry.absolutePath, info).value();
}

QString FolderContentModel::metadataFor(const Entry &entry) const
{
    const PdfInfo &info = pdfInfoFor(entry);
    const QString pages =
        info.pageCount >= 0 ? tr("Pages: %1").arg(info.pageCount) : tr("Pages: Unknown");

    const QFileInfo fileInfo(entry.absolutePath);
    const QDateTime created = fileInfo.birthTime();
    const QString createdText =
        tr("Created: %1")
            .arg(created.isValid() ? QLocale().toString(created, QLocale::ShortFormat) : tr("Unknown"));
    const QString modifiedText =
        tr("Modified: %1").arg(QLocale().toString(fileInfo.lastModified(), QLocale::ShortFormat));

    return QStringLiteral("%1    %2    %3").arg(pages, createdText, modifiedText);
}
