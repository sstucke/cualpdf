#include "pdfviewerwidget.h"

#include "pdfdocument.h"

#include <QFrame>
#include <QLabel>
#include <QLoggingCategory>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kPageSpacing = 12;
}

PdfViewerWidget::PdfViewerWidget(const QString &filePath, int pageRenderWidth, QWidget *parent)
    : QWidget(parent)
    , m_filePath(filePath)
    , m_pageRenderWidth(pageRenderWidth)
    , m_scrollArea(new QScrollArea(this))
    , m_pagesContainer(new QWidget)
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->addWidget(m_scrollArea);

    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setWidget(m_pagesContainer);

    auto *containerLayout = new QVBoxLayout(m_pagesContainer);
    m_loadingLabel = new QLabel(tr("Loading…"), m_pagesContainer);
    m_loadingLabel->setAlignment(Qt::AlignCenter);
    containerLayout->addWidget(m_loadingLabel);

    startLoading();
}

PdfViewerWidget::~PdfViewerWidget() = default;

void PdfViewerWidget::startLoading()
{
    const QString path = m_filePath;

    // The worker owns its own PdfDocument handle; only the (thread-safe,
    // ref-counted) result crosses back to the GUI thread, so the file is
    // never opened twice. QMetaObject::invokeMethod with `this` as context
    // safely no-ops if the widget is destroyed before the load finishes.
    QThread *thread = QThread::create([this, path]() {
        auto document = std::make_shared<PdfDocument>(path);
        const bool valid = document->isValid() && document->pageCount() > 0;

        QVector<QSizeF> pageSizes;
        if (valid) {
            const int count = document->pageCount();
            pageSizes.reserve(count);
            for (int i = 0; i < count; ++i)
                pageSizes.append(document->pageSizePoints(i));
        }

        QMetaObject::invokeMethod(
            this, [this, valid, document, pageSizes]() { onDocumentLoaded(valid, document, pageSizes); },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::onDocumentLoaded(bool valid, const std::shared_ptr<PdfDocument> &document,
                                        const QVector<QSizeF> &pageSizes)
{
    delete m_loadingLabel;
    m_loadingLabel = nullptr;

    m_valid = valid;
    m_document = document;

    if (!valid) {
        qWarning() << "Failed to open PDF for viewing:" << m_filePath;
        auto *errorLabel = new QLabel(tr("Could not open this PDF file."), m_pagesContainer);
        errorLabel->setAlignment(Qt::AlignCenter);
        m_pagesContainer->layout()->addWidget(errorLabel);
        emit documentLoaded(false, 0);
        return;
    }

    buildPageLabels(pageSizes);
    connect(m_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &PdfViewerWidget::renderVisiblePages);
    QTimer::singleShot(0, this, &PdfViewerWidget::renderVisiblePages);

    emit documentLoaded(true, m_pageLabels.size());
}

void PdfViewerWidget::buildPageLabels(const QVector<QSizeF> &pageSizes)
{
    auto *layout = qobject_cast<QVBoxLayout *>(m_pagesContainer->layout());
    layout->setSpacing(kPageSpacing);
    layout->setContentsMargins(kPageSpacing, kPageSpacing, kPageSpacing, kPageSpacing);

    const int pageCount = pageSizes.size();
    m_pageLabels.reserve(pageCount);
    m_pageRendered.resize(pageCount);

    for (int i = 0; i < pageCount; ++i) {
        const QSizeF sizePt = pageSizes.at(i);

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
    if (!m_valid || m_pageLabels.isEmpty())
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

        if (!m_pageRendered[i] && labelRect.intersects(expanded))
            scheduleRender(i, label->width());
    }

    if (visiblePage != m_currentPageIndex) {
        m_currentPageIndex = visiblePage;
        emit currentPageChanged(m_currentPageIndex);
    }
}

void PdfViewerWidget::scheduleRender(int pageIndex, int widthPx)
{
    if (m_pagesLoading.contains(pageIndex))
        return;
    m_pagesLoading.insert(pageIndex);

    const std::shared_ptr<PdfDocument> document = m_document;
    QThread *thread = QThread::create([this, document, pageIndex, widthPx]() {
        const QImage image = document->renderPage(pageIndex, widthPx);
        QMetaObject::invokeMethod(
            this, [this, pageIndex, image]() { onPageRendered(pageIndex, image); }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::onPageRendered(int pageIndex, const QImage &image)
{
    m_pagesLoading.remove(pageIndex);

    if (pageIndex < 0 || pageIndex >= m_pageLabels.size())
        return;

    if (!image.isNull())
        m_pageLabels[pageIndex]->setPixmap(QPixmap::fromImage(image));
    else
        qWarning() << "Failed to render page" << pageIndex << "of" << m_filePath;
    m_pageRendered[pageIndex] = true;
}
