#include "pdfdocument.h"

#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_transformpage.h>

#include <QByteArray>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

namespace {
bool g_libraryInitialized = false;

// PDFium is not safe to call concurrently from multiple threads. Callers
// (FolderContentModel on the GUI thread, PdfViewerWidget's worker threads)
// may legitimately overlap, so every FPDF_* call in this file is serialized
// through this single global lock rather than assuming a single-thread
// caller. This does not make loading/rendering parallel — only safe to call
// from a background thread instead of blocking the GUI thread.
QMutex &pdfiumMutex()
{
    static QMutex mutex;
    return mutex;
}

struct PdfFileWriter {
    FPDF_FILEWRITE fileWrite = {};
    QIODevice *device = nullptr;
};

int writePdfBlock(FPDF_FILEWRITE *fileWrite, const void *data, unsigned long size)
{
    auto *writer = reinterpret_cast<PdfFileWriter *>(fileWrite);
    return writer->device
               && writer->device->write(static_cast<const char *>(data),
                                        static_cast<qint64>(size)) == static_cast<qint64>(size);
}

QByteArray serializeDocument(FPDF_DOCUMENT document)
{
    QByteArray data;
    QBuffer buffer(&data);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};

    PdfFileWriter writer;
    writer.fileWrite.version = 1;
    writer.fileWrite.WriteBlock = &writePdfBlock;
    writer.device = &buffer;
    if (!FPDF_SaveAsCopy(document, &writer.fileWrite, FPDF_NO_INCREMENTAL))
        return {};
    return data;
}

bool hasUniqueIds(const QVector<quint64> &ids)
{
    QSet<quint64> uniqueIds;
    uniqueIds.reserve(ids.size());
    for (const quint64 id : ids) {
        if (id == 0 || uniqueIds.contains(id))
            return false;
        uniqueIds.insert(id);
    }
    return true;
}

bool backupNameFits(const QString &directoryPath, const QString &fileName)
{
    // 240 UTF-8 bytes leaves room below the usual 255-byte component limit.
    if (fileName.toUtf8().size() > 240)
        return false;
#ifdef Q_OS_WIN
    // Stay below the legacy Windows path boundary for installations where
    // long-path support is not enabled.
    if (QDir(directoryPath).absoluteFilePath(fileName).size() >= 248)
        return false;
#endif
    return true;
}

QString timestampedBackupPath(const QString &filePath, QString *backupPrefix)
{
    const QFileInfo sourceInfo(filePath);
    const QString directoryPath = sourceInfo.absolutePath();
    const QString extension = sourceInfo.suffix().isEmpty()
                                  ? QStringLiteral("pdf")
                                  : sourceInfo.suffix();
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss"));

    for (int collision = 0; collision < 100; ++collision) {
        const QString collisionSuffix = collision == 0
                                            ? QString()
                                            : QStringLiteral("_%1").arg(collision, 2, 10, QLatin1Char('0'));
        QString stem = sourceInfo.completeBaseName();
        QString candidate;
        do {
            candidate = QStringLiteral("%1_%2%3.%4")
                            .arg(stem, timestamp, collisionSuffix, extension);
            if (backupNameFits(directoryPath, candidate))
                break;
            if (stem.isEmpty())
                return {};
            stem.chop(1);
            if (!stem.isEmpty() && stem.back().isHighSurrogate())
                stem.chop(1);
        } while (true);

        const QString path = QDir(directoryPath).filePath(candidate);
        if (!QFileInfo::exists(path)) {
            if (backupPrefix)
                *backupPrefix = stem;
            return path;
        }
    }
    return {};
}

void pruneTimestampedBackups(const QString &filePath, const QString &backupPrefix,
                             int versionLimit)
{
    if (backupPrefix.isEmpty() || versionLimit < 1)
        return;

    const QFileInfo sourceInfo(filePath);
    const QString extension = sourceInfo.suffix().isEmpty()
                                  ? QStringLiteral("pdf")
                                  : sourceInfo.suffix();
    const QRegularExpression pattern(
        QStringLiteral("^%1_\\d{14}(?:_\\d{2})?\\.%2$")
            .arg(QRegularExpression::escape(backupPrefix),
                 QRegularExpression::escape(extension)),
        QRegularExpression::CaseInsensitiveOption);

    QFileInfoList matchingBackups;
    const QDir directory(sourceInfo.absolutePath());
    for (const QFileInfo &entry : directory.entryInfoList(QDir::Files | QDir::Readable)) {
        if (pattern.match(entry.fileName()).hasMatch())
            matchingBackups.append(entry);
    }
    std::sort(matchingBackups.begin(), matchingBackups.end(),
              [](const QFileInfo &left, const QFileInfo &right) {
                  return left.fileName() > right.fileName();
              });
    for (int index = versionLimit; index < matchingBackups.size(); ++index) {
        if (!QFile::remove(matchingBackups[index].absoluteFilePath()))
            qWarning() << "Could not remove old PDF backup:"
                       << matchingBackups[index].absoluteFilePath();
    }
}
}

void PdfDocument::initializeLibrary()
{
    const QMutexLocker locker(&pdfiumMutex());
    if (g_libraryInitialized)
        return;

    FPDF_LIBRARY_CONFIG config = {};
    config.version = 2;
    config.m_pUserFontPaths = nullptr;
    config.m_pIsolate = nullptr;
    config.m_v8EmbedderSlot = 0;
    FPDF_InitLibraryWithConfig(&config);
    g_libraryInitialized = true;
}

void PdfDocument::shutdownLibrary()
{
    const QMutexLocker locker(&pdfiumMutex());
    if (!g_libraryInitialized)
        return;

    FPDF_DestroyLibrary();
    g_libraryInitialized = false;
}

PdfDocument::PdfDocument(const QString &filePath)
{
    const QByteArray path = filePath.toUtf8();
    const QMutexLocker locker(&pdfiumMutex());
    m_document = FPDF_LoadDocument(path.constData(), nullptr);
    if (!m_document)
        qWarning() << "PDFium failed to load:" << filePath
                   << "(error" << FPDF_GetLastError() << ")";
}

PdfDocument::~PdfDocument()
{
    const QMutexLocker locker(&pdfiumMutex());
    if (m_document)
        FPDF_CloseDocument(static_cast<FPDF_DOCUMENT>(m_document));
}

int PdfDocument::pageCount() const
{
    if (!m_document)
        return 0;

    const QMutexLocker locker(&pdfiumMutex());
    return FPDF_GetPageCount(static_cast<FPDF_DOCUMENT>(m_document));
}

QSizeF PdfDocument::pageSizePoints(int pageIndex) const
{
    if (!m_document)
        return {};

    const QMutexLocker locker(&pdfiumMutex());
    const FPDF_PAGE page = FPDF_LoadPage(static_cast<FPDF_DOCUMENT>(m_document), pageIndex);
    if (!page)
        return {};

    const QSizeF size(FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page));
    FPDF_ClosePage(page);
    return size;
}

QVector<QSizeF> PdfDocument::allPageSizes() const
{
    if (!m_document)
        return {};

    const QMutexLocker locker(&pdfiumMutex());
    const auto doc = static_cast<FPDF_DOCUMENT>(m_document);
    const int count = FPDF_GetPageCount(doc);

    QVector<QSizeF> sizes;
    sizes.reserve(count);
    for (int i = 0; i < count; ++i) {
        const FPDF_PAGE page = FPDF_LoadPage(doc, i);
        if (!page) {
            sizes.append(QSizeF(595, 842)); // A4 fallback
            continue;
        }

        const QSizeF size(FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page));
        FPDF_ClosePage(page);
        sizes.append(size.width() > 0.0 && size.height() > 0.0
                         ? size
                         : QSizeF(595, 842));
    }
    return sizes;
}

QImage PdfDocument::renderPage(int pageIndex, int targetWidthPx) const
{
    if (!m_document || targetWidthPx <= 0)
        return {};

    const QMutexLocker locker(&pdfiumMutex());
    const auto document = static_cast<FPDF_DOCUMENT>(m_document);
    const FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
    if (!page) {
        qWarning() << "PDFium failed to load page" << pageIndex
                   << "(error" << FPDF_GetLastError() << ")";
        return {};
    }

    const double pageWidthPt = FPDF_GetPageWidthF(page);
    const double pageHeightPt = FPDF_GetPageHeightF(page);
    if (pageWidthPt <= 0.0 || pageHeightPt <= 0.0) {
        FPDF_ClosePage(page);
        return {};
    }

    const int width = targetWidthPx;
    const int height = qMax(1, qRound(targetWidthPx * pageHeightPt / pageWidthPt));

    const FPDF_BITMAP bitmap = FPDFBitmap_Create(width, height, /*alpha=*/0);
    if (!bitmap) {
        FPDF_ClosePage(page);
        return {};
    }

    FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, /*rotate=*/0, /*flags=*/0);

    const auto *buffer = static_cast<const uchar *>(FPDFBitmap_GetBuffer(bitmap));
    const int stride = FPDFBitmap_GetStride(bitmap);

    // FPDFBitmap_Create with alpha=0 yields BGRx byte order, which matches
    // QImage::Format_RGB32's in-memory layout on little-endian hosts.
    const QImage rendered(buffer, width, height, stride, QImage::Format_RGB32);
    const QImage copy = rendered.copy();

    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);

    return copy;
}

bool PdfDocument::rotatePages(const QVector<int> &pageIndexes, bool clockwise)
{
    if (!m_document || pageIndexes.isEmpty())
        return false;

    const QMutexLocker locker(&pdfiumMutex());
    const auto document = static_cast<FPDF_DOCUMENT>(m_document);
    const int count = FPDF_GetPageCount(document);
    bool transformed = false;

    for (const int pageIndex : pageIndexes) {
        if (pageIndex < 0 || pageIndex >= count)
            continue;

        const FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
        if (!page)
            continue;

        const int rotation = FPDFPage_GetRotation(page);
        if (rotation >= 0) {
            FPDFPage_SetRotation(page, (rotation + (clockwise ? 1 : 3)) % 4);
            transformed = true;
        }
        FPDF_ClosePage(page);
    }
    return transformed;
}

bool PdfDocument::cropPages(const QVector<int> &pageIndexes,
                            const QMarginsF &marginsPoints)
{
    if (!m_document || pageIndexes.isEmpty() || marginsPoints.left() < 0.0
        || marginsPoints.top() < 0.0 || marginsPoints.right() < 0.0
        || marginsPoints.bottom() < 0.0) {
        return false;
    }

    const QMutexLocker locker(&pdfiumMutex());
    const auto document = static_cast<FPDF_DOCUMENT>(m_document);
    const int pageCount = FPDF_GetPageCount(document);
    bool transformed = false;

    for (const int pageIndex : pageIndexes) {
        if (pageIndex < 0 || pageIndex >= pageCount)
            continue;

        const FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
        if (!page)
            continue;

        const double pageWidth = FPDF_GetPageWidthF(page);
        const double pageHeight = FPDF_GetPageHeightF(page);
        const double croppedWidth = pageWidth - marginsPoints.left() - marginsPoints.right();
        const double croppedHeight = pageHeight - marginsPoints.top() - marginsPoints.bottom();
        if (pageWidth <= 0.0 || pageHeight <= 0.0 || croppedWidth <= 0.01
            || croppedHeight <= 0.01) {
            FPDF_ClosePage(page);
            continue;
        }

        // Use the same aspect ratio and rotation argument as renderPage().
        // DeviceToPage accounts for an existing CropBox and page rotation.
        constexpr int deviceWidth = 100000;
        const int deviceHeight = qMax(1, qRound(deviceWidth * pageHeight / pageWidth));
        const int deviceLeft = qRound(deviceWidth * marginsPoints.left() / pageWidth);
        const int deviceRight = qRound(deviceWidth * (1.0 - marginsPoints.right() / pageWidth));
        const int deviceTop = qRound(deviceHeight * marginsPoints.top() / pageHeight);
        const int deviceBottom =
            qRound(deviceHeight * (1.0 - marginsPoints.bottom() / pageHeight));

        double firstX = 0.0;
        double firstY = 0.0;
        double secondX = 0.0;
        double secondY = 0.0;
        const bool firstConverted = FPDF_DeviceToPage(
            page, 0, 0, deviceWidth, deviceHeight, 0,
            deviceLeft, deviceTop, &firstX, &firstY);
        const bool secondConverted = FPDF_DeviceToPage(
            page, 0, 0, deviceWidth, deviceHeight, 0,
            deviceRight, deviceBottom, &secondX, &secondY);

        if (firstConverted && secondConverted) {
            const float left = static_cast<float>(qMin(firstX, secondX));
            const float right = static_cast<float>(qMax(firstX, secondX));
            const float bottom = static_cast<float>(qMin(firstY, secondY));
            const float top = static_cast<float>(qMax(firstY, secondY));
            if (right - left > 0.01f && top - bottom > 0.01f) {
                FPDFPage_SetCropBox(page, left, bottom, right, top);
                transformed = true;
            }
        }
        FPDF_ClosePage(page);
    }
    return transformed;
}

QVector<PdfPageState> PdfDocument::pageStates(const QVector<int> &pageIndexes) const
{
    if (!m_document || pageIndexes.isEmpty())
        return {};

    const QMutexLocker locker(&pdfiumMutex());
    const auto document = static_cast<FPDF_DOCUMENT>(m_document);
    const int pageCount = FPDF_GetPageCount(document);
    QVector<PdfPageState> states;
    states.reserve(pageIndexes.size());

    for (const int pageIndex : pageIndexes) {
        if (pageIndex < 0 || pageIndex >= pageCount)
            return {};

        const FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
        if (!page)
            return {};

        const int rotation = FPDFPage_GetRotation(page);
        float left = 0.0f;
        float bottom = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        bool hasBox = FPDFPage_GetCropBox(page, &left, &bottom, &right, &top);
        if (!hasBox)
            hasBox = FPDFPage_GetMediaBox(page, &left, &bottom, &right, &top);
        FPDF_ClosePage(page);

        if (rotation < 0 || !hasBox || right <= left || top <= bottom)
            return {};
        states.append({pageIndex, rotation, left, bottom, right, top});
    }
    return states;
}

bool PdfDocument::restorePageStates(const QVector<PdfPageState> &states)
{
    if (!m_document || states.isEmpty())
        return false;

    const QMutexLocker locker(&pdfiumMutex());
    const auto document = static_cast<FPDF_DOCUMENT>(m_document);
    const int pageCount = FPDF_GetPageCount(document);

    for (const PdfPageState &state : states) {
        if (state.pageIndex < 0 || state.pageIndex >= pageCount
            || state.rotation < 0 || state.rotation > 3
            || state.cropRight <= state.cropLeft || state.cropTop <= state.cropBottom) {
            return false;
        }

        const FPDF_PAGE page = FPDF_LoadPage(document, state.pageIndex);
        if (!page)
            return false;
        FPDFPage_SetRotation(page, state.rotation);
        FPDFPage_SetCropBox(page,
                            static_cast<float>(state.cropLeft),
                            static_cast<float>(state.cropBottom),
                            static_cast<float>(state.cropRight),
                            static_cast<float>(state.cropTop));
        FPDF_ClosePage(page);
    }
    return true;
}

QByteArray PdfDocument::exportPages(const QVector<int> &pageIndexes) const
{
    if (!m_document || pageIndexes.isEmpty())
        return {};

    const QMutexLocker locker(&pdfiumMutex());
    const auto sourceDocument = static_cast<FPDF_DOCUMENT>(m_document);
    const int pageCount = FPDF_GetPageCount(sourceDocument);
    QSet<int> uniqueIndexes;
    for (const int pageIndex : pageIndexes) {
        if (pageIndex < 0 || pageIndex >= pageCount || uniqueIndexes.contains(pageIndex))
            return {};
        uniqueIndexes.insert(pageIndex);
    }

    const FPDF_DOCUMENT archiveDocument = FPDF_CreateNewDocument();
    if (!archiveDocument)
        return {};

    const bool imported = FPDF_ImportPagesByIndex(
        archiveDocument, sourceDocument, pageIndexes.constData(),
        static_cast<unsigned long>(pageIndexes.size()), 0);
    const QByteArray archive = imported ? serializeDocument(archiveDocument) : QByteArray();
    FPDF_CloseDocument(archiveDocument);
    return archive;
}

QByteArray PdfDocument::createBlankPageArchive(const QSizeF &pageSize)
{
    if (pageSize.width() <= 0.0 || pageSize.height() <= 0.0)
        return {};

    const QMutexLocker locker(&pdfiumMutex());
    const FPDF_DOCUMENT archiveDocument = FPDF_CreateNewDocument();
    if (!archiveDocument)
        return {};

    const FPDF_PAGE page = FPDFPage_New(archiveDocument, 0,
                                        pageSize.width(), pageSize.height());
    QByteArray archive;
    if (page) {
        const bool generated = FPDFPage_GenerateContent(page);
        FPDF_ClosePage(page);
        if (generated)
            archive = serializeDocument(archiveDocument);
    }
    FPDF_CloseDocument(archiveDocument);
    return archive;
}

bool PdfDocument::mergeFiles(const QStringList &inputPaths, const QString &outputPath,
                             QString *failedInputPath, QString *fileErrorMessage)
{
    if (failedInputPath)
        failedInputPath->clear();
    if (fileErrorMessage)
        fileErrorMessage->clear();
    if (inputPaths.size() < 2 || outputPath.isEmpty())
        return false;

    QSaveFile output(outputPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        if (fileErrorMessage)
            *fileErrorMessage = output.errorString();
        return false;
    }

    bool success = true;
    {
        const QMutexLocker locker(&pdfiumMutex());
        const FPDF_DOCUMENT mergedDocument = FPDF_CreateNewDocument();
        if (!mergedDocument) {
            success = false;
        } else {
            int insertionIndex = 0;
            for (const QString &inputPath : inputPaths) {
                const QByteArray pathBytes = inputPath.toUtf8();
                const FPDF_DOCUMENT inputDocument =
                    FPDF_LoadDocument(pathBytes.constData(), nullptr);
                if (!inputDocument || FPDF_GetPageCount(inputDocument) <= 0) {
                    if (failedInputPath)
                        *failedInputPath = inputPath;
                    if (inputDocument)
                        FPDF_CloseDocument(inputDocument);
                    success = false;
                    break;
                }

                const int inputPageCount = FPDF_GetPageCount(inputDocument);
                success = FPDF_ImportPages(
                    mergedDocument, inputDocument, nullptr, insertionIndex);
                FPDF_CloseDocument(inputDocument);
                if (!success) {
                    if (failedInputPath)
                        *failedInputPath = inputPath;
                    break;
                }
                insertionIndex += inputPageCount;
            }

            if (success) {
                PdfFileWriter writer;
                writer.fileWrite.version = 1;
                writer.fileWrite.WriteBlock = &writePdfBlock;
                writer.device = &output;
                success = FPDF_SaveAsCopy(
                    mergedDocument, &writer.fileWrite, FPDF_NO_INCREMENTAL);
            }
            FPDF_CloseDocument(mergedDocument);
        }
    }

    if (!success || !output.flush()) {
        output.cancelWriting();
        if (fileErrorMessage && !output.errorString().isEmpty())
            *fileErrorMessage = output.errorString();
        return false;
    }
    if (!output.commit()) {
        if (fileErrorMessage)
            *fileErrorMessage = output.errorString();
        return false;
    }
    return true;
}

bool PdfDocument::restorePageStructure(const QVector<quint64> &currentPageIds,
                                       const QVector<quint64> &targetPageIds,
                                       const QVector<quint64> &archivedPageIds,
                                       const QByteArray &pageArchive)
{
    if (!m_document || targetPageIds.isEmpty()
        || !hasUniqueIds(currentPageIds) || !hasUniqueIds(targetPageIds)
        || (!archivedPageIds.isEmpty() && !hasUniqueIds(archivedPageIds))) {
        return false;
    }

    const QSet<quint64> currentIdSet(currentPageIds.cbegin(), currentPageIds.cend());
    const QSet<quint64> archivedIdSet(archivedPageIds.cbegin(), archivedPageIds.cend());
    for (const quint64 targetId : targetPageIds) {
        if (!currentIdSet.contains(targetId) && !archivedIdSet.contains(targetId))
            return false;
    }

    const QMutexLocker locker(&pdfiumMutex());
    auto document = static_cast<FPDF_DOCUMENT>(m_document);
    if (FPDF_GetPageCount(document) != currentPageIds.size())
        return false;

    FPDF_DOCUMENT archiveDocument = nullptr;
    if (!archivedPageIds.isEmpty()) {
        if (pageArchive.isEmpty())
            return false;
        archiveDocument = FPDF_LoadMemDocument64(
            pageArchive.constData(), static_cast<size_t>(pageArchive.size()), nullptr);
        if (!archiveDocument
            || FPDF_GetPageCount(archiveDocument) != archivedPageIds.size()) {
            if (archiveDocument)
                FPDF_CloseDocument(archiveDocument);
            return false;
        }
    }

    const QByteArray rollbackData = serializeDocument(document);
    if (rollbackData.isEmpty()) {
        if (archiveDocument)
            FPDF_CloseDocument(archiveDocument);
        return false;
    }

    QVector<quint64> workingIds = currentPageIds;
    const QSet<quint64> targetIdSet(targetPageIds.cbegin(), targetPageIds.cend());
    bool success = true;
    for (int index = workingIds.size() - 1; index >= 0; --index) {
        if (!targetIdSet.contains(workingIds.at(index))) {
            FPDFPage_Delete(document, index);
            workingIds.removeAt(index);
        }
    }

    for (int targetIndex = 0; success && targetIndex < targetPageIds.size(); ++targetIndex) {
        const quint64 targetId = targetPageIds.at(targetIndex);
        if (workingIds.contains(targetId))
            continue;

        const int archiveIndex = archivedPageIds.indexOf(targetId);
        success = archiveDocument && archiveIndex >= 0;
        if (success) {
            const int sourceIndex = archiveIndex;
            success = FPDF_ImportPagesByIndex(document, archiveDocument, &sourceIndex, 1,
                                              targetIndex);
        }
        if (success)
            workingIds.insert(targetIndex, targetId);
    }

    if (success && workingIds != targetPageIds) {
        QVector<int> targetIndexes;
        targetIndexes.reserve(targetPageIds.size());
        for (const quint64 targetId : targetPageIds) {
            const int currentIndex = workingIds.indexOf(targetId);
            if (currentIndex < 0) {
                success = false;
                break;
            }
            targetIndexes.append(currentIndex);
        }
        if (success) {
            success = FPDF_MovePages(document, targetIndexes.constData(),
                                     static_cast<unsigned long>(targetIndexes.size()), 0);
        }
    }

    if (archiveDocument)
        FPDF_CloseDocument(archiveDocument);
    if (success && FPDF_GetPageCount(document) == targetPageIds.size())
        return true;

    FPDF_CloseDocument(document);
    m_memoryData = rollbackData;
    m_document = FPDF_LoadMemDocument64(
        m_memoryData.constData(), static_cast<size_t>(m_memoryData.size()), nullptr);
    return false;
}

bool PdfDocument::saveSafely(const QString &filePath, bool createTimestampedBackup,
                             int backupVersionLimit, QString *errorMessage)
{
    if (!m_document) {
        if (errorMessage)
            *errorMessage = QStringLiteral("The PDF document is not loaded.");
        return false;
    }

    QSaveFile output(filePath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        if (errorMessage)
            *errorMessage = output.errorString();
        return false;
    }

    PdfFileWriter writer;
    writer.fileWrite.version = 1;
    writer.fileWrite.WriteBlock = &writePdfBlock;
    writer.device = &output;
    bool serialized = false;
    {
        const QMutexLocker locker(&pdfiumMutex());
        serialized = FPDF_SaveAsCopy(static_cast<FPDF_DOCUMENT>(m_document),
                                     &writer.fileWrite, FPDF_NO_INCREMENTAL);
    }
    if (!serialized || !output.flush()) {
        output.cancelWriting();
        if (errorMessage)
            *errorMessage = output.errorString().isEmpty()
                                ? QStringLiteral("PDFium could not serialize the document.")
                                : output.errorString();
        return false;
    }

    // Close PDFium's source handle before replacing the file. This is needed
    // on Windows, where an open source file cannot be atomically replaced.
    {
        const QMutexLocker locker(&pdfiumMutex());
        FPDF_CloseDocument(static_cast<FPDF_DOCUMENT>(m_document));
        m_document = nullptr;
        m_memoryData.clear();
    }

    QString backupPath;
    QString backupPrefix;
    bool backupCreated = false;
    if (createTimestampedBackup) {
        backupPath = timestampedBackupPath(filePath, &backupPrefix);
        backupCreated = !backupPath.isEmpty() && QFile::copy(filePath, backupPath)
                        && QFileInfo(backupPath).size() == QFileInfo(filePath).size();
        if (!backupCreated) {
            if (!backupPath.isEmpty())
                QFile::remove(backupPath);
            output.cancelWriting();
        }
    }

    const bool committed = (!createTimestampedBackup || backupCreated) && output.commit();
    const QByteArray pathBytes = filePath.toUtf8();
    {
        const QMutexLocker locker(&pdfiumMutex());
        m_document = FPDF_LoadDocument(pathBytes.constData(), nullptr);
        m_memoryData.clear();
    }

    if (!committed) {
        if (errorMessage) {
            *errorMessage = createTimestampedBackup && !backupCreated
                                ? QStringLiteral("The timestamped backup could not be created.")
                                : output.errorString();
        }
        return false;
    }
    if (!m_document) {
        if (errorMessage)
            *errorMessage = QStringLiteral("The PDF was saved but could not be reopened.");
        return false;
    }

    if (backupCreated)
        pruneTimestampedBackups(filePath, backupPrefix, qBound(1, backupVersionLimit, 99));
    return true;
}
