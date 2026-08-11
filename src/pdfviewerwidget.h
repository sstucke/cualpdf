#pragma once

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
class PdfViewerWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit PdfViewerWidget(const QString &filePath, int pageRenderWidth = 900, QWidget *parent = nullptr);
    ~PdfViewerWidget() override;

    bool isValid() const;
    QString filePath() const { return m_filePath; }
    int pageCount() const { return m_pageLabels.size(); }
    int currentPageIndex() const { return m_currentPageIndex; }

public slots:
    void goToPage(int pageIndex);

signals:
    void currentPageChanged(int pageIndex);

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void renderVisiblePages();

private:
    void buildPageLabels();

    QString m_filePath;
    int m_pageRenderWidth;
    std::unique_ptr<PdfDocument> m_document;
    QScrollArea *m_scrollArea;
    QWidget *m_pagesContainer;
    QVector<QLabel *> m_pageLabels;
    QVector<bool> m_pageRendered;
    int m_currentPageIndex = -1;
};
