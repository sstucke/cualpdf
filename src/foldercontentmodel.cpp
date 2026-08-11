#include "foldercontentmodel.h"

#include "pdfdocument.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QIcon>
#include <QLocale>
#include <QLoggingCategory>
#include <QPointer>
#include <QThread>

FolderContentModel::FolderContentModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void FolderContentModel::setDirectory(const QString &path)
{
    beginResetModel();

    m_directory = path;
    m_entries.clear();
    m_pdfInfoCache.clear();
    m_pendingPaths.clear();
    m_loadQueue.clear();

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
    m_pendingPaths.clear();
    m_loadQueue.clear();

    if (!m_entries.isEmpty())
        emit dataChanged(index(0), index(m_entries.size() - 1), {Qt::DecorationRole});
}

int FolderContentModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;

    return m_entries.size();
}

QVariant FolderContentModel::data(const QModelIndex &idx, int role) const
{
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_entries.size())
        return {};

    const Entry &entry = m_entries.at(idx.row());

    switch (role) {
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
        return entry.fileName;
    case Qt::DecorationRole: {
        if (!entry.isDir) {
            const auto cached = m_pdfInfoCache.constFind(entry.absolutePath);
            if (cached != m_pdfInfoCache.constEnd()) {
                if (!cached->thumbnail.isNull())
                    return QIcon(cached->thumbnail);
            } else {
                enqueueThumbnailLoad(entry);
            }
        }
        static QFileIconProvider iconProvider;
        return iconProvider.icon(QFileInfo(entry.absolutePath));
    }
    case FilePathRole:
        return entry.absolutePath;
    case IsDirRole:
        return entry.isDir;
    case MetadataRole: {
        if (entry.isDir)
            return QVariant();
        const auto cached = m_pdfInfoCache.constFind(entry.absolutePath);
        if (cached != m_pdfInfoCache.constEnd())
            return metadataFor(entry);
        return QVariant();
    }
    case PageCountRole: {
        if (entry.isDir)
            return QVariant();
        const auto cached = m_pdfInfoCache.constFind(entry.absolutePath);
        if (cached != m_pdfInfoCache.constEnd())
            return pagesTextFor(entry);
        return QVariant();
    }
    default:
        return {};
    }
}

void FolderContentModel::enqueueThumbnailLoad(const Entry &entry) const
{
    if (m_pendingPaths.contains(entry.absolutePath))
        return;

    m_pendingPaths.insert(entry.absolutePath);
    m_loadQueue.enqueue(entry);

    if (m_activeLoads < kMaxConcurrentLoads)
        startNextLoad();
}

void FolderContentModel::startNextLoad() const
{
    if (m_loadQueue.isEmpty())
        return;

    const Entry entry = m_loadQueue.dequeue();
    const QString path = entry.absolutePath;
    const int thumbSize = m_thumbnailSize;

    ++m_activeLoads;

    // Posting through qApp (always alive) rather than `this`, and checking
    // liveness via QPointer once back on the GUI thread: invokeMethod only
    // guarantees a previously-*posted* call won't fire after its context is
    // destroyed, not that *posting* a new one against an already-destroyed
    // context is safe — posting still has to read that context's thread
    // affinity, which is a use-after-free if `this` is gone by the time this
    // worker thread finishes. See PdfViewerWidget::startLoading() for the
    // same pattern (found via a real crash's backtrace).
    const QPointer<FolderContentModel> weakSelf(const_cast<FolderContentModel *>(this));
    QThread *thread = QThread::create([weakSelf, path, thumbSize]() {
        PdfInfo info;
        info.loaded = true;

        const PdfDocument document(path);
        if (document.isValid() && document.pageCount() > 0) {
            info.pageCount = document.pageCount();
            const QImage image = document.renderPage(0, thumbSize);
            if (!image.isNull())
                info.thumbnail = QPixmap::fromImage(image);
        } else {
            qWarning() << "Could not read PDF:" << path;
            info.failed = true;
        }

        QMetaObject::invokeMethod(
            qApp,
            [weakSelf, path, info]() {
                if (weakSelf)
                    weakSelf->onThumbnailLoaded(path, info);
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void FolderContentModel::onThumbnailLoaded(const QString &path, PdfInfo info)
{
    --m_activeLoads;
    m_pendingPaths.remove(path);

    // Find the row for this path — discard if directory changed while loading
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].absolutePath == path) {
            m_pdfInfoCache.insert(path, std::move(info));
            emit dataChanged(index(i), index(i), {Qt::DecorationRole, MetadataRole, PageCountRole});
            break;
        }
    }

    startNextLoad();
}

QString FolderContentModel::pagesTextFor(const Entry &entry) const
{
    const auto cached = m_pdfInfoCache.constFind(entry.absolutePath);
    const int pageCount = (cached != m_pdfInfoCache.constEnd()) ? cached->pageCount : -1;
    return pageCount >= 0 ? tr("Pages: %1").arg(pageCount) : tr("Pages: Unknown");
}

QString FolderContentModel::metadataFor(const Entry &entry) const
{
    const QString pages = pagesTextFor(entry);

    const QFileInfo fileInfo(entry.absolutePath);
    const QDateTime created = fileInfo.birthTime();
    const QString createdText =
        tr("Created: %1")
            .arg(created.isValid() ? QLocale().toString(created, QLocale::ShortFormat) : tr("Unknown"));
    const QString modifiedText =
        tr("Modified: %1").arg(QLocale().toString(fileInfo.lastModified(), QLocale::ShortFormat));

    return QStringLiteral("%1    %2    %3").arg(pages, createdText, modifiedText);
}
