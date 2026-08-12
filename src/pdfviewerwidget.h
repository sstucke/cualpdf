#pragma once

#include <QHash>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <memory>

class PdfDocument;
class QComboBox;
class QGridLayout;
class QLabel;
class QScrollArea;
class QSpinBox;
class QToolButton;

// PDF viewer with lazy page rendering. Full document tabs expose navigation,
// page layout and zoom controls; the compact details-panel preview reuses the
// same renderer without the toolbar.
class PdfViewerWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit PdfViewerWidget(const QString &filePath, int pageRenderWidth = 900,
                             QWidget *parent = nullptr, bool showToolbar = true);
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
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void renderVisiblePages();

private:
    enum class PageLayout {
        SinglePage,
        Continuous,
        MultipleColumns,
    };

    enum class ZoomMode {
        FixedWidth,
        FitPage,
        FitWidth,
        Custom,
    };

    void buildToolbar();
    void startLoading();
    void onDocumentLoaded(bool valid, const std::shared_ptr<PdfDocument> &document,
                          const QVector<QSizeF> &pageSizes);
    void buildPageLabels(int pageCount);
    void rebuildPageLayout();
    void setPageLayout(PageLayout layout);
    void applyZoom();
    void setCustomZoom(int percent);
    void zoomByStep(int direction);
    void activateZoomSelection(int index);
    void applyTypedZoom();
    void syncNavigationControls();
    void syncZoomControl();
    void scrollToCurrentPage();
    QSizeF pageSize(int pageIndex) const;
    int multiColumnCount() const;
    void scheduleRender(int pageIndex, int widthPx);
    void onPageRendered(int pageIndex, int widthPx, const QImage &image);

    QString m_filePath;
    int m_initialPageRenderWidth;
    bool m_showToolbar;
    std::shared_ptr<PdfDocument> m_document;
    QScrollArea *m_scrollArea;
    QWidget *m_pagesContainer;
    QGridLayout *m_pagesLayout;
    QLabel *m_loadingLabel = nullptr;
    QVector<QLabel *> m_pageLabels;
    QVector<QSizeF> m_pageSizes;
    QVector<int> m_renderedWidths;
    QHash<int, int> m_pagesLoading;
    QToolButton *m_previousPageButton = nullptr;
    QSpinBox *m_pageSpinBox = nullptr;
    QLabel *m_pageCountLabel = nullptr;
    QToolButton *m_nextPageButton = nullptr;
    QComboBox *m_pageLayoutCombo = nullptr;
    QToolButton *m_zoomOutButton = nullptr;
    QComboBox *m_zoomCombo = nullptr;
    QToolButton *m_zoomInButton = nullptr;
    PageLayout m_pageLayout = PageLayout::Continuous;
    ZoomMode m_zoomMode = ZoomMode::FixedWidth;
    int m_zoomPercent = 100;
    int m_effectiveZoomPercent = 100;
    bool m_updatingControls = false;
    bool m_valid = false;
    int m_currentPageIndex = -1;
};
