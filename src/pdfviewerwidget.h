#pragma once

#include <QSet>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <memory>

class PdfDocument;
class QLabel;
class QScrollArea;

// Primitive, scroll-only PDF page viewer: pages are stacked vertically and
// rendered lazily as they scroll into view. No zoom, paging modes, or
// multi-column layout yet.
//
// Opening the document (parsing + per-page sizing) and rendering each page
// both run on short-lived worker threads, never the GUI thread, so a large
// or complex PDF cannot freeze the UI. PdfDocument itself is internally
// mutex-guarded so this is safe to call from a background thread.
class PdfViewerWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit PdfViewerWidget(const QString &filePath, int pageRenderWidth = 900, QWidget *parent = nullptr);
    ~PdfViewerWidget() override;

    bool isValid() const { return m_valid; }
    QString filePath() const { return m_filePath; }
    int pageCount() const { return m_pageLabels.size(); }
    int currentPageIndex() const { return m_currentPageIndex; }

public slots:
    void goToPage(int pageIndex);

signals:
    // Emitted once after the background load finishes (success or failure).
    void documentLoaded(bool valid, int pageCount);
    void currentPageChanged(int pageIndex);

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void renderVisiblePages();

private:
    void startLoading();
    void onDocumentLoaded(bool valid, const std::shared_ptr<PdfDocument> &document,
                           const QVector<QSizeF> &pageSizes);
    void buildPageLabels(const QVector<QSizeF> &pageSizes);
    void scheduleRender(int pageIndex, int widthPx);
    void onPageRendered(int pageIndex, const QImage &image);

    QString m_filePath;
    int m_pageRenderWidth;
    std::shared_ptr<PdfDocument> m_document;
    QScrollArea *m_scrollArea;
    QWidget *m_pagesContainer;
    QLabel *m_loadingLabel = nullptr;
    QVector<QLabel *> m_pageLabels;
    QVector<bool> m_pageRendered;
    QSet<int> m_pagesLoading;
    bool m_valid = false;
    int m_currentPageIndex = -1;
};
