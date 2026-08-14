#include "pdfviewerwidget.h"

#include "pdfdocument.h"

#include <QActionGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMenu>
#include <QPainter>
#include <QPalette>
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
constexpr int kMinimumFittedColumnWidth = 280;
constexpr int kMinimumZoom = 10;
constexpr int kMaximumZoom = 400;
constexpr double kPdfPointToPixel = 96.0 / 72.0;
constexpr double kDefaultPageWidth = 595.0;
constexpr double kDefaultPageHeight = 842.0;

constexpr std::array<int, 13> kZoomSteps = {
    10, 25, 33, 50, 67, 75, 100, 125, 150, 200, 250, 300, 400,
};

QIcon viewModeIcon(bool continuous, const QColor &color)
{
    QPixmap pixmap(26, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.4));
    painter.setBrush(Qt::NoBrush);

    if (continuous) {
        painter.drawRoundedRect(QRectF(2.5, 1.5, 14, 5), 1, 1);
        painter.drawRoundedRect(QRectF(2.5, 7.5, 14, 5), 1, 1);
        painter.drawRoundedRect(QRectF(2.5, 13.5, 14, 5), 1, 1);
        painter.drawLine(QPointF(21, 3), QPointF(21, 17));
        painter.drawLine(QPointF(18.5, 14.5), QPointF(21, 17));
        painter.drawLine(QPointF(23.5, 14.5), QPointF(21, 17));
    } else {
        painter.drawRoundedRect(QRectF(1.5, 1.5, 23, 17), 2, 2);
        painter.drawRect(QRectF(5, 4, 7, 12));
        painter.drawRect(QRectF(14, 4, 7, 12));
    }
    return QIcon(pixmap);
}

QIcon fitModeIcon(int mode, const QColor &color)
{
    QPixmap pixmap(26, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.4));
    painter.setBrush(Qt::NoBrush);

    if (mode == 0) { // Fit page
        painter.drawRoundedRect(QRectF(7, 1.5, 12, 17), 1, 1);
        painter.drawLine(QPointF(3, 5), QPointF(3, 2));
        painter.drawLine(QPointF(3, 2), QPointF(6, 2));
        painter.drawLine(QPointF(23, 5), QPointF(23, 2));
        painter.drawLine(QPointF(23, 2), QPointF(20, 2));
        painter.drawLine(QPointF(3, 15), QPointF(3, 18));
        painter.drawLine(QPointF(3, 18), QPointF(6, 18));
        painter.drawLine(QPointF(23, 15), QPointF(23, 18));
        painter.drawLine(QPointF(23, 18), QPointF(20, 18));
    } else if (mode == 1) { // Fit width
        painter.drawRoundedRect(QRectF(6.5, 1.5, 13, 17), 1, 1);
        painter.drawLine(QPointF(2, 10), QPointF(24, 10));
        painter.drawLine(QPointF(2, 10), QPointF(5, 7));
        painter.drawLine(QPointF(2, 10), QPointF(5, 13));
        painter.drawLine(QPointF(24, 10), QPointF(21, 7));
        painter.drawLine(QPointF(24, 10), QPointF(21, 13));
    } else if (mode == 2) { // Two columns
        painter.drawRoundedRect(QRectF(2.5, 1.5, 9, 17), 1, 1);
        painter.drawRoundedRect(QRectF(14.5, 1.5, 9, 17), 1, 1);
    } else { // Generic fit selector
        painter.drawRoundedRect(QRectF(7, 2, 12, 16), 1, 1);
        painter.drawLine(QPointF(3, 10), QPointF(6, 10));
        painter.drawLine(QPointF(20, 10), QPointF(23, 10));
    }
    return QIcon(pixmap);
}
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

    const QColor baseColor = palette().color(QPalette::Base);
    const QColor canvasColor = baseColor.lightness() >= 128
        ? QColor(QStringLiteral("#c9ccd1"))
        : QColor(QStringLiteral("#303236"));
    QPalette pagesPalette = m_pagesContainer->palette();
    pagesPalette.setColor(QPalette::Window, canvasColor);
    m_pagesContainer->setPalette(pagesPalette);
    m_pagesContainer->setAutoFillBackground(true);

    m_pagesLayout->setContentsMargins(kPageSpacing, kPageSpacing, kPageSpacing, kPageSpacing);
    m_pagesLayout->setHorizontalSpacing(kPageSpacing);
    m_pagesLayout->setVerticalSpacing(kPageSpacing);
    m_pagesLayout->setSizeConstraint(QLayout::SetMinimumSize);

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
    toolbar->setIconSize(QSize(24, 20));
    layout()->addWidget(toolbar);

    const QColor iconColor = palette().color(QPalette::Text);

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

    m_zoomOutButton = new QToolButton(toolbar);
    m_zoomOutButton->setText(QStringLiteral("−"));
    m_zoomOutButton->setToolTip(tr("Zoom Out"));
    toolbar->addWidget(m_zoomOutButton);

    m_zoomCombo = new QComboBox(toolbar);
    m_zoomCombo->setEditable(true);
    m_zoomCombo->setInsertPolicy(QComboBox::NoInsert);
    m_zoomCombo->setFixedWidth(82);
    m_zoomCombo->setToolTip(tr("Zoom"));
    for (const int percent : {50, 75, 100, 125, 150, 200})
        m_zoomCombo->addItem(QStringLiteral("%1%").arg(percent), -percent);
    toolbar->addWidget(m_zoomCombo);

    m_zoomInButton = new QToolButton(toolbar);
    m_zoomInButton->setText(QStringLiteral("+"));
    m_zoomInButton->setToolTip(tr("Zoom In"));
    toolbar->addWidget(m_zoomInButton);

    toolbar->addSeparator();

    m_viewModeButton = new QToolButton(toolbar);
    m_viewModeButton->setPopupMode(QToolButton::InstantPopup);
    m_viewModeButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_viewModeButton->setToolTip(tr("View Mode"));
    auto *viewMenu = new QMenu(m_viewModeButton);
    auto *viewGroup = new QActionGroup(viewMenu);
    viewGroup->setExclusive(true);
    m_continuousAction = viewMenu->addAction(
        viewModeIcon(true, iconColor), tr("Continuous"));
    m_discreteAction = viewMenu->addAction(
        viewModeIcon(false, iconColor), tr("Discrete"));
    for (QAction *action : {m_continuousAction, m_discreteAction}) {
        action->setCheckable(true);
        viewGroup->addAction(action);
    }
    m_viewModeButton->setMenu(viewMenu);
    toolbar->addWidget(m_viewModeButton);

    m_zoomFitButton = new QToolButton(toolbar);
    m_zoomFitButton->setPopupMode(QToolButton::InstantPopup);
    m_zoomFitButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_zoomFitButton->setToolTip(tr("Page Fit"));
    auto *fitMenu = new QMenu(m_zoomFitButton);
    auto *fitGroup = new QActionGroup(fitMenu);
    fitGroup->setExclusive(true);
    m_fitPageAction = fitMenu->addAction(
        fitModeIcon(0, iconColor), tr("Fit Page"));
    m_fitWidthAction = fitMenu->addAction(
        fitModeIcon(1, iconColor), tr("Fit Width"));
    m_fitTwoColumnsAction = fitMenu->addAction(
        fitModeIcon(2, iconColor), tr("Fit Two Columns"));
    for (QAction *action : {m_fitPageAction, m_fitWidthAction, m_fitTwoColumnsAction}) {
        action->setCheckable(true);
        fitGroup->addAction(action);
    }
    m_zoomFitButton->setMenu(fitMenu);
    toolbar->addWidget(m_zoomFitButton);

    m_previousPageButton->setEnabled(false);
    m_pageSpinBox->setEnabled(false);
    m_nextPageButton->setEnabled(false);

    connect(m_previousPageButton, &QToolButton::clicked, this,
            [this]() { navigateByPageGroup(-1); });
    connect(m_nextPageButton, &QToolButton::clicked, this,
            [this]() { navigateByPageGroup(1); });
    connect(m_pageSpinBox, &QSpinBox::valueChanged, this, [this](int pageNumber) {
        if (!m_updatingControls)
            goToPage(pageNumber - 1);
    });
    connect(m_continuousAction, &QAction::triggered, this,
            [this]() { setPageLayout(PageLayout::Continuous); });
    connect(m_discreteAction, &QAction::triggered, this,
            [this]() { setPageLayout(PageLayout::Discrete); });
    connect(m_zoomOutButton, &QToolButton::clicked, this, [this]() { zoomByStep(-1); });
    connect(m_zoomInButton, &QToolButton::clicked, this, [this]() { zoomByStep(1); });
    connect(m_zoomCombo, &QComboBox::activated, this, &PdfViewerWidget::activateZoomSelection);
    connect(m_zoomCombo->lineEdit(), &QLineEdit::editingFinished,
            this, &PdfViewerWidget::applyTypedZoom);
    connect(m_fitPageAction, &QAction::triggered, this, [this]() {
        m_zoomMode = ZoomMode::FitPage;
        applyZoom();
    });
    connect(m_fitWidthAction, &QAction::triggered, this, [this]() {
        m_zoomMode = ZoomMode::FitWidth;
        applyZoom();
    });
    connect(m_fitTwoColumnsAction, &QAction::triggered, this, [this]() {
        m_zoomMode = ZoomMode::FitTwoColumns;
        applyZoom();
    });

    m_zoomMode = ZoomMode::FitWidth;
    syncViewControl();
    syncFitControl();
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
    // A single Qt layout tops out at QLAYOUTSIZE_MAX (roughly 524k px).
    // Long documents can exceed that at ordinary zoom levels, so valid PDF
    // pages are positioned manually on the scroll canvas instead.
    m_pagesLayout->setEnabled(false);
    m_pagesLayout->setSizeConstraint(QLayout::SetNoConstraint);
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
        label->setFrameShape(QFrame::NoFrame);
        label->setStyleSheet(QStringLiteral("background-color: palette(base); border: none;"));
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
    while (QLayoutItem *item = m_pagesLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->hide();
        delete item;
    }

    if (m_pageLabels.isEmpty())
        return;

    for (QLabel *label : m_pageLabels)
        label->hide();

    QVector<int> visiblePages;
    int columns = 1;
    if (m_pageLayout == PageLayout::Continuous) {
        columns = availableColumnCount();
        visiblePages.reserve(m_pageLabels.size());
        for (int i = 0; i < m_pageLabels.size(); ++i) {
            visiblePages.append(i);
        }
    } else {
        const QSize grid = discreteGridShape();
        columns = grid.width();
        const int firstPage = discreteChunkStart();
        const int lastPage = qMin(firstPage + columns * grid.height(), m_pageLabels.size());
        visiblePages.reserve(lastPage - firstPage);
        for (int i = firstPage; i < lastPage; ++i)
            visiblePages.append(i);
    }

    if (visiblePages.isEmpty())
        return;

    columns = qBound(1, columns, visiblePages.size());
    const int rows = (visiblePages.size() + columns - 1) / columns;
    QVector<int> columnWidths(columns, 0);
    QVector<int> rowHeights(rows, 0);
    for (int offset = 0; offset < visiblePages.size(); ++offset) {
        const QLabel *label = m_pageLabels[visiblePages[offset]];
        const int column = offset % columns;
        const int row = offset / columns;
        columnWidths[column] = qMax(columnWidths[column], label->width());
        rowHeights[row] = qMax(rowHeights[row], label->height());
    }

    int contentWidth = 0;
    for (const int width : columnWidths)
        contentWidth += width;
    contentWidth += (columns - 1) * kPageSpacing;

    const int viewportWidth = qMax(1, m_scrollArea->viewport()->width());
    const int viewportHeight = qMax(1, m_scrollArea->viewport()->height());
    const int canvasWidth = qMax(viewportWidth, contentWidth + 2 * kPageSpacing);
    QVector<int> columnLeft(columns, 0);
    int x = (canvasWidth - contentWidth) / 2;
    for (int column = 0; column < columns; ++column) {
        columnLeft[column] = x;
        x += columnWidths[column] + kPageSpacing;
    }

    int y = kPageSpacing;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const int offset = row * columns + column;
            if (offset >= visiblePages.size())
                break;
            QLabel *label = m_pageLabels[visiblePages[offset]];
            const int pageX = columnLeft[column]
                + (columnWidths[column] - label->width()) / 2;
            label->setGeometry(pageX, y, label->width(), label->height());
            label->show();
        }
        y += rowHeights[row] + kPageSpacing;
    }

    const int canvasHeight = qMax(viewportHeight, y);
    m_pagesContainer->setMinimumSize(0, 0);
    m_pagesContainer->setMinimumSize(canvasWidth, canvasHeight);
    m_pagesContainer->updateGeometry();
}

int PdfViewerWidget::availableColumnCount() const
{
    if (m_pageLabels.isEmpty())
        return 1;

    const int availableWidth = qMax(1, m_scrollArea->viewport()->width() - 2 * kPageSpacing);
    const int referenceIndex = qBound(0, m_currentPageIndex, m_pageLabels.size() - 1);
    const int pageWidth = qMax(1, m_pageLabels[referenceIndex]->width());
    return qMax(1, (availableWidth + kPageSpacing) / (pageWidth + kPageSpacing));
}

int PdfViewerWidget::fitTwoColumnCount() const
{
    const int availableWidth = qMax(1, m_scrollArea->viewport()->width() - 2 * kPageSpacing);
    const int widthNeededForTwo = 2 * kMinimumFittedColumnWidth + kPageSpacing;
    return availableWidth >= widthNeededForTwo ? 2 : 1;
}

QSize PdfViewerWidget::discreteGridShape() const
{
    if (m_pageLabels.isEmpty())
        return QSize(1, 1);

    const int availableHeight = qMax(1, m_scrollArea->viewport()->height() - 2 * kPageSpacing);
    const int referenceIndex = qBound(0, m_currentPageIndex, m_pageLabels.size() - 1);
    const int pageHeight = qMax(1, m_pageLabels[referenceIndex]->height());
    const int columns = availableColumnCount();
    const int rows = qMax(1, (availableHeight + kPageSpacing) / (pageHeight + kPageSpacing));
    return QSize(columns, rows);
}

int PdfViewerWidget::discretePageCount() const
{
    const QSize grid = discreteGridShape();
    return qMax(1, grid.width() * grid.height());
}

int PdfViewerWidget::discreteChunkStart() const
{
    const int count = discretePageCount();
    return qMax(0, (m_currentPageIndex / count) * count);
}

void PdfViewerWidget::setPageLayout(PageLayout layout)
{
    if (!m_valid || m_pageLayout == layout)
        return;

    m_pageLayout = layout;

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
    } else if (m_zoomMode == ZoomMode::FitTwoColumns) {
        const int columns = fitTwoColumnCount();
        const int gapsWidth = (columns - 1) * kPageSpacing;
        const int columnWidth = qMax(1, (availableWidth - gapsWidth) / columns);
        scale = columnWidth / referenceSize.width();
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
    syncFitControl();
    syncViewControl();
    syncNavigationControls();
    QTimer::singleShot(0, this, [this]() {
        rebuildPageLayout();
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

void PdfViewerWidget::setZoomPercent(int percent)
{
    setCustomZoom(percent);
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
    if (value < 0)
        setCustomZoom(-value);
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
    const int displayedPercent = m_zoomMode == ZoomMode::Custom
        ? m_zoomPercent
        : m_effectiveZoomPercent;
    m_zoomCombo->setEditText(QStringLiteral("%1%").arg(displayedPercent));
    m_updatingControls = false;

    if (displayedPercent != m_lastReportedZoomPercent) {
        m_lastReportedZoomPercent = displayedPercent;
        emit zoomPercentChanged(displayedPercent);
    }
}

void PdfViewerWidget::syncViewControl()
{
    if (!m_viewModeButton)
        return;

    const bool continuous = m_pageLayout == PageLayout::Continuous;
    m_continuousAction->setChecked(continuous);
    m_discreteAction->setChecked(!continuous);
    QAction *active = continuous ? m_continuousAction : m_discreteAction;
    m_viewModeButton->setIcon(active->icon());
    m_viewModeButton->setText(active->text());
}

void PdfViewerWidget::syncFitControl()
{
    if (!m_zoomFitButton)
        return;

    QAction *active = nullptr;
    if (m_zoomMode == ZoomMode::FitPage)
        active = m_fitPageAction;
    else if (m_zoomMode == ZoomMode::FitWidth)
        active = m_fitWidthAction;
    else if (m_zoomMode == ZoomMode::FitTwoColumns)
        active = m_fitTwoColumnsAction;

    for (QAction *action : {m_fitPageAction, m_fitWidthAction, m_fitTwoColumnsAction})
        action->setChecked(action == active);

    if (active) {
        m_zoomFitButton->setIcon(active->icon());
        if (m_zoomMode == ZoomMode::FitPage)
            m_zoomFitButton->setText(tr("Page"));
        else if (m_zoomMode == ZoomMode::FitWidth)
            m_zoomFitButton->setText(tr("Width"));
        else
            m_zoomFitButton->setText(tr("Two Columns"));
        m_zoomFitButton->setToolTip(active->text());
    } else {
        m_zoomFitButton->setIcon(fitModeIcon(3, palette().color(QPalette::Text)));
        m_zoomFitButton->setText(tr("Fit"));
        m_zoomFitButton->setToolTip(tr("Page Fit"));
    }
}

void PdfViewerWidget::navigateByPageGroup(int direction)
{
    if (m_pageLabels.isEmpty() || direction == 0)
        return;

    if (m_pageLayout == PageLayout::Discrete) {
        const int firstPage = discreteChunkStart();
        const int step = discretePageCount();
        goToPage(direction > 0 ? firstPage + step : qMax(0, firstPage - step));
    } else {
        goToPage(m_currentPageIndex + (direction > 0 ? 1 : -1));
    }
}

void PdfViewerWidget::syncNavigationControls()
{
    if (!m_pageSpinBox || m_pageLabels.isEmpty())
        return;

    m_updatingControls = true;
    m_pageSpinBox->setValue(m_currentPageIndex + 1);
    m_updatingControls = false;
    if (m_pageLayout == PageLayout::Discrete) {
        const int firstPage = discreteChunkStart();
        m_previousPageButton->setEnabled(firstPage > 0);
        m_nextPageButton->setEnabled(firstPage + discretePageCount() < m_pageLabels.size());
    } else {
        m_previousPageButton->setEnabled(m_currentPageIndex > 0);
        m_nextPageButton->setEnabled(m_currentPageIndex + 1 < m_pageLabels.size());
    }
}

bool PdfViewerWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_scrollArea->viewport() && event->type() == QEvent::Wheel) {
        auto *wheelEvent = static_cast<QWheelEvent *>(event);
        if (wheelEvent->modifiers().testFlag(Qt::ControlModifier)) {
            zoomByStep(wheelEvent->angleDelta().y() >= 0 ? 1 : -1);
            return true;
        }
        if (m_pageLayout == PageLayout::Discrete) {
            QScrollBar *bar = m_scrollArea->verticalScrollBar();
            const bool scrollingDown = wheelEvent->angleDelta().y() < 0;
            const bool atBoundary = bar->maximum() == 0
                || (scrollingDown && bar->value() >= bar->maximum())
                || (!scrollingDown && bar->value() <= bar->minimum());
            if (atBoundary) {
                navigateByPageGroup(scrollingDown ? 1 : -1);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PdfViewerWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!m_valid)
        return;

    if (m_zoomMode == ZoomMode::FitPage || m_zoomMode == ZoomMode::FitWidth
        || m_zoomMode == ZoomMode::FitTwoColumns) {
        applyZoom();
    } else {
        rebuildPageLayout();
        syncNavigationControls();
        QTimer::singleShot(0, this, &PdfViewerWidget::renderVisiblePages);
    }
}

void PdfViewerWidget::goToPage(int pageIndex)
{
    if (pageIndex < 0 || pageIndex >= m_pageLabels.size())
        return;

    const bool pageChanged = pageIndex != m_currentPageIndex;
    m_currentPageIndex = pageIndex;

    if (m_pageLayout == PageLayout::Discrete && pageChanged
        && (m_zoomMode == ZoomMode::FitPage || m_zoomMode == ZoomMode::FitWidth
            || m_zoomMode == ZoomMode::FitTwoColumns)) {
        applyZoom();
    } else if (m_pageLayout == PageLayout::Discrete && pageChanged) {
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

        const bool belongsToVisibleChunk = m_pageLayout == PageLayout::Discrete;
        if (m_renderedWidths[i] != label->width()
            && (belongsToVisibleChunk || labelRect.intersects(expanded))) {
            scheduleRender(i, label->width());
        }
    }

    if (m_pageLayout == PageLayout::Continuous && visiblePage != m_currentPageIndex) {
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

    if (image.height() != label->height()) {
        label->setFixedHeight(image.height());
        if (pageIndex < m_pageSizes.size() && image.width() > 0) {
            QSizeF correctedSize = m_pageSizes[pageIndex];
            correctedSize.setHeight(correctedSize.width() * image.height() / image.width());
            m_pageSizes[pageIndex] = correctedSize;
        }
        rebuildPageLayout();
    }
    label->setPixmap(QPixmap::fromImage(image));
    m_renderedWidths[pageIndex] = widthPx;
}
