#pragma once

#include <QImage>
#include <QMarginsF>
#include <QSizeF>
#include <QString>
#include <QVector>

struct PdfPageState {
    int pageIndex = -1;
    int rotation = 0;
    double cropLeft = 0.0;
    double cropBottom = 0.0;
    double cropRight = 0.0;
    double cropTop = 0.0;
};

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

    // Returns sizes for all pages in a single mutex acquisition — use this
    // instead of calling pageSizePoints() in a loop to avoid N lock round-trips.
    QVector<QSizeF> allPageSizes() const;

    // Renders a page at the given width in pixels, preserving aspect ratio.
    QImage renderPage(int pageIndex, int targetWidthPx) const;

    // Applies page-dictionary transformations to the loaded document. These
    // changes live in memory until a save workflow persists the document.
    bool rotatePages(const QVector<int> &pageIndexes, bool clockwise);
    bool cropPages(const QVector<int> &pageIndexes, const QMarginsF &marginsPoints);
    QVector<PdfPageState> pageStates(const QVector<int> &pageIndexes) const;
    bool restorePageStates(const QVector<PdfPageState> &states);
    bool saveSafely(const QString &filePath, bool createTimestampedBackup,
                    int backupVersionLimit, QString *errorMessage);

private:
    void *m_document = nullptr; // FPDF_DOCUMENT
};
