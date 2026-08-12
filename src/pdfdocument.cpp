#include "pdfdocument.h"

#include <fpdfview.h>

#include <QByteArray>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>

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
    FS_SIZEF size;
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

    QVector<QSizeF> sizes;
    sizes.reserve(count);
    for (int i = 0; i < count; ++i) {
        FS_SIZEF size;
        if (FPDF_GetPageSizeByIndexF(doc, i, &size))
            sizes.append(QSizeF(size.width, size.height));
        else
            sizes.append(QSizeF(595, 842)); // A4 fallback
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
