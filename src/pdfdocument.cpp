#include "pdfdocument.h"

#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>
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
#include <cmath>
#include <cstring>

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
    FS_SIZEF size = {};
    if (!FPDF_GetPageSizeByIndexF(static_cast<FPDF_DOCUMENT>(m_document), pageIndex, &size))
        return {};
    return QSizeF(size.width, size.height);
}

QVector<QSizeF> PdfDocument::allPageSizes() const
{
    if (!m_document)
        return {};

    const QMutexLocker locker(&pdfiumMutex());
    const auto doc = static_cast<FPDF_DOCUMENT>(m_document);
    const int count = FPDF_GetPageCount(doc);

    // Page-dictionary lookup. FPDF_LoadPage parses the page and is what made
    // opening a long document stall before the first paint. Rotation is
    // included; a mismatch with the rendered bitmap is corrected later.
    QVector<QSizeF> sizes;
    sizes.reserve(count);
    for (int i = 0; i < count; ++i) {
        FS_SIZEF size = {};
        if (!FPDF_GetPageSizeByIndexF(doc, i, &size) || size.width <= 0.0 || size.height <= 0.0)
            sizes.append(QSizeF(595, 842)); // A4 fallback
        else
            sizes.append(QSizeF(size.width, size.height));
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

namespace {
PdfPageObjectKind objectKind(int type)
{
    switch (type) {
    case FPDF_PAGEOBJ_TEXT:
        return PdfPageObjectKind::Text;
    case FPDF_PAGEOBJ_PATH:
        return PdfPageObjectKind::Path;
    case FPDF_PAGEOBJ_IMAGE:
        return PdfPageObjectKind::Image;
    case FPDF_PAGEOBJ_SHADING:
        return PdfPageObjectKind::Shading;
    case FPDF_PAGEOBJ_FORM:
        return PdfPageObjectKind::Form;
    default:
        return PdfPageObjectKind::Unknown;
    }
}

QRect deviceBounds(FPDF_PAGE page, const QSize &deviceSize, const FS_RECTF &bounds)
{
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    bool any = false;
    const double xs[4] = {bounds.left, bounds.right, bounds.right, bounds.left};
    const double ys[4] = {bounds.top, bounds.top, bounds.bottom, bounds.bottom};
    for (int i = 0; i < 4; ++i) {
        int deviceX = 0;
        int deviceY = 0;
        if (!FPDF_PageToDevice(page, 0, 0, deviceSize.width(), deviceSize.height(),
                               /*rotate=*/0, xs[i], ys[i], &deviceX, &deviceY))
            continue;
        if (!any) {
            minX = maxX = deviceX;
            minY = maxY = deviceY;
            any = true;
        } else {
            minX = qMin(minX, deviceX);
            minY = qMin(minY, deviceY);
            maxX = qMax(maxX, deviceX);
            maxY = qMax(maxY, deviceY);
        }
    }
    if (!any || maxX <= minX || maxY <= minY)
        return {};
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

QImage bitmapToImage(FPDF_BITMAP bitmap)
{
    if (!bitmap)
        return {};
    const int width = FPDFBitmap_GetWidth(bitmap);
    const int height = FPDFBitmap_GetHeight(bitmap);
    const int stride = FPDFBitmap_GetStride(bitmap);
    auto *buffer = static_cast<uchar *>(FPDFBitmap_GetBuffer(bitmap));
    if (!buffer || width <= 0 || height <= 0 || stride <= 0)
        return {};

    QImage::Format format = QImage::Format_Invalid;
    switch (FPDFBitmap_GetFormat(bitmap)) {
    case FPDFBitmap_BGRA:
        format = QImage::Format_ARGB32;
        break;
    case FPDFBitmap_BGRx:
        format = QImage::Format_RGB32;
        break;
    case FPDFBitmap_BGR:
        format = QImage::Format_BGR888;
        break;
    case FPDFBitmap_Gray:
        format = QImage::Format_Grayscale8;
        break;
    default:
        return {};
    }
    return QImage(buffer, width, height, stride, format).copy();
}

struct PlacedMatrix {
    float a = 1;
    float b = 0;
    float c = 0;
    float d = 1;
    float e = 0;
    float f = 0;
};

PlacedMatrix multiply(const PlacedMatrix &parent, const PlacedMatrix &local)
{
    return {parent.a * local.a + parent.c * local.b,
            parent.b * local.a + parent.d * local.b,
            parent.a * local.c + parent.c * local.d,
            parent.b * local.c + parent.d * local.d,
            parent.a * local.e + parent.c * local.f + parent.e,
            parent.b * local.e + parent.d * local.f + parent.f};
}

QRect boundsInPage(FPDF_PAGE page, const QSize &deviceSize, FPDF_PAGEOBJECT object,
                   const PlacedMatrix &toPage)
{
    FS_RECTF bounds = {};
    if (!FPDFPageObj_GetBounds(object, &bounds.left, &bounds.bottom, &bounds.right, &bounds.top))
        return {};
    const double xs[4] = {bounds.left, bounds.right, bounds.right, bounds.left};
    const double ys[4] = {bounds.bottom, bounds.bottom, bounds.top, bounds.top};
    FS_RECTF pageBounds = {};
    bool any = false;
    for (int i = 0; i < 4; ++i) {
        const float x = toPage.a * xs[i] + toPage.c * ys[i] + toPage.e;
        const float y = toPage.b * xs[i] + toPage.d * ys[i] + toPage.f;
        if (!any) {
            pageBounds.left = pageBounds.right = x;
            pageBounds.bottom = pageBounds.top = y;
            any = true;
        } else {
            pageBounds.left = qMin(pageBounds.left, x);
            pageBounds.right = qMax(pageBounds.right, x);
            pageBounds.bottom = qMin(pageBounds.bottom, y);
            pageBounds.top = qMax(pageBounds.top, y);
        }
    }
    return any ? deviceBounds(page, deviceSize, pageBounds) : QRect();
}

QString textOf(FPDF_PAGEOBJECT object, FPDF_TEXTPAGE textPage)
{
    if (!textPage)
        return {};
    const unsigned long bytes = FPDFTextObj_GetText(object, textPage, nullptr, 0);
    if (bytes < sizeof(FPDF_WCHAR))
        return {};
    QVector<FPDF_WCHAR> buffer(static_cast<int>(bytes / sizeof(FPDF_WCHAR)));
    FPDFTextObj_GetText(object, textPage, buffer.data(), bytes);
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(buffer.constData()));
}

QByteArray fontBytes(FPDF_FONT font)
{
    size_t size = 0;
    if (!font || !FPDFFont_GetFontData(font, nullptr, 0, &size) || size == 0)
        return {};
    QByteArray data(static_cast<int>(size), Qt::Uninitialized);
    size_t written = 0;
    if (!FPDFFont_GetFontData(font, reinterpret_cast<uint8_t *>(data.data()), size, &written)
        || written == 0)
        return {};
    data.resize(static_cast<int>(written));
    return data;
}

QString utf8FontName(size_t (*read)(FPDF_FONT, char *, size_t), FPDF_FONT font)
{
    const size_t bytes = read(font, nullptr, 0);
    if (bytes < 2)
        return {};
    QByteArray buffer(static_cast<int>(bytes), Qt::Uninitialized);
    read(font, buffer.data(), bytes);
    return QString::fromUtf8(buffer.constData());
}

QPoint devicePoint(FPDF_PAGE page, const QSize &deviceSize, float pageX, float pageY)
{
    int x = 0;
    int y = 0;
    if (!FPDF_PageToDevice(page, 0, 0, deviceSize.width(), deviceSize.height(), /*rotate=*/0,
                           pageX, pageY, &x, &y))
        return {};
    return QPoint(x, y);
}

void describeText(FPDF_PAGE page, const QSize &deviceSize, FPDF_PAGEOBJECT object,
                  const PlacedMatrix &toPage, PdfPageObjectInfo *info)
{
    float fontSize = 0;
    FPDFTextObj_GetFontSize(object, &fontSize);
    FS_MATRIX local = {1, 0, 0, 1, 0, 0};
    FPDFPageObj_GetMatrix(object, &local);
    const float matrixScale = std::hypot(local.a, local.b);
    float userScale = 1.f;
    if (fontSize > 0.01f && !(matrixScale > fontSize * 0.5f && matrixScale < fontSize * 2.f))
        userScale = matrixScale > 0.01f ? matrixScale : 1.f;
    const double pageWidth = FPDF_GetPageWidthF(page);
    const double pixelsPerPoint = pageWidth > 0.0 ? deviceSize.width() / pageWidth : 1.0;
    info->fontPixelSize = qMax(1, qRound(fontSize * userScale * pixelsPerPoint));
    info->fontSizePoints = fontSize * userScale;

    const PlacedMatrix placed = multiply(toPage, {local.a, local.b, local.c, local.d, local.e, local.f});
    info->baseline = devicePoint(page, deviceSize, placed.e, placed.f);

    const FPDF_FONT font = FPDFTextObj_GetFont(object);
    info->fontData = fontBytes(font);
    info->fontFamily = utf8FontName(&FPDFFont_GetFamilyName, font);
    if (info->fontFamily.isEmpty())
        info->fontFamily = utf8FontName(&FPDFFont_GetBaseFontName, font);
    const int weight = font ? FPDFFont_GetWeight(font) : -1;
    info->fontWeight = weight > 0 ? weight : 400;
    int italicAngle = 0;
    info->italic = font && FPDFFont_GetItalicAngle(font, &italicAngle) && italicAngle != 0;

    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;
    unsigned int alpha = 255;
    if (FPDFPageObj_GetFillColor(object, &red, &green, &blue, &alpha))
        info->color = QColor(static_cast<int>(red), static_cast<int>(green),
                             static_cast<int>(blue), static_cast<int>(alpha));
}

void collectObjects(FPDF_PAGE page, const QSize &deviceSize, FPDF_TEXTPAGE textPage,
                    FPDF_PAGEOBJECT container, int containerCount, bool containerIsForm,
                    const PlacedMatrix &toPage, QVector<int> path,
                    QVector<PdfPageObjectInfo> *objects)
{
    for (int index = 0; index < containerCount; ++index) {
        const FPDF_PAGEOBJECT object = containerIsForm
                                           ? FPDFFormObj_GetObject(container, static_cast<unsigned long>(index))
                                           : FPDFPage_GetObject(page, index);
        if (!object)
            continue;
        QVector<int> objectPath = path;
        objectPath.append(index);
        const PdfPageObjectKind kind = objectKind(FPDFPageObj_GetType(object));
        if (kind == PdfPageObjectKind::Form) {
            const int childCount = FPDFFormObj_CountObjects(object);
            if (childCount <= 0)
                continue;
            PlacedMatrix formMatrix;
            FS_MATRIX matrix = {};
            if (FPDFPageObj_GetMatrix(object, &matrix))
                formMatrix = {matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f};
            collectObjects(page, deviceSize, textPage, object, childCount, true,
                           multiply(toPage, formMatrix), objectPath, objects);
            continue;
        }
        if (kind == PdfPageObjectKind::Unknown)
            continue;
        const QRect device = boundsInPage(page, deviceSize, object, toPage);
        if (device.isEmpty())
            continue;
        PdfPageObjectInfo info;
        info.path = objectPath;
        info.kind = kind;
        info.bounds = device;
        if (kind == PdfPageObjectKind::Text) {
            info.text = textOf(object, textPage);
            describeText(page, deviceSize, object, toPage, &info);
        }
        objects->append(info);
    }
}

FPDF_PAGEOBJECT pageObjectAt(FPDF_PAGE page, const QVector<int> &objectPath)
{
    if (!page || objectPath.isEmpty() || objectPath.first() < 0
        || objectPath.first() >= FPDFPage_CountObjects(page))
        return nullptr;
    FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, objectPath.first());
    for (int depth = 1; object && depth < objectPath.size(); ++depth) {
        if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_FORM)
            return nullptr;
        const int index = objectPath.at(depth);
        if (index < 0 || index >= FPDFFormObj_CountObjects(object))
            return nullptr;
        object = FPDFFormObj_GetObject(object, static_cast<unsigned long>(index));
    }
    return object;
}

QVector<float> matrixOf(FPDF_PAGEOBJECT object)
{
    FS_MATRIX matrix = {};
    if (!FPDFPageObj_GetMatrix(object, &matrix))
        return {};
    return {matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f};
}

bool applyMatrix(FPDF_PAGE page, FPDF_PAGEOBJECT object, const QVector<float> &matrix)
{
    if (matrix.size() != 6)
        return false;
    FS_MATRIX value = {matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]};
    if (!FPDFPageObj_SetMatrix(object, &value))
        return false;
    return FPDFPage_GenerateContent(page);
}
}

QVector<PdfPageObjectInfo> PdfDocument::pageObjects(int pageIndex,
                                                   const QSize &deviceSize) const
{
    if (!m_document || deviceSize.width() <= 0 || deviceSize.height() <= 0)
        return {};

    const QMutexLocker locker(&pdfiumMutex());
    const FPDF_PAGE page = FPDF_LoadPage(static_cast<FPDF_DOCUMENT>(m_document), pageIndex);
    if (!page)
        return {};

    QVector<PdfPageObjectInfo> objects;
    const FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    collectObjects(page, deviceSize, textPage, nullptr, FPDFPage_CountObjects(page), false,
                   {}, {}, &objects);
    FPDFText_ClosePage(textPage);
    FPDF_ClosePage(page);
    return objects;
}

bool PdfDocument::translatePageObject(int pageIndex, const QVector<int> &objectPath,
                                     const QSize &deviceSize, const QPoint &deltaPixels,
                                     QVector<float> *beforeMatrix, QVector<float> *afterMatrix)
{
    if (!m_document || deviceSize.width() <= 0 || deviceSize.height() <= 0 || deltaPixels.isNull())
        return false;

    const QMutexLocker locker(&pdfiumMutex());
    const FPDF_PAGE page = FPDF_LoadPage(static_cast<FPDF_DOCUMENT>(m_document), pageIndex);
    if (!page)
        return false;

    const FPDF_PAGEOBJECT object = pageObjectAt(page, objectPath);
    if (!object) {
        FPDF_ClosePage(page);
        return false;
    }

    double originX = 0.0;
    double originY = 0.0;
    double movedX = 0.0;
    double movedY = 0.0;
    const bool mapped = FPDF_DeviceToPage(page, 0, 0, deviceSize.width(), deviceSize.height(),
                                          /*rotate=*/0, 0, 0, &originX, &originY)
                        && FPDF_DeviceToPage(page, 0, 0, deviceSize.width(), deviceSize.height(),
                                             /*rotate=*/0, deltaPixels.x(), deltaPixels.y(),
                                             &movedX, &movedY);
    const QVector<float> before = matrixOf(object);
    bool applied = mapped && !before.isEmpty();
    if (applied)
        FPDFPageObj_Transform(object, 1, 0, 0, 1, movedX - originX, movedY - originY);
    const QVector<float> after = matrixOf(object);
    applied = applied && applyMatrix(page, object, after);
    FPDF_ClosePage(page);
    if (!applied)
        return false;
    if (beforeMatrix)
        *beforeMatrix = before;
    if (afterMatrix)
        *afterMatrix = after;
    return true;
}

bool PdfDocument::setPageObjectMatrix(int pageIndex, const QVector<int> &objectPath,
                                     const QVector<float> &matrix)
{
    if (!m_document)
        return false;

    const QMutexLocker locker(&pdfiumMutex());
    const FPDF_PAGE page = FPDF_LoadPage(static_cast<FPDF_DOCUMENT>(m_document), pageIndex);
    if (!page)
        return false;
    const FPDF_PAGEOBJECT object = pageObjectAt(page, objectPath);
    const bool applied = object && applyMatrix(page, object, matrix);
    FPDF_ClosePage(page);
    return applied;
}

QByteArray PdfDocument::pageObjectImagePng(int pageIndex, const QVector<int> &objectPath) const
{
    if (!m_document)
        return {};

    const QMutexLocker locker(&pdfiumMutex());
    const FPDF_PAGE page = FPDF_LoadPage(static_cast<FPDF_DOCUMENT>(m_document), pageIndex);
    if (!page)
        return {};
    const FPDF_PAGEOBJECT object = pageObjectAt(page, objectPath);
    QByteArray png;
    if (object && FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_IMAGE) {
        const FPDF_BITMAP bitmap = FPDFImageObj_GetBitmap(object);
        const QImage image = bitmapToImage(bitmap);
        FPDFBitmap_Destroy(bitmap);
        if (!image.isNull()) {
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
        }
    }
    FPDF_ClosePage(page);
    return png;
}

bool PdfDocument::setPageObjectImagePng(int pageIndex, const QVector<int> &objectPath, const QByteArray &png)
{
    if (!m_document || png.isEmpty())
        return false;

    QImage image;
    if (!image.loadFromData(png))
        return false;
    image = image.convertToFormat(QImage::Format_ARGB32);

    const QMutexLocker locker(&pdfiumMutex());
    const FPDF_PAGE page = FPDF_LoadPage(static_cast<FPDF_DOCUMENT>(m_document), pageIndex);
    if (!page)
        return false;
    const FPDF_PAGEOBJECT object = pageObjectAt(page, objectPath);
    if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_IMAGE) {
        FPDF_ClosePage(page);
        return false;
    }

    const FPDF_BITMAP bitmap = FPDFBitmap_Create(image.width(), image.height(), /*alpha=*/1);
    if (!bitmap) {
        FPDF_ClosePage(page);
        return false;
    }
    auto *buffer = static_cast<uchar *>(FPDFBitmap_GetBuffer(bitmap));
    const int stride = FPDFBitmap_GetStride(bitmap);
    for (int y = 0; y < image.height(); ++y)
        memcpy(buffer + y * stride, image.constScanLine(y), image.width() * 4);
    const bool applied = FPDFImageObj_SetBitmap(nullptr, 0, object, bitmap)
                         && FPDFPage_GenerateContent(page);
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);
    return applied;
}

bool PdfDocument::setPageObjectText(int pageIndex, const QVector<int> &objectPath, const QString &text)
{
    if (!m_document || text.isEmpty())
        return false;

    const QMutexLocker locker(&pdfiumMutex());
    const FPDF_PAGE page = FPDF_LoadPage(static_cast<FPDF_DOCUMENT>(m_document), pageIndex);
    if (!page)
        return false;
    const FPDF_PAGEOBJECT object = pageObjectAt(page, objectPath);
    const bool applied = object
                         && FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_TEXT
                         && FPDFText_SetText(object, reinterpret_cast<FPDF_WIDESTRING>(text.utf16()))
                         && FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);
    return applied;
}

bool PdfDocument::setPageObjectFontSize(int pageIndex, const QVector<int> &objectPath,
                                       float sizePoints)
{
    if (!m_document || sizePoints < 0.f)
        return false;
    const QMutexLocker locker(&pdfiumMutex());
    const FPDF_PAGE page = FPDF_LoadPage(static_cast<FPDF_DOCUMENT>(m_document), pageIndex);
    if (!page)
        return false;
    const FPDF_PAGEOBJECT object = pageObjectAt(page, objectPath);
    const bool applied = object && FPDFTextObj_SetFontSize(object, sizePoints)
                         && FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);
    return applied;
}

bool PdfDocument::setPageObjectTextColor(int pageIndex, const QVector<int> &objectPath,
                                         const QColor &color)
{
    if (!m_document || !color.isValid())
        return false;
    const QMutexLocker locker(&pdfiumMutex());
    const FPDF_PAGE page = FPDF_LoadPage(static_cast<FPDF_DOCUMENT>(m_document), pageIndex);
    if (!page)
        return false;
    const FPDF_PAGEOBJECT object = pageObjectAt(page, objectPath);
    const bool applied = object
                         && FPDFPageObj_SetFillColor(object,
                                                     static_cast<unsigned int>(color.red()),
                                                     static_cast<unsigned int>(color.green()),
                                                     static_cast<unsigned int>(color.blue()),
                                                     static_cast<unsigned int>(color.alpha()))
                         && FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);
    return applied;
}

bool PdfDocument::replacePageObjectTypeface(int pageIndex, const QVector<int> &objectPath,
                                            const QByteArray &fontData,
                                            const QString &standardFontName)
{
    if (!m_document || objectPath.size() != 1)
        return false;
    const QMutexLocker locker(&pdfiumMutex());
    const auto document = static_cast<FPDF_DOCUMENT>(m_document);
    const FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
    if (!page)
        return false;
    const FPDF_PAGEOBJECT object = pageObjectAt(page, objectPath);
    if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT) {
        FPDF_ClosePage(page);
        return false;
    }
    float size = 12.f;
    FPDFTextObj_GetFontSize(object, &size);
    const QVector<float> matrix = matrixOf(object);
    const FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    const QString text = textOf(object, textPage);
    FPDFText_ClosePage(textPage);
    unsigned int red = 0, green = 0, blue = 0, alpha = 255;
    FPDFPageObj_GetFillColor(object, &red, &green, &blue, &alpha);

    FPDF_FONT font = nullptr;
    if (!standardFontName.isEmpty())
        font = FPDFText_LoadStandardFont(document, standardFontName.toLatin1().constData());
    else if (!fontData.isEmpty())
        font = FPDFText_LoadFont(document, reinterpret_cast<const uint8_t *>(fontData.constData()),
                                 static_cast<uint32_t>(fontData.size()), FPDF_FONT_TRUETYPE,
                                 /*cid=*/false);
    if (!font) {
        FPDF_ClosePage(page);
        return false;
    }
    const FPDF_PAGEOBJECT created = FPDFPageObj_CreateTextObj(document, font, size);
    FPDFFont_Close(font);
    bool applied = false;
    if (created && !text.isEmpty()
        && FPDFText_SetText(created, reinterpret_cast<FPDF_WIDESTRING>(text.utf16()))
        && matrix.size() == 6) {
        FS_MATRIX value = {matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]};
        FPDFPageObj_SetMatrix(created, &value);
        FPDFPageObj_SetFillColor(created, red, green, blue, alpha);
        if (FPDFPage_RemoveObject(page, object)) {
            FPDFPageObj_Destroy(object);
            applied = FPDFPage_InsertObjectAtIndex(page, created, static_cast<size_t>(objectPath.first()))
                      && FPDFPage_GenerateContent(page);
        }
    }
    if (!applied && created)
        FPDFPageObj_Destroy(created);
    FPDF_ClosePage(page);
    return applied;
}

int PdfDocument::insertPageText(int pageIndex, const QPointF &originPoints, const QString &text,
                                float sizePoints)
{
    if (!m_document || text.isEmpty() || sizePoints <= 0.f)
        return -1;
    const QMutexLocker locker(&pdfiumMutex());
    const auto document = static_cast<FPDF_DOCUMENT>(m_document);
    const FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
    if (!page)
        return -1;
    const FPDF_FONT font = FPDFText_LoadStandardFont(document, "Helvetica");
    const FPDF_PAGEOBJECT created = font ? FPDFPageObj_CreateTextObj(document, font, sizePoints)
                                         : nullptr;
    if (font)
        FPDFFont_Close(font);
    bool applied = false;
    if (created && FPDFText_SetText(created, reinterpret_cast<FPDF_WIDESTRING>(text.utf16()))) {
        FS_MATRIX value = {1, 0, 0, 1, static_cast<float>(originPoints.x()),
                           static_cast<float>(originPoints.y())};
        FPDFPageObj_SetMatrix(created, &value);
        FPDFPageObj_SetFillColor(created, 0, 0, 0, 255);
        applied = FPDFPage_InsertObject(page, created) && FPDFPage_GenerateContent(page);
    }
    if (!applied && created)
        FPDFPageObj_Destroy(created);
    const int index = applied ? FPDFPage_CountObjects(page) - 1 : -1;
    FPDF_ClosePage(page);
    return index;
}

QPointF PdfDocument::pagePointAt(int pageIndex, const QSize &deviceSize, const QPoint &pixel) const
{
    if (!m_document || deviceSize.isEmpty())
        return {};
    const QMutexLocker locker(&pdfiumMutex());
    const FPDF_PAGE page = FPDF_LoadPage(static_cast<FPDF_DOCUMENT>(m_document), pageIndex);
    if (!page)
        return {};
    double x = 0;
    double y = 0;
    const bool mapped = FPDF_DeviceToPage(page, 0, 0, deviceSize.width(), deviceSize.height(),
                                          /*rotate=*/0, pixel.x(), pixel.y(), &x, &y);
    FPDF_ClosePage(page);
    return mapped ? QPointF(x, y) : QPointF();
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

QByteArray PdfDocument::createImagePageArchive(const QImage &image, const QSizeF &pageSize)
{
    if (image.isNull() || pageSize.width() <= 0.0 || pageSize.height() <= 0.0)
        return {};

    // FPDFBitmap_Create(..., alpha=0) yields BGRx byte order, matching
    // QImage::Format_RGB32's in-memory layout — see renderPage() above.
    const QImage source = image.convertToFormat(QImage::Format_RGB32);

    const QMutexLocker locker(&pdfiumMutex());
    const FPDF_DOCUMENT archiveDocument = FPDF_CreateNewDocument();
    if (!archiveDocument)
        return {};

    QByteArray archive;
    const FPDF_PAGE page = FPDFPage_New(archiveDocument, 0,
                                        pageSize.width(), pageSize.height());
    if (page) {
        const FPDF_BITMAP bitmap =
            FPDFBitmap_Create(source.width(), source.height(), /*alpha=*/0);
        if (bitmap) {
            auto *buffer = static_cast<uchar *>(FPDFBitmap_GetBuffer(bitmap));
            const int stride = FPDFBitmap_GetStride(bitmap);
            for (int row = 0; row < source.height(); ++row) {
                std::memcpy(buffer + static_cast<size_t>(row) * stride,
                           source.constScanLine(row),
                           static_cast<size_t>(source.width()) * 4);
            }

            const FPDF_PAGEOBJECT imageObject = FPDFPageObj_NewImageObj(archiveDocument);
            if (imageObject && FPDFImageObj_SetBitmap(nullptr, 0, imageObject, bitmap)) {
                // Image objects start as a 1x1 unit square; scale to fill
                // the page exactly.
                FPDFPageObj_Transform(imageObject, pageSize.width(), 0, 0,
                                      pageSize.height(), 0, 0);
                FPDFPage_InsertObject(page, imageObject);
                if (FPDFPage_GenerateContent(page))
                    archive = serializeDocument(archiveDocument);
            } else if (imageObject) {
                FPDFPageObj_Destroy(imageObject);
            }
            FPDFBitmap_Destroy(bitmap);
        }
        FPDF_ClosePage(page);
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
