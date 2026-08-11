#pragma once

#include <QImage>
#include <QSizeF>
#include <QString>

// Thin RAII wrapper around PDFium's C API. Keeps every FPDF_* type out of
// this header so callers do not need PDFium's include path.
class PdfDocument
{
public:
    // Must be called once before any PdfDocument is constructed, and
    // shutdownLibrary() once no PdfDocument instances remain.
    static void initializeLibrary();
    static void shutdownLibrary();

    explicit PdfDocument(const QString &filePath);
    ~PdfDocument();

    PdfDocument(const PdfDocument &) = delete;
    PdfDocument &operator=(const PdfDocument &) = delete;

    bool isValid() const { return m_document != nullptr; }
    int pageCount() const;
    QSizeF pageSizePoints(int pageIndex) const;

    // Renders a page at the given width in pixels, preserving aspect ratio.
    QImage renderPage(int pageIndex, int targetWidthPx) const;

private:
    void *m_document = nullptr; // FPDF_DOCUMENT
};
