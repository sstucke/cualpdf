#include "pdfviewerwidget.h"

#include "pdfdocument.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <array>

namespace {
constexpr int kPageSpacing = 12;
constexpr int kMinimumZoom = 10;
constexpr int kMaximumZoom = 400;
constexpr double kPdfPointToPixel = 96.0 / 72.0;
constexpr double kDefaultPageWidth = 595.0;
constexpr double kDefaultPageHeight = 842.0;

constexpr std::array<int, 13> kZoomSteps = {
    10, 25, 33, 50, 67, 75, 100, 125, 150, 200, 250, 300, 400,
};
}

PdfViewerWidget::PdfViewerWidget(const QString &filePath, int pageRenderWidth,
                                 QWidget *parent, bool showToolbar)
    : QWidget(parent)
    , m_filePath(filePath)
    , m_initialPageRenderWidth(pageRenderWidth)
    , m_showToolbar(showToolbar)
    , m_scrollArea(new QScrollArea(this))
    , m_pagesContainer(new QWidget)
    , m_pagesLayout(new QGridLayout(m_pagesContainer))
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    if (m_showToolbar)
        buildToolbar();

    outerLayout->addWidget(m_scrollArea);

    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_scrollArea->setWidget(m_pagesContainer);
    m_scrollArea->viewport()->installEventFilter(this);

    m_pagesLayout->setContentsMargins(kPageSpacing, kPageSpacing, kPageSpacing, kPageSpacing);
    m_pagesLayout->setHorizontalSpacing(kPageSpacing);
    m_pagesLayout->setVerticalSpacing(kPageSpacing);

    m_loadingLabel = new QLabel(tr("Loading…"), m_pagesContainer);
    m_loadingLabel->setAlignment(Qt::AlignCenter);
    m_pagesLayout->addWidget(m_loadingLabel, 0, 0);
    m_pagesLayout->setRowStretch(1, 1);

    startLoading();
}

PdfViewerWidget::~PdfViewerWidget() = default;

void PdfViewerWidget::buildToolbar()
{
    auto *toolbar = new QToolBar(this);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(16, 16));
    layout()->addWidget(toolbar);

    m_previousPageButton = new QToolButton(toolbar);
    m_previousPageButton->setIcon(style()->standardIcon(QStyle::SP_ArrowLeft));
    m_previousPageButton->setToolTip(tr("Previous Page"));
    toolbar->addWidget(m_previousPageButton);

    m_pageSpinBox = new QSpinBox(toolbar);
    m_pageSpinBox->setRange(1, 1);
    m_pageSpinBox->setFixedWidth(64);
    m_pageSpinBox->setAlignment(Qt::AlignRight);
    m_pageSpinBox->setToolTip(tr("Page"));
    toolbar->addWidget(m_pageSpinBox);

    m_pageCountLabel = new QLabel(QStringLiteral("/ —"), toolbar);
    m_pageCountLabel->setContentsMargins(4, 0, 8, 0);
    toolbar->addWidget(m_pageCountLabel);

    m_nextPageButton = new QToolButton(toolbar);
    m_nextPageButton->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
    m_nextPageButton->setToolTip(tr("Next Page"));
    toolbar->addWidget(m_nextPageButton);

    toolbar->addSeparator();

    m_pageLayoutCombo = new QComboBox(toolbar);
    m_pageLayoutCombo->setToolTip(tr("Page Layout"));
    m_pageLayoutCombo->addItem(tr("Single Page"), static_cast<int>(PageLayout::SinglePage));
    m_pageLayoutCombo->addItem(tr("Continuous"), static_cast<int>(PageLayout::Continuous));
    m_pageLayoutCombo->addItem(tr("Multiple Columns"),
                               static_cast<int>(PageLayout::MultipleColumns));
    m_pageLayoutCombo->setCurrentIndex(1);
    toolbar->addWidget(m_pageLayoutCombo);

    toolbar->addSeparator();

    m_zoomOutButton = new QToolButton(toolbar);
    m_zoomOutButton->setText(QStringLiteral("−"));
    m_zoomOutButton->setToolTip(tr("Zoom Out"));
    toolbar->addWidget(m_zoomOutButton);

    m_zoomCombo = new QComboBox(toolbar);
    m_zoomCombo->setEditable(true);
    m_zoomCombo->setInsertPolicy(QComboBox::NoInsert);
    m_zoomCombo->setFixedWidth(130);
    m_zoomCombo->setToolTip(tr("Zoom"));
    m_zoomCombo->addItem(tr("Fit Page"), static_cast<int>(ZoomMode::FitPage));
    m_zoomCombo->addItem(tr("Fit Width"), static_cast<int>(ZoomMode::FitWidth));
    for (const int percent : {50, 75, 100, 125, 150, 200})
        m_zoomCombo->addItem(QStringLiteral("%1%").arg(percent), -percent);
    toolbar->addWidget(m_zoomCombo);

    m_zoomInButton = new QToolButton(toolbar);
    m_zoomInButton->setText(QStringLiteral("+"));
    m_zoomInButton->setToolTip(tr("Zoom In"));
    toolbar->addWidget(m_zoomInButton);

    m_previousPageButton->setEnabled(false);
    m_pageSpinBox->setEnabled(false);
    m_nextPageButton->setEnabled(false);

    connect(m_previousPageButton, &QToolButton::clicked, this,
            [this]() { goToPage(m_currentPageIndex - 1); });
    connect(m_nextPageButton, &QToolButton::clicked, this,
            [this]() { goToPage(m_currentPageIndex + 1); });
    connect(m_pageSpinBox, &QSpinBox::valueChanged, this, [this](int pageNumber) {
        if (!m_updatingControls)
            goToPage(pageNumber - 1);
    });
    connect(m_pageLayoutCombo, &QComboBox::activated, this, [this](int index) {
        setPageLayout(static_cast<PageLayout>(m_pageLayoutCombo->itemData(index).toInt()));
    });
    connect(m_zoomOutButton, &QToolButton::clicked, this, [this]() { zoomByStep(-1); });
    connect(m_zoomInButton, &QToolButton::clicked, this, [this]() { zoomByStep(1); });
    connect(m_zoomCombo, &QComboBox::activated, this, &PdfViewerWidget::activateZoomSelection);
    connect(m_zoomCombo->lineEdit(), &QLineEdit::editingFinished,
            this, &PdfViewerWidget::applyTypedZoom);

    m_zoomMode = ZoomMode::FitWidth;
    syncZoomControl();
}

void PdfViewerWidget::startLoading()
{
    const QPointer<PdfViewerWidget> weakSelf(this);
    QThread *thread = QThread::create([weakSelf, path = m_filePath]() {
        auto document = std::make_shared<PdfDocument>(path);
        QVector<QSizeF> pageSizes;
        if (document->isValid() && document->pageCount() > 0)
            pageSizes = document->allPageSizes();

        QMetaObject::invokeMethod(
            qApp,
            [weakSelf, document, pageSizes]() {
                if (weakSelf)
                    weakSelf->onDocumentLoaded(!pageSizes.isEmpty(), document, pageSizes);
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::onDocumentLoaded(bool valid,
                                       const std::shared_ptr<PdfDocument> &document,
                                       const QVector<QSizeF> &pageSizes)
{
    delete m_loadingLabel;
    m_loadingLabel = nullptr;

    m_valid = valid;
    m_document = document;
    m_pageSizes = pageSizes;

    if (!valid) {
        qWarning() << "Failed to open PDF for viewing:" << m_filePath;
        auto *errorLabel = new QLabel(tr("Could not open this PDF file."), m_pagesContainer);
        errorLabel->setAlignment(Qt::AlignCenter);
        m_pagesLayout->addWidget(errorLabel, 0, 0);
        emit documentLoaded(false, 0);
        return;
    }

    m_currentPageIndex = 0;
    buildPageLabels(pageSizes.size());
    applyZoom();

    connect(m_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &PdfViewerWidget::renderVisiblePages);
    connect(m_scrollArea->horizontalScrollBar(), &QScrollBar::valueChanged, this,
            &PdfViewerWidget::renderVisiblePages);

    if (m_pageSpinBox) {
        m_pageSpinBox->setRange(1, pageSizes.size());
        m_pageSpinBox->setEnabled(true);
        m_pageCountLabel->setText(QStringLiteral("/ %1").arg(pageSizes.size()));
    }
    syncNavigationControls();

    QTimer::singleShot(0, this, &PdfViewerWidget::renderVisiblePages);
    emit documentLoaded(true, m_pageLabels.size());
}

void PdfViewerWidget::buildPageLabels(int pageCount)
{
    m_pageLabels.reserve(pageCount);
    m_renderedWidths.fill(0, pageCount);

    for (int i = 0; i < pageCount; ++i) {
        auto *label = new QLabel(m_pagesContainer);
        label->setFrameShape(QFrame::Box);
        label->setStyleSheet(
            QStringLiteral("background-color: palette(base); border: 1px solid palette(mid);"));
        label->setAlignment(Qt::AlignCenter);
        label->hide();
        m_pageLabels.append(label);
    }
}

QSizeF PdfViewerWidget::pageSize(int pageIndex) const
{
    if (pageIndex >= 0 && pageIndex < m_pageSizes.size()) {
        const QSizeF size = m_pageSizes.at(pageIndex);
        if (size.width() > 0.0 && size.height() > 0.0)
            return size;
    }
    return QSizeF(kDefaultPageWidth, kDefaultPageHeight);
}

void PdfViewerWidget::rebuildPageLayout()
{
    for (int row = 0; row < m_pagesLayout->rowCount(); ++row)
        m_pagesLayout->setRowStretch(row, 0);
    for (int column = 0; column < m_pagesLayout->columnCount(); ++column)
        m_pagesLayout->setColumnStretch(column, 0);

    while (QLayoutItem *item = m_pagesLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->hide();
        delete item;
    }

    if (m_pageLabels.isEmpty())
        return;

    if (m_pageLayout == PageLayout::SinglePage) {
        QLabel *label = m_pageLabels[m_currentPageIndex];
        label->show();
        m_pagesLayout->addWidget(label, 0, 0, Qt::AlignHCenter | Qt::AlignTop);
        m_pagesLayout->setRowStretch(1, 1);
    } else if (m_pageLayout == PageLayout::Continuous) {
        for (int i = 0; i < m_pageLabels.size(); ++i) {
            m_pageLabels[i]->show();
            m_pagesLayout->addWidget(m_pageLabels[i], i, 0, Qt::AlignHCenter | Qt::AlignTop);
        }
        m_pagesLayout->setRowStretch(m_pageLabels.size(), 1);
    } else {
        const int columns = multiColumnCount();
        for (int i = 0; i < m_pageLabels.size(); ++i) {
            m_pageLabels[i]->show();
            m_pagesLayout->addWidget(m_pageLabels[i], i / columns, i % columns,
                                     Qt::AlignHCenter | Qt::AlignTop);
        }
        m_pagesLayout->setRowStretch((m_pageLabels.size() + columns - 1) / columns, 1);
    }

    m_pagesLayout->invalidate();
}

int PdfViewerWidget::multiColumnCount() const
{
    if (m_pageLabels.isEmpty())
        return 1;

    const int availableWidth = qMax(1, m_scrollArea->viewport()->width() - 2 * kPageSpacing);
    const int referenceIndex = qBound(0, m_currentPageIndex, m_pageLabels.size() - 1);
    const int pageWidth = qMax(1, m_pageLabels[referenceIndex]->width());
    return qMax(1, (availableWidth + kPageSpacing) / (pageWidth + kPageSpacing));
}

void PdfViewerWidget::setPageLayout(PageLayout layout)
{
    if (!m_valid || m_pageLayout == layout)
        return;

    m_pageLayout = layout;

    if (layout == PageLayout::MultipleColumns) {
        const QSizeF referenceSize = pageSize(m_currentPageIndex);
        const int availableWidth = qMax(1, m_scrollArea->viewport()->width() - 3 * kPageSpacing);
        const double targetPageWidth = availableWidth / 2.0;
        m_zoomPercent = qBound(
            kMinimumZoom,
            qRound(100.0 * targetPageWidth / (referenceSize.width() * kPdfPointToPixel)),
            kMaximumZoom);
        m_zoomMode = ZoomMode::Custom;
    }

    applyZoom();
}

void PdfViewerWidget::applyZoom()
{
    if (!m_valid || m_pageLabels.isEmpty())
        return;

    const int referenceIndex = qBound(0, m_currentPageIndex, m_pageLabels.size() - 1);
    const QSizeF referenceSize = pageSize(referenceIndex);
    const int availableWidth = qMax(1, m_scrollArea->viewport()->width() - 2 * kPageSpacing);
    const int availableHeight = qMax(1, m_scrollArea->viewport()->height() - 2 * kPageSpacing);

    double scale = kPdfPointToPixel * m_zoomPercent / 100.0;
    if (m_zoomMode == ZoomMode::FitWidth) {
        scale = availableWidth / referenceSize.width();
    } else if (m_zoomMode == ZoomMode::FitPage) {
        scale = qMin(availableWidth / referenceSize.width(),
                     availableHeight / referenceSize.height());
    } else if (m_zoomMode == ZoomMode::FixedWidth) {
        scale = m_initialPageRenderWidth / referenceSize.width();
    }

    m_effectiveZoomPercent = qBound(
        kMinimumZoom, qRound(100.0 * scale / kPdfPointToPixel), kMaximumZoom);

    for (int i = 0; i < m_pageLabels.size(); ++i) {
        const QSizeF size = pageSize(i);
        int width = qMax(1, qRound(size.width() * scale));
        if (m_zoomMode == ZoomMode::FixedWidth)
            width = m_initialPageRenderWidth;
        else if (m_zoomMode == ZoomMode::FitWidth)
            width = availableWidth;
        const int height = qMax(1, qRound(width * size.height() / size.width()));
        QLabel *label = m_pageLabels[i];
        if (label->width() != width || label->height() != height) {
            label->clear();
            label->setFixedSize(width, height);
            m_renderedWidths[i] = 0;
        }
    }

    rebuildPageLayout();
    syncZoomControl();
    syncNavigationControls();
    QTimer::singleShot(0, this, [this]() {
        scrollToCurrentPage();
        renderVisiblePages();
    });
}

void PdfViewerWidget::setCustomZoom(int percent)
{
    m_zoomMode = ZoomMode::Custom;
    m_zoomPercent = qBound(kMinimumZoom, percent, kMaximumZoom);
    applyZoom();
}

void PdfViewerWidget::zoomByStep(int direction)
{
    const int current = m_zoomMode == ZoomMode::Custom ? m_zoomPercent : m_effectiveZoomPercent;
    int target = current;

    if (direction > 0) {
        for (const int step : kZoomSteps) {
            if (step > current) {
                target = step;
                break;
            }
        }
    } else {
        for (auto it = kZoomSteps.rbegin(); it != kZoomSteps.rend(); ++it) {
            if (*it < current) {
                target = *it;
                break;
            }
        }
    }

    setCustomZoom(target);
}

void PdfViewerWidget::activateZoomSelection(int index)
{
    const int value = m_zoomCombo->itemData(index).toInt();
    if (value == static_cast<int>(ZoomMode::FitPage)) {
        m_zoomMode = ZoomMode::FitPage;
        applyZoom();
    } else if (value == static_cast<int>(ZoomMode::FitWidth)) {
        m_zoomMode = ZoomMode::FitWidth;
        applyZoom();
    } else if (value < 0) {
        setCustomZoom(-value);
    }
}

void PdfViewerWidget::applyTypedZoom()
{
    if (!m_zoomCombo || m_updatingControls)
        return;

    QString text = m_zoomCombo->currentText().trimmed();
    text.remove(QLatin1Char('%'));
    bool ok = false;
    const int percent = qRound(text.toDouble(&ok));
    if (ok)
        setCustomZoom(percent);
    else
        syncZoomControl();
}

void PdfViewerWidget::syncZoomControl()
{
    if (!m_zoomCombo)
        return;

    m_updatingControls = true;
    if (m_zoomMode == ZoomMode::FitPage)
        m_zoomCombo->setCurrentIndex(0);
    else if (m_zoomMode == ZoomMode::FitWidth)
        m_zoomCombo->setCurrentIndex(1);
    else
        m_zoomCombo->setEditText(QStringLiteral("%1%").arg(m_zoomPercent));
    m_updatingControls = false;
}

void PdfViewerWidget::syncNavigationControls()
{
    if (!m_pageSpinBox || m_pageLabels.isEmpty())
        return;

    m_updatingControls = true;
    m_pageSpinBox->setValue(m_currentPageIndex + 1);
    m_updatingControls = false;
    m_previousPageButton->setEnabled(m_currentPageIndex > 0);
    m_nextPageButton->setEnabled(m_currentPageIndex + 1 < m_pageLabels.size());
}

bool PdfViewerWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_scrollArea->viewport() && event->type() == QEvent::Wheel) {
        auto *wheelEvent = static_cast<QWheelEvent *>(event);
        if (wheelEvent->modifiers().testFlag(Qt::ControlModifier)) {
            zoomByStep(wheelEvent->angleDelta().y() >= 0 ? 1 : -1);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PdfViewerWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!m_valid)
        return;

    if (m_zoomMode == ZoomMode::FitPage || m_zoomMode == ZoomMode::FitWidth)
        applyZoom();
    else if (m_pageLayout == PageLayout::MultipleColumns) {
        rebuildPageLayout();
        QTimer::singleShot(0, this, &PdfViewerWidget::renderVisiblePages);
    } else {
        renderVisiblePages();
    }
}

void PdfViewerWidget::goToPage(int pageIndex)
{
    if (pageIndex < 0 || pageIndex >= m_pageLabels.size())
        return;

    const bool pageChanged = pageIndex != m_currentPageIndex;
    m_currentPageIndex = pageIndex;

    if (m_pageLayout == PageLayout::SinglePage && pageChanged
        && m_zoomMode == ZoomMode::FitPage) {
        applyZoom();
    } else if (m_pageLayout == PageLayout::SinglePage && pageChanged) {
        rebuildPageLayout();
    }

    syncNavigationControls();
    scrollToCurrentPage();
    renderVisiblePages();

    if (pageChanged)
        emit currentPageChanged(m_currentPageIndex);
}

void PdfViewerWidget::scrollToCurrentPage()
{
    if (m_currentPageIndex < 0 || m_currentPageIndex >= m_pageLabels.size())
        return;

    QLabel *label = m_pageLabels[m_currentPageIndex];
    if (label->isHidden())
        return;

    const QPoint containerPosition = label->mapTo(m_pagesContainer, QPoint(0, 0));
    m_scrollArea->verticalScrollBar()->setValue(containerPosition.y() - kPageSpacing);
    m_scrollArea->horizontalScrollBar()->setValue(containerPosition.x() - kPageSpacing);
}

void PdfViewerWidget::renderVisiblePages()
{
    if (!m_valid || m_pageLabels.isEmpty())
        return;

    const QRect viewportRect = m_scrollArea->viewport()->rect();
    const int margin = qMax(viewportRect.height(), 1);
    const QRect expanded = viewportRect.adjusted(0, -margin, 0, margin);

    int visiblePage = m_currentPageIndex;
    bool foundVisible = false;

    for (int i = 0; i < m_pageLabels.size(); ++i) {
        QLabel *label = m_pageLabels[i];
        if (label->isHidden())
            continue;

        const QPoint topLeft = label->mapTo(m_scrollArea->viewport(), QPoint(0, 0));
        const QRect labelRect(topLeft, label->size());

        if (!foundVisible && labelRect.bottom() >= viewportRect.top()
            && labelRect.right() >= viewportRect.left()) {
            visiblePage = i;
            foundVisible = true;
        }

        if (m_renderedWidths[i] != label->width() && labelRect.intersects(expanded))
            scheduleRender(i, label->width());
    }

    if (visiblePage != m_currentPageIndex) {
        m_currentPageIndex = visiblePage;
        syncNavigationControls();
        emit currentPageChanged(m_currentPageIndex);
    }
}

void PdfViewerWidget::scheduleRender(int pageIndex, int widthPx)
{
    if (m_pagesLoading.value(pageIndex) == widthPx)
        return;
    m_pagesLoading.insert(pageIndex, widthPx);

    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    QThread *thread = QThread::create([weakSelf, document, pageIndex, widthPx]() {
        const QImage image = document->renderPage(pageIndex, widthPx);
        QMetaObject::invokeMethod(
            qApp,
            [weakSelf, pageIndex, widthPx, image]() {
                if (weakSelf)
                    weakSelf->onPageRendered(pageIndex, widthPx, image);
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::onPageRendered(int pageIndex, int widthPx, const QImage &image)
{
    if (m_pagesLoading.value(pageIndex) == widthPx)
        m_pagesLoading.remove(pageIndex);

    if (pageIndex < 0 || pageIndex >= m_pageLabels.size())
        return;

    QLabel *label = m_pageLabels[pageIndex];
    if (label->width() != widthPx)
        return;

    if (image.isNull()) {
        qWarning() << "Failed to render page" << pageIndex << "of" << m_filePath;
        m_renderedWidths[pageIndex] = widthPx;
        return;
    }

    if (image.height() != label->height())
        label->setFixedHeight(image.height());
    label->setPixmap(QPixmap::fromImage(image));
    m_renderedWidths[pageIndex] = widthPx;
}
