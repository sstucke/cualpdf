#include "pdfviewerwidget.h"

#include "pdfdocument.h"

#include <QFrame>
#include <QLabel>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kPageSpacing = 12;
}

PdfViewerWidget::PdfViewerWidget(const QString &filePath, int pageRenderWidth, QWidget *parent)
    : QWidget(parent)
    , m_filePath(filePath)
    , m_pageRenderWidth(pageRenderWidth)
    , m_document(std::make_unique<PdfDocument>(filePath))
    , m_scrollArea(new QScrollArea(this))
    , m_pagesContainer(new QWidget)
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->addWidget(m_scrollArea);

    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setWidget(m_pagesContainer);

    if (m_document->isValid() && m_document->pageCount() > 0) {
        buildPageLabels();
        connect(m_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged, this,
                &PdfViewerWidget::renderVisiblePages);
        QTimer::singleShot(0, this, &PdfViewerWidget::renderVisiblePages);
    } else {
        auto *layout = new QVBoxLayout(m_pagesContainer);
        auto *errorLabel = new QLabel(tr("Could not open this PDF file."), m_pagesContainer);
        errorLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(errorLabel);
    }
}

PdfViewerWidget::~PdfViewerWidget() = default;

bool PdfViewerWidget::isValid() const
{
    return m_document && m_document->isValid();
}

void PdfViewerWidget::buildPageLabels()
{
    auto *layout = new QVBoxLayout(m_pagesContainer);
    layout->setSpacing(kPageSpacing);
    layout->setContentsMargins(kPageSpacing, kPageSpacing, kPageSpacing, kPageSpacing);

    const int pageCount = m_document->pageCount();
    m_pageLabels.reserve(pageCount);
    m_pageRendered.resize(pageCount);

    for (int i = 0; i < pageCount; ++i) {
        const QSizeF sizePt = m_document->pageSizePoints(i);

        int width = m_pageRenderWidth;
        int height = width;
        if (sizePt.width() > 0.0 && sizePt.height() > 0.0)
            height = qMax(1, qRound(width * sizePt.height() / sizePt.width()));

        auto *label = new QLabel(m_pagesContainer);
        label->setFixedSize(width, height);
        label->setFrameShape(QFrame::Box);
        label->setStyleSheet(QStringLiteral("background-color: palette(base); border: 1px solid palette(mid);"));
        label->setAlignment(Qt::AlignCenter);

        layout->addWidget(label, 0, Qt::AlignHCenter);
        m_pageLabels.append(label);
    }

    layout->addStretch();
}

void PdfViewerWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    renderVisiblePages();
}

void PdfViewerWidget::goToPage(int pageIndex)
{
    if (pageIndex < 0 || pageIndex >= m_pageLabels.size())
        return;

    m_scrollArea->verticalScrollBar()->setValue(m_pageLabels[pageIndex]->y());
}

void PdfViewerWidget::renderVisiblePages()
{
    if (!m_document || !m_document->isValid() || m_pageLabels.isEmpty())
        return;

    const QRect viewportRect = m_scrollArea->viewport()->rect();
    const int margin = qMax(viewportRect.height(), 1);
    const QRect expanded = viewportRect.adjusted(0, -margin, 0, margin);

    int visiblePage = m_pageLabels.size() - 1;
    bool foundVisible = false;

    for (int i = 0; i < m_pageLabels.size(); ++i) {
        QLabel *label = m_pageLabels[i];
        const QPoint topLeft = label->mapTo(m_scrollArea->viewport(), QPoint(0, 0));
        const QRect labelRect(topLeft, label->size());

        // The first page whose bottom edge has not scrolled past the
        // viewport's top is considered the "current" page.
        if (!foundVisible && labelRect.bottom() >= viewportRect.top()) {
            visiblePage = i;
            foundVisible = true;
        }

        if (!m_pageRendered[i] && labelRect.intersects(expanded)) {
            const QImage image = m_document->renderPage(i, label->width());
            if (!image.isNull())
                label->setPixmap(QPixmap::fromImage(image));
            m_pageRendered[i] = true;
        }
    }

    if (visiblePage != m_currentPageIndex) {
        m_currentPageIndex = visiblePage;
        emit currentPageChanged(m_currentPageIndex);
    }
}
