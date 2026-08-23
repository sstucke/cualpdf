#pragma once

#include "pdfdocument.h"

#include <QHash>
#include <QRectF>
#include <QSet>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <memory>

class QAction;
class QComboBox;
class QGridLayout;
class QLabel;
class QScrollArea;
class QSpinBox;
class QToolButton;
class PdfInsertionPlaceholder;

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
    bool isModified() const { return m_modified; }
    bool isSaveInProgress() const { return m_saveInProgress; }
    bool isOperationInProgress() const { return m_transformInProgress; }
    bool canUndo() const
    {
        return !m_transformInProgress && !m_saveInProgress && m_historyPosition > 0;
    }
    bool canRedo() const
    {
        return !m_transformInProgress && !m_saveInProgress
               && m_historyPosition < m_editHistory.size();
    }
    QString filePath() const { return m_filePath; }
    int pageCount() const { return m_pageLabels.size(); }
    int currentPageIndex() const { return m_currentPageIndex; }
    int zoomPercent() const
    {
        return m_zoomMode == ZoomMode::Custom ? m_zoomPercent : m_effectiveZoomPercent;
    }

public slots:
    void goToPage(int pageIndex);
    void setZoomPercent(int percent);
    void saveDocument(bool createTimestampedBackup, int backupVersionLimit);
    void undo();
    void redo();

signals:
    // Emitted once after the background load finishes (success or failure).
    void documentLoaded(bool valid, int pageCount);
    void currentPageChanged(int pageIndex);
    void zoomPercentChanged(int percent);
    void modifiedChanged(bool modified);
    void operationInProgressChanged(bool inProgress);
    void saveFinished(bool success, const QString &errorMessage);
    void historyChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void renderVisiblePages();

private:
    struct EditHistoryEntry {
        QString description;
        QVector<PdfPageState> beforeStates;
        QVector<PdfPageState> afterStates;
        QVector<quint64> beforePageIds;
        QVector<quint64> afterPageIds;
        QVector<quint64> archivedPageIds;
        QByteArray pageArchive;

        bool changesPageStructure() const { return !beforePageIds.isEmpty(); }
    };

    enum class PageLayout {
        Continuous,
        Discrete,
    };

    enum class ZoomMode {
        FixedWidth,
        FitPage,
        FitWidth,
        FitTwoColumns,
        Custom,
    };

    enum class SelectionMode {
        Page,
        Region,
    };

    void buildToolbar();
    void startLoading();
    void onDocumentLoaded(bool valid, const std::shared_ptr<PdfDocument> &document,
                          const QVector<QSizeF> &pageSizes);
    void buildPageLabels(int pageCount);
    void buildInsertionPlaceholders();
    void rebuildPageLayout();
    void setOrganizePagesEnabled(bool enabled);
    void showPageContextMenu(int pageIndex, const QPoint &globalPosition);
    void showInsertionContextMenu(int insertionIndex, const QPoint &globalPosition);
    void startSelectedPageDrag(QLabel *sourceLabel);
    void moveSelectedPagesTo(int insertionIndex);
    void copySelectedPages(bool cut);
    void finishPageCopy(const QByteArray &pageArchive, int pageCount);
    void extractSelectedPages();
    void insertBlankPageAt(int insertionIndex);
    void pastePagesAt(int insertionIndex);
    void insertPdfAt(int insertionIndex);
    void insertArchiveAt(int insertionIndex, const QByteArray &pageArchive,
                         int pageCount, const QString &description);
    void applyPageStructureChange(const QString &description,
                                  const QVector<quint64> &beforePageIds,
                                  const QVector<quint64> &afterPageIds,
                                  const QVector<quint64> &archivedPageIds,
                                  const QByteArray &pageArchive,
                                  const QVector<quint64> &selectedPageIds);
    void finishPageStructureChange(bool success, const QVector<QSizeF> &pageSizes,
                                   const EditHistoryEntry &historyEntry,
                                   const QVector<quint64> &selectedPageIds);
    void rebuildPagesAfterStructure(const QVector<QSizeF> &pageSizes,
                                    const QVector<quint64> &pageIds,
                                    const QVector<quint64> &selectedPageIds);
    void setPageLayout(PageLayout layout);
    void applyZoom();
    void setCustomZoom(int percent);
    void zoomByStep(int direction);
    void activateZoomSelection(int index);
    void applyTypedZoom();
    void syncNavigationControls();
    void syncZoomControl();
    void syncViewControl();
    void syncFitControl();
    void setSelectionMode(SelectionMode mode);
    void applyPageSelectionCommand(int commandIndex);
    void updateSelectionOverlays();
    void syncEditControls();
    void rotateSelectedPages(bool clockwise);
    void cropSelectedRegion();
    void lightenSelectedPage();
    void finishDocumentTransform(bool success, const QVector<QSizeF> &pageSizes,
                                 const QVector<int> &affectedPages, bool clearRegion,
                                 const QString &historyDescription,
                                 const QVector<PdfPageState> &beforeStates,
                                 const QVector<PdfPageState> &afterStates);
    void recordHistoryEntry(EditHistoryEntry entry);
    void navigateHistory(bool redoOperation);
    void finishHistoryNavigation(bool success, const QVector<QSizeF> &pageSizes,
                                 const QVector<int> &affectedPages,
                                 int targetHistoryPosition);
    void finishStructureHistoryNavigation(bool success,
                                          const QVector<QSizeF> &pageSizes,
                                          const QVector<quint64> &targetPageIds,
                                          int targetHistoryPosition);
    void updateModifiedState();
    void finishDocumentSave(bool success, const QString &errorMessage);
    void setCurrentPageFromPointer(int pageIndex);
    QPointF normalizedPagePosition(const QLabel *label, const QPointF &position) const;
    void navigateByPageGroup(int direction);
    void scrollToCurrentPage();
    QSizeF pageSize(int pageIndex) const;
    int availableColumnCount() const;
    int fitTwoColumnCount() const;
    QSize discreteGridShape() const;
    int discretePageCount() const;
    int discreteChunkStart() const;
    void scheduleRender(int pageIndex, int widthPx);
    void onPageRendered(int pageIndex, int widthPx, int documentRevision,
                        const QImage &image);

    QString m_filePath;
    int m_initialPageRenderWidth;
    bool m_showToolbar;
    std::shared_ptr<PdfDocument> m_document;
    QScrollArea *m_scrollArea;
    QWidget *m_pagesContainer;
    QGridLayout *m_pagesLayout;
    QLabel *m_loadingLabel = nullptr;
    QVector<QLabel *> m_pageLabels;
    QVector<PdfInsertionPlaceholder *> m_pagePlaceholders;
    QVector<QSizeF> m_pageSizes;
    QVector<quint64> m_pageIds;
    QVector<int> m_renderedWidths;
    QHash<int, int> m_pagesLoading;
    QToolButton *m_previousPageButton = nullptr;
    QSpinBox *m_pageSpinBox = nullptr;
    QLabel *m_pageCountLabel = nullptr;
    QToolButton *m_nextPageButton = nullptr;
    QToolButton *m_viewModeButton = nullptr;
    QAction *m_continuousAction = nullptr;
    QAction *m_discreteAction = nullptr;
    QToolButton *m_zoomOutButton = nullptr;
    QComboBox *m_zoomCombo = nullptr;
    QToolButton *m_zoomInButton = nullptr;
    QToolButton *m_zoomFitButton = nullptr;
    QAction *m_fitPageAction = nullptr;
    QAction *m_fitWidthAction = nullptr;
    QAction *m_fitTwoColumnsAction = nullptr;
    QToolButton *m_selectPageButton = nullptr;
    QToolButton *m_selectRegionButton = nullptr;
    QComboBox *m_pageSelectionCombo = nullptr;
    QToolButton *m_cropButton = nullptr;
    QToolButton *m_rotateCounterclockwiseButton = nullptr;
    QToolButton *m_rotateClockwiseButton = nullptr;
    QToolButton *m_organizePagesButton = nullptr;
    QAction *m_cutPagesAction = nullptr;
    QAction *m_copyPagesAction = nullptr;
    QAction *m_extractPagesAction = nullptr;
    PageLayout m_pageLayout = PageLayout::Continuous;
    ZoomMode m_zoomMode = ZoomMode::FixedWidth;
    SelectionMode m_selectionMode = SelectionMode::Page;
    QSet<int> m_selectedPages;
    int m_pageSelectionAnchor = -1;
    int m_regionPageIndex = -1;
    QRectF m_regionSelection;
    QPointF m_regionDragStart;
    bool m_draggingRegion = false;
    bool m_organizePagesEnabled = false;
    QPoint m_pageDragStart;
    int m_pageDragSourceIndex = -1;
    bool m_transformInProgress = false;
    bool m_saveInProgress = false;
    bool m_modified = false;
    QVector<EditHistoryEntry> m_editHistory;
    int m_historyPosition = 0;
    int m_savedHistoryPosition = 0;
    int m_documentRevision = 0;
    int m_zoomPercent = 100;
    int m_effectiveZoomPercent = 100;
    int m_lastReportedZoomPercent = -1;
    bool m_updatingControls = false;
    bool m_valid = false;
    int m_currentPageIndex = -1;
    quint64 m_nextPageId = 1;

    static constexpr int kHistoryLimit = 20;
};
