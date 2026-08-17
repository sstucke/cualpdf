#include "pdfviewerwidget.h"

#include "pdfdocument.h"

#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDir>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMenu>
#include <QMimeData>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSaveFile>
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
#include <algorithm>
#include <functional>

namespace {
constexpr auto kPageDragMimeType = "application/x-cualpdf-page-selection";
}

class PdfInsertionPlaceholder final : public QWidget
{
public:
    using ContextMenuHandler = std::function<void(int, const QPoint &)>;
    using DropHandler = std::function<void(int)>;

    PdfInsertionPlaceholder(int insertionIndex, QByteArray dragToken,
                            ContextMenuHandler contextMenuHandler,
                            DropHandler dropHandler, QWidget *parent)
        : QWidget(parent)
        , m_insertionIndex(insertionIndex)
        , m_dragToken(std::move(dragToken))
        , m_contextMenuHandler(std::move(contextMenuHandler))
        , m_dropHandler(std::move(dropHandler))
    {
        setAcceptDrops(true);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QCoreApplication::translate(
            "PdfViewerWidget", "Insert pages here"));
    }

    void setDragActive(bool active)
    {
        if (m_dragActive == active)
            return;
        m_dragActive = active;
        if (!active)
            m_dropTarget = false;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor accent(QStringLiteral("#2684ff"));
        const bool horizontal = width() >= height();
        const QPointF first = horizontal ? QPointF(4, height() / 2.0)
                                         : QPointF(width() / 2.0, 4);
        const QPointF second = horizontal ? QPointF(width() - 4, height() / 2.0)
                                          : QPointF(width() / 2.0, height() - 4);
        QColor lineColor = palette().color(QPalette::Mid);
        qreal lineWidth = 1.0;
        if (m_dragActive) {
            lineColor = accent;
            lineWidth = m_dropTarget ? 4.0 : 2.0;
            painter.fillRect(rect(), QColor(38, 132, 255, m_dropTarget ? 48 : 18));
        }
        painter.setPen(QPen(lineColor, lineWidth, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(first, second);

        const QPointF center = rect().center();
        painter.setBrush(m_dragActive ? accent : palette().color(QPalette::Button));
        painter.setPen(QPen(lineColor, 1.2));
        painter.drawEllipse(center, 6, 6);
        painter.setPen(QPen(m_dragActive ? Qt::white : lineColor, 1.3));
        painter.drawLine(center + QPointF(-3, 0), center + QPointF(3, 0));
        painter.drawLine(center + QPointF(0, -3), center + QPointF(0, 3));
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (m_contextMenuHandler)
            m_contextMenuHandler(m_insertionIndex, event->globalPos());
        event->accept();
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->data(kPageDragMimeType) == m_dragToken) {
            m_dropTarget = true;
            update();
            event->acceptProposedAction();
        }
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        m_dropTarget = false;
        update();
        event->accept();
    }

    void dropEvent(QDropEvent *event) override
    {
        if (event->mimeData()->data(kPageDragMimeType) != m_dragToken)
            return;
        m_dropTarget = false;
        update();
        event->acceptProposedAction();
        if (m_dropHandler)
            m_dropHandler(m_insertionIndex);
    }

private:
    int m_insertionIndex;
    QByteArray m_dragToken;
    ContextMenuHandler m_contextMenuHandler;
    DropHandler m_dropHandler;
    bool m_dragActive = false;
    bool m_dropTarget = false;
};

namespace {
constexpr int kPageSpacing = 12;
constexpr int kMinimumFittedColumnWidth = 280;
constexpr int kMinimumZoom = 10;
constexpr int kMaximumZoom = 400;
constexpr double kPdfPointToPixel = 96.0 / 72.0;
constexpr double kDefaultPageWidth = 595.0;
constexpr double kDefaultPageHeight = 842.0;

struct PageClipboard {
    QByteArray pageArchive;
    int pageCount = 0;
};

PageClipboard g_pageClipboard;

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

QIcon selectionModeIcon(bool pageMode, const QColor &color)
{
    QPixmap pixmap(24, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.5));
    painter.setBrush(Qt::NoBrush);

    if (pageMode) {
        painter.drawRoundedRect(QRectF(4, 1.5, 14, 17), 1.5, 1.5);
        QPolygonF pointer;
        pointer << QPointF(13, 9) << QPointF(22, 13) << QPointF(18, 15)
                << QPointF(16, 19);
        painter.setBrush(color);
        painter.drawPolygon(pointer);
    } else {
        QPen dashedPen(color, 1.5, Qt::DashLine);
        painter.setPen(dashedPen);
        painter.drawRect(QRectF(2.5, 2.5, 19, 15));
    }
    return QIcon(pixmap);
}

QIcon cropToolIcon(const QColor &color)
{
    QPixmap pixmap(28, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(QPen(color, 1.5, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(2.5, 2.5, 17, 16));

    painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
    painter.drawEllipse(QRectF(18, 13, 4, 4));
    painter.drawEllipse(QRectF(18, 18, 4, 4));
    painter.drawLine(QPointF(21.5, 15), QPointF(26, 10.5));
    painter.drawLine(QPointF(21.5, 20), QPointF(26, 23));
    return QIcon(pixmap);
}

QIcon rotationToolIcon(bool clockwise, const QColor &color)
{
    QPixmap pixmap(28, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    QPainterPath path;
    if (clockwise) {
        path.moveTo(6, 7);
        path.cubicTo(9, 2, 18, 1.5, 22, 7);
        path.cubicTo(25, 13, 20, 18, 12, 18);
        painter.drawPath(path);
        painter.drawLine(QPointF(12, 18), QPointF(15.5, 14.5));
        painter.drawLine(QPointF(12, 18), QPointF(15.5, 21.5));
    } else {
        path.moveTo(22, 7);
        path.cubicTo(19, 2, 10, 1.5, 6, 7);
        path.cubicTo(3, 13, 8, 18, 16, 18);
        painter.drawPath(path);
        painter.drawLine(QPointF(16, 18), QPointF(12.5, 14.5));
        painter.drawLine(QPointF(16, 18), QPointF(12.5, 21.5));
    }
    return QIcon(pixmap);
}

QString viewerText(const char *sourceText)
{
    return QCoreApplication::translate("PdfViewerWidget", sourceText);
}

class CropPreviewWidget final : public QWidget
{
public:
    explicit CropPreviewWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(300, 330);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setPage(const QPixmap &pixmap, const QSizeF &pageSizePoints)
    {
        m_pixmap = pixmap;
        m_pageSizePoints = pageSizePoints;
        update();
    }

    void setMargins(const QMarginsF &marginsPoints)
    {
        m_marginsPoints = marginsPoints;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), palette().color(QPalette::Window));

        if (m_pageSizePoints.width() <= 0.0 || m_pageSizePoints.height() <= 0.0)
            return;

        const QRectF available = QRectF(rect()).adjusted(18, 18, -18, -18);
        const double scale = qMin(available.width() / m_pageSizePoints.width(),
                                  available.height() / m_pageSizePoints.height());
        const QSizeF previewSize(m_pageSizePoints.width() * scale,
                                 m_pageSizePoints.height() * scale);
        const QRectF pageRect(available.center().x() - previewSize.width() / 2,
                              available.center().y() - previewSize.height() / 2,
                              previewSize.width(), previewSize.height());

        painter.fillRect(pageRect, Qt::white);
        if (!m_pixmap.isNull())
            painter.drawPixmap(pageRect.toRect(), m_pixmap);
        painter.setPen(QPen(QColor(QStringLiteral("#667085")), 1));
        painter.drawRect(pageRect);

        const QRectF cropRect(
            pageRect.left() + pageRect.width() * m_marginsPoints.left()
                                  / m_pageSizePoints.width(),
            pageRect.top() + pageRect.height() * m_marginsPoints.top()
                                 / m_pageSizePoints.height(),
            pageRect.width() * (m_pageSizePoints.width() - m_marginsPoints.left()
                                - m_marginsPoints.right()) / m_pageSizePoints.width(),
            pageRect.height() * (m_pageSizePoints.height() - m_marginsPoints.top()
                                 - m_marginsPoints.bottom()) / m_pageSizePoints.height());
        if (!cropRect.isValid() || cropRect.isEmpty())
            return;

        QPainterPath outside;
        outside.addRect(pageRect);
        QPainterPath inside;
        inside.addRect(cropRect);
        painter.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 80));
        painter.setPen(QPen(QColor(QStringLiteral("#2684ff")), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(cropRect);
    }

private:
    QPixmap m_pixmap;
    QSizeF m_pageSizePoints;
    QMarginsF m_marginsPoints;
};

class CropPagesDialog final : public QDialog
{
public:
    CropPagesDialog(const QPixmap &pagePixmap, const QSizeF &pageSizePoints,
                    const QMarginsF &selectionMargins, int currentPageIndex,
                    int pageCount, QWidget *parent = nullptr)
        : QDialog(parent)
        , m_pageSizePoints(pageSizePoints)
        , m_selectionMargins(selectionMargins)
        , m_marginsPoints(selectionMargins)
        , m_preview(new CropPreviewWidget(this))
        , m_sizeLabel(new QLabel(this))
        , m_unitsCombo(new QComboBox(this))
        , m_allPagesRadio(new QRadioButton(viewerText("All pages"), this))
        , m_rangeRadio(new QRadioButton(viewerText("From:"), this))
        , m_fromPageSpinBox(new QSpinBox(this))
        , m_toPageSpinBox(new QSpinBox(this))
        , m_subsetCombo(new QComboBox(this))
    {
        setWindowTitle(viewerText("Crop Pages"));
        resize(820, 570);

        auto *dialogLayout = new QVBoxLayout(this);
        auto *topLayout = new QHBoxLayout;

        auto *marginGroup = new QGroupBox(viewerText("Crop Box Margins"), this);
        auto *marginLayout = new QGridLayout(marginGroup);
        marginLayout->addWidget(new QLabel(viewerText("Units:"), marginGroup), 0, 0);
        m_unitsCombo->addItem(viewerText("Millimeters"));
        m_unitsCombo->addItem(viewerText("Points"));
        marginLayout->addWidget(m_unitsCombo, 0, 1);

        const std::array<QString, 4> labels = {
            viewerText("Top:"), viewerText("Bottom:"),
            viewerText("Left:"), viewerText("Right:"),
        };
        for (int index = 0; index < static_cast<int>(m_marginSpinBoxes.size()); ++index) {
            m_marginSpinBoxes[index] = new QDoubleSpinBox(marginGroup);
            m_marginSpinBoxes[index]->setKeyboardTracking(false);
            marginLayout->addWidget(new QLabel(labels[index], marginGroup), index + 1, 0);
            marginLayout->addWidget(m_marginSpinBoxes[index], index + 1, 1);
            connect(m_marginSpinBoxes[index], &QDoubleSpinBox::valueChanged,
                    this, [this]() { updateMarginsFromControls(); });
        }

        auto *zeroButton = new QPushButton(viewerText("Set to Zero"), marginGroup);
        auto *restoreButton = new QPushButton(viewerText("Restore Selection"), marginGroup);
        connect(zeroButton, &QPushButton::clicked,
                this, [this]() { setMargins(QMarginsF()); });
        connect(restoreButton, &QPushButton::clicked,
                this, [this]() { setMargins(m_selectionMargins); });
        marginLayout->addWidget(zeroButton, 5, 0);
        marginLayout->addWidget(restoreButton, 5, 1);
        marginLayout->setRowStretch(6, 1);
        topLayout->addWidget(marginGroup);

        auto *previewLayout = new QVBoxLayout;
        m_preview->setPage(pagePixmap, pageSizePoints);
        previewLayout->addWidget(m_preview, 1);
        m_sizeLabel->setAlignment(Qt::AlignCenter);
        previewLayout->addWidget(m_sizeLabel);
        topLayout->addLayout(previewLayout, 1);
        dialogLayout->addLayout(topLayout, 1);

        auto *rangeGroup = new QGroupBox(viewerText("Page Range"), this);
        auto *rangeLayout = new QGridLayout(rangeGroup);
        rangeLayout->addWidget(m_allPagesRadio, 0, 0, 1, 4);
        rangeLayout->addWidget(m_rangeRadio, 1, 0);
        m_fromPageSpinBox->setRange(1, pageCount);
        m_toPageSpinBox->setRange(1, pageCount);
        m_fromPageSpinBox->setValue(currentPageIndex + 1);
        m_toPageSpinBox->setValue(currentPageIndex + 1);
        rangeLayout->addWidget(m_fromPageSpinBox, 1, 1);
        rangeLayout->addWidget(new QLabel(viewerText("To:"), rangeGroup), 1, 2);
        rangeLayout->addWidget(m_toPageSpinBox, 1, 3);
        rangeLayout->addWidget(new QLabel(viewerText("Apply to:"), rangeGroup), 2, 0);
        m_subsetCombo->addItem(viewerText("All pages"));
        m_subsetCombo->addItem(viewerText("Even pages only"));
        m_subsetCombo->addItem(viewerText("Odd pages only"));
        rangeLayout->addWidget(m_subsetCombo, 2, 1, 1, 3);
        m_rangeRadio->setChecked(true);
        dialogLayout->addWidget(rangeGroup);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        m_applyButton = buttons->button(QDialogButtonBox::Ok);
        m_applyButton->setText(viewerText("Apply"));
        buttons->button(QDialogButtonBox::Cancel)->setText(viewerText("Cancel"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        dialogLayout->addWidget(buttons);

        connect(m_unitsCombo, &QComboBox::currentIndexChanged,
                this, [this](int unitIndex) {
                    m_unitIndex = unitIndex;
                    populateMarginControls();
                });
        connect(m_rangeRadio, &QRadioButton::toggled, this, [this](bool checked) {
            m_fromPageSpinBox->setEnabled(checked);
            m_toPageSpinBox->setEnabled(checked);
        });

        populateMarginControls();
    }

    QMarginsF marginsPoints() const { return m_marginsPoints; }

    QVector<int> affectedPageIndexes() const
    {
        const int firstPage = m_allPagesRadio->isChecked()
                                  ? 0
                                  : qMin(m_fromPageSpinBox->value(),
                                         m_toPageSpinBox->value()) - 1;
        const int lastPage = m_allPagesRadio->isChecked()
                                 ? m_fromPageSpinBox->maximum() - 1
                                 : qMax(m_fromPageSpinBox->value(),
                                        m_toPageSpinBox->value()) - 1;
        QVector<int> pages;
        pages.reserve(lastPage - firstPage + 1);
        for (int pageIndex = firstPage; pageIndex <= lastPage; ++pageIndex) {
            const int pageNumber = pageIndex + 1;
            const bool included = m_subsetCombo->currentIndex() == 0
                                  || (m_subsetCombo->currentIndex() == 1
                                      && pageNumber % 2 == 0)
                                  || (m_subsetCombo->currentIndex() == 2
                                      && pageNumber % 2 != 0);
            if (included)
                pages.append(pageIndex);
        }
        return pages;
    }

private:
    double displayFactor() const
    {
        return m_unitIndex == 0 ? 25.4 / 72.0 : 1.0;
    }

    void populateMarginControls()
    {
        const double factor = displayFactor();
        const QString suffix = m_unitIndex == 0 ? QStringLiteral(" mm")
                                                 : QStringLiteral(" pt");
        const std::array<double, 4> values = {
            m_marginsPoints.top(), m_marginsPoints.bottom(),
            m_marginsPoints.left(), m_marginsPoints.right(),
        };
        const std::array<double, 4> maximums = {
            m_pageSizePoints.height(), m_pageSizePoints.height(),
            m_pageSizePoints.width(), m_pageSizePoints.width(),
        };
        for (int index = 0; index < static_cast<int>(m_marginSpinBoxes.size()); ++index) {
            const QSignalBlocker blocker(m_marginSpinBoxes[index]);
            m_marginSpinBoxes[index]->setDecimals(m_unitIndex == 0 ? 3 : 2);
            m_marginSpinBoxes[index]->setRange(0.0, maximums[index] * factor);
            m_marginSpinBoxes[index]->setSuffix(suffix);
            m_marginSpinBoxes[index]->setValue(values[index] * factor);
        }
        updatePreview();
    }

    void updateMarginsFromControls()
    {
        const double inverseFactor = 1.0 / displayFactor();
        m_marginsPoints = QMarginsF(m_marginSpinBoxes[2]->value() * inverseFactor,
                                    m_marginSpinBoxes[0]->value() * inverseFactor,
                                    m_marginSpinBoxes[3]->value() * inverseFactor,
                                    m_marginSpinBoxes[1]->value() * inverseFactor);
        updatePreview();
    }

    void setMargins(const QMarginsF &margins)
    {
        m_marginsPoints = margins;
        populateMarginControls();
    }

    void updatePreview()
    {
        const double croppedWidth = m_pageSizePoints.width() - m_marginsPoints.left()
                                    - m_marginsPoints.right();
        const double croppedHeight = m_pageSizePoints.height() - m_marginsPoints.top()
                                     - m_marginsPoints.bottom();
        const bool valid = croppedWidth > 0.01 && croppedHeight > 0.01;
        m_applyButton->setEnabled(valid);
        m_preview->setMargins(m_marginsPoints);

        const double factor = displayFactor();
        const QString unit = m_unitIndex == 0 ? QStringLiteral("mm") : QStringLiteral("pt");
        m_sizeLabel->setText(viewerText("Cropped page size: %1 × %2 %3")
                                 .arg(QLocale().toString(qMax(0.0, croppedWidth * factor), 'f', 2),
                                      QLocale().toString(qMax(0.0, croppedHeight * factor), 'f', 2),
                                      unit));
    }

    QSizeF m_pageSizePoints;
    QMarginsF m_selectionMargins;
    QMarginsF m_marginsPoints;
    CropPreviewWidget *m_preview;
    QLabel *m_sizeLabel;
    QComboBox *m_unitsCombo;
    std::array<QDoubleSpinBox *, 4> m_marginSpinBoxes = {};
    QRadioButton *m_allPagesRadio;
    QRadioButton *m_rangeRadio;
    QSpinBox *m_fromPageSpinBox;
    QSpinBox *m_toPageSpinBox;
    QComboBox *m_subsetCombo;
    QPushButton *m_applyButton = nullptr;
    int m_unitIndex = 0;
};

class PdfPageLabel final : public QLabel
{
public:
    explicit PdfPageLabel(QWidget *parent)
        : QLabel(parent)
    {
    }

    void setPageSelected(bool selected)
    {
        if (m_pageSelected == selected)
            return;
        m_pageSelected = selected;
        update();
    }

    void setRegionSelection(const QRectF &normalizedRegion)
    {
        if (m_regionSelection == normalizedRegion)
            return;
        m_regionSelection = normalizedRegion;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QLabel::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor accent(QStringLiteral("#2684ff"));

        if (m_pageSelected) {
            painter.setPen(QPen(accent, 3));
            painter.setBrush(QColor(38, 132, 255, 24));
            painter.drawRect(rect().adjusted(2, 2, -2, -2));
        }

        if (!m_regionSelection.isEmpty()) {
            const QRectF selection(m_regionSelection.x() * width(),
                                   m_regionSelection.y() * height(),
                                   m_regionSelection.width() * width(),
                                   m_regionSelection.height() * height());
            QPainterPath outside;
            outside.addRect(QRectF(rect()));
            QPainterPath inside;
            inside.addRect(selection);
            painter.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 70));
            painter.setPen(QPen(accent, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(selection);

            painter.setPen(Qt::NoPen);
            painter.setBrush(accent);
            constexpr qreal handleSize = 7.0;
            const QList<QPointF> handles = {
                selection.topLeft(), selection.topRight(),
                selection.bottomLeft(), selection.bottomRight(),
            };
            for (const QPointF &point : handles) {
                painter.drawRect(QRectF(point.x() - handleSize / 2,
                                        point.y() - handleSize / 2,
                                        handleSize, handleSize));
            }
        }
    }

private:
    bool m_pageSelected = false;
    QRectF m_regionSelection;
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

    auto *selectionGroup = new QButtonGroup(toolbar);
    selectionGroup->setExclusive(true);

    m_selectPageButton = new QToolButton(toolbar);
    m_selectPageButton->setCheckable(true);
    m_selectPageButton->setChecked(true);
    m_selectPageButton->setIcon(selectionModeIcon(true, iconColor));
    m_selectPageButton->setText(tr("Select Page"));
    m_selectPageButton->setToolTip(tr("Select pages"));
    m_selectPageButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    selectionGroup->addButton(m_selectPageButton);
    toolbar->addWidget(m_selectPageButton);

    m_selectRegionButton = new QToolButton(toolbar);
    m_selectRegionButton->setCheckable(true);
    m_selectRegionButton->setIcon(selectionModeIcon(false, iconColor));
    m_selectRegionButton->setText(tr("Select Region"));
    m_selectRegionButton->setToolTip(tr("Select a rectangular region"));
    m_selectRegionButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    selectionGroup->addButton(m_selectRegionButton);
    toolbar->addWidget(m_selectRegionButton);

    m_pageSelectionCombo = new QComboBox(toolbar);
    m_pageSelectionCombo->setFixedWidth(110);
    m_pageSelectionCombo->setPlaceholderText(tr("Select"));
    m_pageSelectionCombo->addItem(tr("All"));
    m_pageSelectionCombo->addItem(tr("None"));
    m_pageSelectionCombo->addItem(tr("Even"));
    m_pageSelectionCombo->addItem(tr("Odd"));
    m_pageSelectionCombo->setCurrentIndex(-1);
    m_pageSelectionCombo->setEnabled(false);
    toolbar->addWidget(m_pageSelectionCombo);

    toolbar->addSeparator();

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

    connect(m_selectPageButton, &QToolButton::clicked, this,
            [this]() { setSelectionMode(SelectionMode::Page); });
    connect(m_selectRegionButton, &QToolButton::clicked, this,
            [this]() { setSelectionMode(SelectionMode::Region); });
    connect(m_pageSelectionCombo, &QComboBox::activated,
            this, &PdfViewerWidget::applyPageSelectionCommand);

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

    m_zoomMode = ZoomMode::FitPage;
    setSelectionMode(SelectionMode::Page);
    syncViewControl();
    syncFitControl();
    syncZoomControl();

    auto *editToolbar = new QToolBar(this);
    editToolbar->setObjectName(QStringLiteral("editToolBar"));
    editToolbar->setMovable(false);
    editToolbar->setFloatable(false);
    editToolbar->setIconSize(QSize(28, 24));
    layout()->addWidget(editToolbar);

    m_organizePagesButton = new QToolButton(editToolbar);
    m_organizePagesButton->setObjectName(QStringLiteral("organizePagesButton"));
    m_organizePagesButton->setCheckable(true);
    m_organizePagesButton->setIcon(
        style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    m_organizePagesButton->setText(tr("Organize Pages"));
    m_organizePagesButton->setToolTip(tr("Reorder, copy, and insert pages"));
    m_organizePagesButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    editToolbar->addWidget(m_organizePagesButton);

    editToolbar->addSeparator();

    m_cutPagesAction = new QAction(tr("Cut"), this);
    m_cutPagesAction->setShortcut(QKeySequence::Cut);
    m_cutPagesAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(m_cutPagesAction);
    auto *cutPagesButton = new QToolButton(editToolbar);
    cutPagesButton->setDefaultAction(m_cutPagesAction);
    cutPagesButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    editToolbar->addWidget(cutPagesButton);

    m_copyPagesAction = new QAction(tr("Copy"), this);
    m_copyPagesAction->setShortcut(QKeySequence::Copy);
    m_copyPagesAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(m_copyPagesAction);
    auto *copyPagesButton = new QToolButton(editToolbar);
    copyPagesButton->setDefaultAction(m_copyPagesAction);
    copyPagesButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    editToolbar->addWidget(copyPagesButton);

    m_extractPagesAction = new QAction(tr("Extract PDF…"), this);
    auto *extractPagesButton = new QToolButton(editToolbar);
    extractPagesButton->setDefaultAction(m_extractPagesAction);
    extractPagesButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    editToolbar->addWidget(extractPagesButton);

    editToolbar->addSeparator();

    m_cropButton = new QToolButton(editToolbar);
    m_cropButton->setObjectName(QStringLiteral("cropButton"));
    m_cropButton->setIcon(cropToolIcon(iconColor));
    m_cropButton->setToolTip(tr("Crop"));
    editToolbar->addWidget(m_cropButton);

    editToolbar->addSeparator();

    m_rotateCounterclockwiseButton = new QToolButton(editToolbar);
    m_rotateCounterclockwiseButton->setObjectName(
        QStringLiteral("rotateCounterclockwiseButton"));
    m_rotateCounterclockwiseButton->setIcon(rotationToolIcon(false, iconColor));
    m_rotateCounterclockwiseButton->setToolTip(tr("Rotate Counterclockwise"));
    editToolbar->addWidget(m_rotateCounterclockwiseButton);

    m_rotateClockwiseButton = new QToolButton(editToolbar);
    m_rotateClockwiseButton->setObjectName(QStringLiteral("rotateClockwiseButton"));
    m_rotateClockwiseButton->setIcon(rotationToolIcon(true, iconColor));
    m_rotateClockwiseButton->setToolTip(tr("Rotate Clockwise"));
    editToolbar->addWidget(m_rotateClockwiseButton);

    connect(m_cropButton, &QToolButton::clicked,
            this, &PdfViewerWidget::cropSelectedRegion);
    connect(m_organizePagesButton, &QToolButton::toggled,
            this, &PdfViewerWidget::setOrganizePagesEnabled);
    connect(m_cutPagesAction, &QAction::triggered, this,
            [this]() { copySelectedPages(/*cut=*/true); });
    connect(m_copyPagesAction, &QAction::triggered, this,
            [this]() { copySelectedPages(/*cut=*/false); });
    connect(m_extractPagesAction, &QAction::triggered,
            this, &PdfViewerWidget::extractSelectedPages);
    connect(m_rotateCounterclockwiseButton, &QToolButton::clicked, this,
            [this]() { rotateSelectedPages(false); });
    connect(m_rotateClockwiseButton, &QToolButton::clicked, this,
            [this]() { rotateSelectedPages(true); });
    syncEditControls();
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
    m_pageIds.clear();
    m_pageIds.reserve(pageSizes.size());
    for (int pageIndex = 0; pageIndex < pageSizes.size(); ++pageIndex)
        m_pageIds.append(m_nextPageId++);
    buildPageLabels(pageSizes.size());
    buildInsertionPlaceholders();
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
        m_pageSelectionCombo->setEnabled(true);
    }
    m_selectedPages.insert(0);
    m_pageSelectionAnchor = 0;
    setSelectionMode(m_selectionMode);
    syncNavigationControls();

    QTimer::singleShot(0, this, &PdfViewerWidget::renderVisiblePages);
    emit documentLoaded(true, m_pageLabels.size());
}

void PdfViewerWidget::buildPageLabels(int pageCount)
{
    m_pageLabels.reserve(pageCount);
    m_renderedWidths.fill(0, pageCount);

    for (int i = 0; i < pageCount; ++i) {
        auto *label = new PdfPageLabel(m_pagesContainer);
        label->setProperty("pdfPageIndex", i);
        label->setFrameShape(QFrame::NoFrame);
        label->setStyleSheet(QStringLiteral("background-color: palette(base); border: none;"));
        label->setAlignment(Qt::AlignCenter);
        label->installEventFilter(this);
        label->hide();
        m_pageLabels.append(label);
    }
}

void PdfViewerWidget::buildInsertionPlaceholders()
{
    m_pagePlaceholders.reserve(m_pageLabels.size() + 1);
    for (int insertionIndex = 0; insertionIndex <= m_pageLabels.size(); ++insertionIndex) {
        auto *placeholder = new PdfInsertionPlaceholder(
            insertionIndex,
            QByteArray::number(reinterpret_cast<quintptr>(this), 16),
            [this](int index, const QPoint &position) {
                showInsertionContextMenu(index, position);
            },
            [this](int index) { moveSelectedPagesTo(index); },
            m_pagesContainer);
        placeholder->setVisible(m_organizePagesEnabled);
        m_pagePlaceholders.append(placeholder);
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
    for (PdfInsertionPlaceholder *placeholder : m_pagePlaceholders)
        placeholder->hide();

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

    if (!m_organizePagesEnabled)
        return;

    for (int insertionIndex = 0; insertionIndex < m_pagePlaceholders.size();
         ++insertionIndex) {
        PdfInsertionPlaceholder *placeholder = m_pagePlaceholders[insertionIndex];
        QLabel *previous = insertionIndex > 0 ? m_pageLabels[insertionIndex - 1] : nullptr;
        QLabel *next = insertionIndex < m_pageLabels.size()
                           ? m_pageLabels[insertionIndex]
                           : nullptr;
        const bool previousVisible = previous && !previous->isHidden();
        const bool nextVisible = next && !next->isHidden();
        if (!previousVisible && !nextVisible)
            continue;

        QRect geometry;
        if (previousVisible && nextVisible
            && qAbs(previous->geometry().center().y() - next->geometry().center().y())
                   < qMin(previous->height(), next->height()) / 2) {
            const int gapLeft = previous->geometry().right() + 1;
            const int gapRight = next->geometry().left() - 1;
            geometry = QRect(gapLeft, qMin(previous->y(), next->y()),
                             qMax(10, gapRight - gapLeft + 1),
                             qMax(previous->height(), next->height()));
        } else if (nextVisible) {
            const int gapTop = previousVisible ? previous->geometry().bottom() + 1
                                               : qMax(0, next->y() - kPageSpacing);
            const int gapBottom = next->y() - 1;
            geometry = QRect(next->x(), gapTop, next->width(),
                             qMax(10, gapBottom - gapTop + 1));
        } else {
            geometry = QRect(previous->x(), previous->geometry().bottom() + 1,
                             previous->width(), kPageSpacing);
        }
        placeholder->setGeometry(geometry);
        placeholder->show();
        placeholder->raise();
    }
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

void PdfViewerWidget::setSelectionMode(SelectionMode mode)
{
    if (mode == SelectionMode::Region && m_organizePagesEnabled) {
        m_organizePagesEnabled = false;
        if (m_organizePagesButton) {
            const QSignalBlocker blocker(m_organizePagesButton);
            m_organizePagesButton->setChecked(false);
        }
        rebuildPageLayout();
    }
    const bool modeChanged = m_selectionMode != mode;
    m_selectionMode = mode;

    if (m_selectPageButton)
        m_selectPageButton->setChecked(mode == SelectionMode::Page);
    if (m_selectRegionButton)
        m_selectRegionButton->setChecked(mode == SelectionMode::Region);
    if (m_pageSelectionCombo)
        m_pageSelectionCombo->setVisible(mode == SelectionMode::Page);

    if (modeChanged && mode == SelectionMode::Page && m_selectedPages.isEmpty()
        && m_currentPageIndex >= 0) {
        m_selectedPages.insert(m_currentPageIndex);
        m_pageSelectionAnchor = m_currentPageIndex;
    }

    for (QLabel *label : m_pageLabels) {
        label->setCursor(mode == SelectionMode::Region
                             ? Qt::CrossCursor
                             : (m_organizePagesEnabled ? Qt::OpenHandCursor
                                                      : Qt::PointingHandCursor));
    }
    updateSelectionOverlays();
}

void PdfViewerWidget::setOrganizePagesEnabled(bool enabled)
{
    if (m_organizePagesEnabled == enabled)
        return;
    m_organizePagesEnabled = enabled;
    if (enabled) {
        setSelectionMode(SelectionMode::Page);
        if (m_pageLayout != PageLayout::Continuous)
            setPageLayout(PageLayout::Continuous);
    }
    for (QLabel *label : m_pageLabels) {
        label->setCursor(enabled ? Qt::OpenHandCursor
                                 : (m_selectionMode == SelectionMode::Region
                                        ? Qt::CrossCursor
                                        : Qt::PointingHandCursor));
    }
    rebuildPageLayout();
    syncEditControls();
}

void PdfViewerWidget::showPageContextMenu(int pageIndex, const QPoint &globalPosition)
{
    if (pageIndex < 0 || pageIndex >= m_pageLabels.size())
        return;
    if (!m_selectedPages.contains(pageIndex)) {
        m_selectedPages = {pageIndex};
        m_pageSelectionAnchor = pageIndex;
        setCurrentPageFromPointer(pageIndex);
        updateSelectionOverlays();
    }

    QMenu menu(this);
    QAction *cutAction = menu.addAction(tr("Cut"));
    cutAction->setShortcut(QKeySequence::Cut);
    QAction *copyAction = menu.addAction(tr("Copy"));
    copyAction->setShortcut(QKeySequence::Copy);
    menu.addSeparator();
    QAction *extractAction = menu.addAction(tr("Extract PDF…"));
    const bool canCopy = m_valid && !m_transformInProgress && !m_saveInProgress
                         && !m_selectedPages.isEmpty();
    copyAction->setEnabled(canCopy);
    cutAction->setEnabled(canCopy && m_selectedPages.size() < m_pageLabels.size());
    extractAction->setEnabled(canCopy);

    QAction *selectedAction = menu.exec(globalPosition);
    if (selectedAction == cutAction)
        copySelectedPages(/*cut=*/true);
    else if (selectedAction == copyAction)
        copySelectedPages(/*cut=*/false);
    else if (selectedAction == extractAction)
        extractSelectedPages();
}

void PdfViewerWidget::showInsertionContextMenu(int insertionIndex,
                                               const QPoint &globalPosition)
{
    if (insertionIndex < 0 || insertionIndex > m_pageLabels.size())
        return;

    QMenu menu(this);
    QAction *blankAction = menu.addAction(tr("Add Blank Page"));
    QAction *pasteAction = menu.addAction(tr("Paste"));
    QAction *insertPdfAction = menu.addAction(tr("Insert from PDF…"));
    const bool canEdit = m_valid && !m_transformInProgress && !m_saveInProgress;
    blankAction->setEnabled(canEdit);
    pasteAction->setEnabled(canEdit && g_pageClipboard.pageCount > 0
                            && !g_pageClipboard.pageArchive.isEmpty());
    insertPdfAction->setEnabled(canEdit);

    QAction *selectedAction = menu.exec(globalPosition);
    if (selectedAction == blankAction)
        insertBlankPageAt(insertionIndex);
    else if (selectedAction == pasteAction)
        pastePagesAt(insertionIndex);
    else if (selectedAction == insertPdfAction)
        insertPdfAt(insertionIndex);
}

void PdfViewerWidget::startSelectedPageDrag(QLabel *sourceLabel)
{
    if (!sourceLabel || !m_organizePagesEnabled || m_selectedPages.isEmpty()
        || m_transformInProgress || m_saveInProgress) {
        return;
    }

    auto *drag = new QDrag(sourceLabel);
    auto *mimeData = new QMimeData;
    mimeData->setData(kPageDragMimeType,
                      QByteArray::number(reinterpret_cast<quintptr>(this), 16));
    drag->setMimeData(mimeData);
    QPixmap preview = sourceLabel->pixmap();
    if (!preview.isNull()) {
        preview = preview.scaled(160, 200, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        drag->setPixmap(preview);
        drag->setHotSpot(preview.rect().center());
    }

    setPlaceholderDragActive(true);
    sourceLabel->setCursor(Qt::ClosedHandCursor);
    drag->exec(Qt::MoveAction);
    sourceLabel->setCursor(Qt::OpenHandCursor);
    setPlaceholderDragActive(false);
}

void PdfViewerWidget::setPlaceholderDragActive(bool active)
{
    for (PdfInsertionPlaceholder *placeholder : m_pagePlaceholders)
        placeholder->setDragActive(active);
}

void PdfViewerWidget::moveSelectedPagesTo(int insertionIndex)
{
    if (!m_valid || m_transformInProgress || m_saveInProgress
        || insertionIndex < 0 || insertionIndex > m_pageIds.size()
        || m_selectedPages.isEmpty()) {
        return;
    }

    QVector<int> selectedIndexes(m_selectedPages.cbegin(), m_selectedPages.cend());
    std::sort(selectedIndexes.begin(), selectedIndexes.end());
    QVector<quint64> movingPageIds;
    movingPageIds.reserve(selectedIndexes.size());
    for (const int pageIndex : selectedIndexes)
        movingPageIds.append(m_pageIds.at(pageIndex));

    QVector<quint64> targetPageIds = m_pageIds;
    for (int index = selectedIndexes.size() - 1; index >= 0; --index)
        targetPageIds.removeAt(selectedIndexes.at(index));
    int adjustedInsertionIndex = insertionIndex;
    for (const int selectedIndex : selectedIndexes) {
        if (selectedIndex < insertionIndex)
            --adjustedInsertionIndex;
    }
    adjustedInsertionIndex = qBound(0, adjustedInsertionIndex, targetPageIds.size());
    for (int offset = 0; offset < movingPageIds.size(); ++offset)
        targetPageIds.insert(adjustedInsertionIndex + offset, movingPageIds.at(offset));
    if (targetPageIds == m_pageIds)
        return;

    applyPageStructureChange(tr("Move pages"), m_pageIds, targetPageIds,
                             {}, {}, movingPageIds);
}

void PdfViewerWidget::copySelectedPages(bool cut)
{
    if (!m_valid || m_transformInProgress || m_saveInProgress
        || m_selectedPages.isEmpty()
        || (cut && m_selectedPages.size() >= m_pageLabels.size())) {
        return;
    }

    QVector<int> selectedIndexes(m_selectedPages.cbegin(), m_selectedPages.cend());
    std::sort(selectedIndexes.begin(), selectedIndexes.end());
    QVector<quint64> selectedPageIds;
    selectedPageIds.reserve(selectedIndexes.size());
    for (const int pageIndex : selectedIndexes)
        selectedPageIds.append(m_pageIds.at(pageIndex));

    m_transformInProgress = true;
    emit operationInProgressChanged(true);
    syncEditControls();
    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    const QVector<quint64> beforePageIds = m_pageIds;
    QThread *thread = QThread::create(
        [weakSelf, document, selectedIndexes, selectedPageIds, beforePageIds, cut]() {
            const QByteArray pageArchive = document->exportPages(selectedIndexes);
            QMetaObject::invokeMethod(
                qApp,
                [weakSelf, pageArchive, selectedPageIds, beforePageIds, cut]() {
                    if (!weakSelf)
                        return;
                    weakSelf->m_transformInProgress = false;
                    emit weakSelf->operationInProgressChanged(false);
                    weakSelf->syncEditControls();
                    if (pageArchive.isEmpty()) {
                        QMessageBox::warning(
                            weakSelf, viewerText("Copy Pages"),
                            viewerText("The selected pages could not be copied."));
                        return;
                    }
                    g_pageClipboard = {
                        pageArchive, static_cast<int>(selectedPageIds.size())};
                    if (!cut) {
                        weakSelf->finishPageCopy(pageArchive, selectedPageIds.size());
                        return;
                    }

                    QVector<quint64> targetPageIds = beforePageIds;
                    for (const quint64 pageId : selectedPageIds)
                        targetPageIds.removeOne(pageId);
                    QVector<quint64> nextSelection;
                    if (!targetPageIds.isEmpty()) {
                        const int selectionIndex = qMin(
                            beforePageIds.indexOf(selectedPageIds.first()),
                            targetPageIds.size() - 1);
                        nextSelection.append(targetPageIds.at(selectionIndex));
                    }
                    weakSelf->applyPageStructureChange(
                        viewerText("Cut pages"), beforePageIds, targetPageIds,
                        selectedPageIds, pageArchive, nextSelection);
                },
                Qt::QueuedConnection);
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::finishPageCopy(const QByteArray &pageArchive, int pageCount)
{
    if (!pageArchive.isEmpty() && pageCount > 0)
        g_pageClipboard = {pageArchive, pageCount};
}

void PdfViewerWidget::extractSelectedPages()
{
    if (!m_valid || m_transformInProgress || m_saveInProgress
        || m_selectionMode != SelectionMode::Page || m_selectedPages.isEmpty()) {
        return;
    }

    QVector<int> selectedIndexes(m_selectedPages.cbegin(), m_selectedPages.cend());
    std::sort(selectedIndexes.begin(), selectedIndexes.end());

    const QFileInfo sourceInfo(m_filePath);
    const QString suggestedPath = QDir(sourceInfo.absolutePath()).filePath(
        sourceInfo.completeBaseName() + QStringLiteral("_pages.pdf"));
    QString targetPath = QFileDialog::getSaveFileName(
        this, tr("Extract Pages"), suggestedPath, tr("PDF files (*.pdf)"));
    if (targetPath.isEmpty())
        return;
    if (QFileInfo(targetPath).suffix().isEmpty())
        targetPath += QStringLiteral(".pdf");

    const QFileInfo targetInfo(targetPath);
    const QString sourceIdentity = sourceInfo.canonicalFilePath().isEmpty()
                                       ? sourceInfo.absoluteFilePath()
                                       : sourceInfo.canonicalFilePath();
    const QString targetIdentity = targetInfo.canonicalFilePath().isEmpty()
                                       ? targetInfo.absoluteFilePath()
                                       : targetInfo.canonicalFilePath();
    if (sourceIdentity == targetIdentity) {
        QMessageBox::warning(
            this, tr("Extract Pages"),
            tr("Choose a different file so the open PDF is not overwritten."));
        return;
    }

    m_transformInProgress = true;
    emit operationInProgressChanged(true);
    syncEditControls();
    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    QThread *thread = QThread::create(
        [weakSelf, document, selectedIndexes, targetPath]() {
            const QByteArray pageArchive = document->exportPages(selectedIndexes);
            bool success = false;
            QString errorMessage;
            if (!pageArchive.isEmpty()) {
                QSaveFile output(targetPath);
                if (!output.open(QIODevice::WriteOnly)) {
                    errorMessage = output.errorString();
                } else if (output.write(pageArchive) != pageArchive.size()) {
                    errorMessage = output.errorString();
                    output.cancelWriting();
                } else if (!output.commit()) {
                    errorMessage = output.errorString();
                } else {
                    success = true;
                }
            }

            QMetaObject::invokeMethod(
                qApp,
                [weakSelf, success, errorMessage]() {
                    if (!weakSelf)
                        return;
                    weakSelf->m_transformInProgress = false;
                    emit weakSelf->operationInProgressChanged(false);
                    weakSelf->syncEditControls();
                    if (!success) {
                        QString message = viewerText(
                            "The selected pages could not be extracted.");
                        if (!errorMessage.isEmpty())
                            message += QStringLiteral("\n\n") + errorMessage;
                        QMessageBox::warning(
                            weakSelf, viewerText("Extract Pages"), message);
                    }
                },
                Qt::QueuedConnection);
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::insertBlankPageAt(int insertionIndex)
{
    const int referenceIndex = insertionIndex > 0 ? insertionIndex - 1 : 0;
    const QByteArray pageArchive = PdfDocument::createBlankPageArchive(
        pageSize(referenceIndex));
    if (pageArchive.isEmpty()) {
        QMessageBox::warning(this, tr("Add Blank Page"),
                             tr("The blank page could not be created."));
        return;
    }
    insertArchiveAt(insertionIndex, pageArchive, 1, tr("Add blank page"));
}

void PdfViewerWidget::pastePagesAt(int insertionIndex)
{
    if (g_pageClipboard.pageArchive.isEmpty() || g_pageClipboard.pageCount <= 0)
        return;
    insertArchiveAt(insertionIndex, g_pageClipboard.pageArchive,
                    g_pageClipboard.pageCount, tr("Paste pages"));
}

void PdfViewerWidget::insertPdfAt(int insertionIndex)
{
    const QString sourcePath = QFileDialog::getOpenFileName(
        this, tr("Insert from PDF"), QString(), tr("PDF files (*.pdf)"));
    if (sourcePath.isEmpty())
        return;

    m_transformInProgress = true;
    emit operationInProgressChanged(true);
    syncEditControls();
    const QPointer<PdfViewerWidget> weakSelf(this);
    QThread *thread = QThread::create([weakSelf, sourcePath, insertionIndex]() {
        PdfDocument sourceDocument(sourcePath);
        QVector<int> pageIndexes;
        if (sourceDocument.isValid()) {
            pageIndexes.reserve(sourceDocument.pageCount());
            for (int pageIndex = 0; pageIndex < sourceDocument.pageCount(); ++pageIndex)
                pageIndexes.append(pageIndex);
        }
        const QByteArray pageArchive = sourceDocument.exportPages(pageIndexes);
        QMetaObject::invokeMethod(
            qApp,
            [weakSelf, insertionIndex, pageArchive, pageCount = pageIndexes.size()]() {
                if (!weakSelf)
                    return;
                weakSelf->m_transformInProgress = false;
                emit weakSelf->operationInProgressChanged(false);
                weakSelf->syncEditControls();
                if (pageArchive.isEmpty() || pageCount <= 0) {
                    QMessageBox::warning(
                        weakSelf, viewerText("Insert from PDF"),
                        viewerText("The selected PDF could not be inserted."));
                    return;
                }
                weakSelf->insertArchiveAt(
                    insertionIndex, pageArchive, pageCount,
                    viewerText("Insert pages from PDF"));
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::insertArchiveAt(int insertionIndex,
                                      const QByteArray &pageArchive,
                                      int pageCount, const QString &description)
{
    if (!m_valid || m_transformInProgress || m_saveInProgress
        || insertionIndex < 0 || insertionIndex > m_pageIds.size()
        || pageArchive.isEmpty() || pageCount <= 0) {
        return;
    }

    QVector<quint64> insertedPageIds;
    insertedPageIds.reserve(pageCount);
    for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex)
        insertedPageIds.append(m_nextPageId++);
    QVector<quint64> targetPageIds = m_pageIds;
    for (int offset = 0; offset < insertedPageIds.size(); ++offset)
        targetPageIds.insert(insertionIndex + offset, insertedPageIds.at(offset));
    applyPageStructureChange(description, m_pageIds, targetPageIds,
                             insertedPageIds, pageArchive, insertedPageIds);
}

void PdfViewerWidget::applyPageStructureChange(
    const QString &description, const QVector<quint64> &beforePageIds,
    const QVector<quint64> &afterPageIds,
    const QVector<quint64> &archivedPageIds, const QByteArray &pageArchive,
    const QVector<quint64> &selectedPageIds)
{
    if (!m_valid || m_transformInProgress || m_saveInProgress
        || beforePageIds != m_pageIds || afterPageIds.isEmpty()) {
        return;
    }

    EditHistoryEntry historyEntry;
    historyEntry.description = description;
    historyEntry.beforePageIds = beforePageIds;
    historyEntry.afterPageIds = afterPageIds;
    historyEntry.archivedPageIds = archivedPageIds;
    historyEntry.pageArchive = pageArchive;

    m_transformInProgress = true;
    emit operationInProgressChanged(true);
    ++m_documentRevision;
    m_pagesLoading.clear();
    syncEditControls();

    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    QThread *thread = QThread::create(
        [weakSelf, document, historyEntry, selectedPageIds]() {
            const bool restored = document->restorePageStructure(
                historyEntry.beforePageIds, historyEntry.afterPageIds,
                historyEntry.archivedPageIds, historyEntry.pageArchive);
            const QVector<QSizeF> pageSizes = restored ? document->allPageSizes()
                                                       : QVector<QSizeF>();
            QMetaObject::invokeMethod(
                qApp,
                [weakSelf, restored, pageSizes, historyEntry, selectedPageIds]() {
                    if (weakSelf) {
                        weakSelf->finishPageStructureChange(
                            restored, pageSizes, historyEntry, selectedPageIds);
                    }
                },
                Qt::QueuedConnection);
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::finishPageStructureChange(
    bool success, const QVector<QSizeF> &pageSizes,
    const EditHistoryEntry &historyEntry,
    const QVector<quint64> &selectedPageIds)
{
    m_transformInProgress = false;
    emit operationInProgressChanged(false);
    success = success && pageSizes.size() == historyEntry.afterPageIds.size();
    if (success) {
        rebuildPagesAfterStructure(pageSizes, historyEntry.afterPageIds,
                                   selectedPageIds);
        recordHistoryEntry(historyEntry);
    } else {
        qWarning() << "Failed to change PDF page structure in memory:" << m_filePath;
        syncEditControls();
        QTimer::singleShot(0, this, &PdfViewerWidget::renderVisiblePages);
    }
}

void PdfViewerWidget::rebuildPagesAfterStructure(
    const QVector<QSizeF> &pageSizes, const QVector<quint64> &pageIds,
    const QVector<quint64> &selectedPageIds)
{
    qDeleteAll(m_pagePlaceholders);
    m_pagePlaceholders.clear();
    qDeleteAll(m_pageLabels);
    m_pageLabels.clear();
    m_renderedWidths.clear();
    m_pagesLoading.clear();
    m_pageSizes = pageSizes;
    m_pageIds = pageIds;
    buildPageLabels(pageSizes.size());
    buildInsertionPlaceholders();

    m_selectedPages.clear();
    for (const quint64 pageId : selectedPageIds) {
        const int pageIndex = m_pageIds.indexOf(pageId);
        if (pageIndex >= 0)
            m_selectedPages.insert(pageIndex);
    }
    if (m_selectedPages.isEmpty() && !m_pageIds.isEmpty())
        m_selectedPages.insert(qBound(0, m_currentPageIndex, m_pageIds.size() - 1));
    QVector<int> sortedSelection(m_selectedPages.cbegin(), m_selectedPages.cend());
    std::sort(sortedSelection.begin(), sortedSelection.end());
    m_currentPageIndex = sortedSelection.isEmpty() ? 0 : sortedSelection.first();
    m_pageSelectionAnchor = m_currentPageIndex;
    m_regionPageIndex = -1;
    m_regionSelection = {};

    if (m_pageSpinBox) {
        m_pageSpinBox->setRange(1, m_pageIds.size());
        m_pageCountLabel->setText(QStringLiteral("/ %1").arg(m_pageIds.size()));
    }
    setSelectionMode(SelectionMode::Page);
    applyZoom();
    syncNavigationControls();
    QTimer::singleShot(0, this, &PdfViewerWidget::renderVisiblePages);
}

void PdfViewerWidget::applyPageSelectionCommand(int commandIndex)
{
    m_selectedPages.clear();
    for (int pageIndex = 0; pageIndex < m_pageLabels.size(); ++pageIndex) {
        const int pageNumber = pageIndex + 1;
        const bool selected = commandIndex == 0
                              || (commandIndex == 2 && pageNumber % 2 == 0)
                              || (commandIndex == 3 && pageNumber % 2 != 0);
        if (selected)
            m_selectedPages.insert(pageIndex);
    }

    m_pageSelectionAnchor = m_selectedPages.contains(m_currentPageIndex)
                                ? m_currentPageIndex
                                : -1;
    updateSelectionOverlays();

    const QSignalBlocker blocker(m_pageSelectionCombo);
    m_pageSelectionCombo->setCurrentIndex(-1);
}

void PdfViewerWidget::updateSelectionOverlays()
{
    for (int pageIndex = 0; pageIndex < m_pageLabels.size(); ++pageIndex) {
        auto *label = static_cast<PdfPageLabel *>(m_pageLabels[pageIndex]);
        label->setPageSelected(m_selectionMode == SelectionMode::Page
                               && m_selectedPages.contains(pageIndex));
        label->setRegionSelection(m_selectionMode == SelectionMode::Region
                                          && pageIndex == m_regionPageIndex
                                      ? m_regionSelection
                                      : QRectF());
    }
    syncEditControls();
}

void PdfViewerWidget::syncEditControls()
{
    const bool canEditPages = m_valid && !m_transformInProgress && !m_saveInProgress
                              && m_selectionMode == SelectionMode::Page
                              && !m_selectedPages.isEmpty();
    const bool canRotate = m_valid && !m_transformInProgress
                           && m_selectionMode == SelectionMode::Page
                           && !m_selectedPages.isEmpty();
    const bool canCrop = m_valid && !m_transformInProgress
                         && m_selectionMode == SelectionMode::Region
                         && m_regionPageIndex >= 0 && !m_regionSelection.isEmpty();
    if (m_rotateCounterclockwiseButton)
        m_rotateCounterclockwiseButton->setEnabled(canRotate);
    if (m_rotateClockwiseButton)
        m_rotateClockwiseButton->setEnabled(canRotate);
    if (m_cropButton)
        m_cropButton->setEnabled(canCrop);
    if (m_organizePagesButton)
        m_organizePagesButton->setEnabled(m_valid && !m_transformInProgress);
    if (m_cutPagesAction)
        m_cutPagesAction->setEnabled(canEditPages
                                     && m_selectedPages.size() < m_pageLabels.size());
    if (m_copyPagesAction)
        m_copyPagesAction->setEnabled(canEditPages);
    if (m_extractPagesAction)
        m_extractPagesAction->setEnabled(canEditPages);
}

void PdfViewerWidget::rotateSelectedPages(bool clockwise)
{
    if (!m_valid || m_transformInProgress || m_selectedPages.isEmpty())
        return;

    QVector<int> affectedPages;
    affectedPages.reserve(m_selectedPages.size());
    for (const int pageIndex : m_selectedPages)
        affectedPages.append(pageIndex);
    std::sort(affectedPages.begin(), affectedPages.end());

    m_transformInProgress = true;
    if (!m_modified) {
        m_modified = true;
        emit modifiedChanged(true);
    }
    emit operationInProgressChanged(true);
    ++m_documentRevision;
    m_pagesLoading.clear();
    syncEditControls();

    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    const QString historyDescription = clockwise
                                           ? tr("Rotate pages clockwise")
                                           : tr("Rotate pages counterclockwise");
    QThread *thread = QThread::create(
        [weakSelf, document, affectedPages, clockwise, historyDescription]() {
            const QVector<PdfPageState> beforeStates = document->pageStates(affectedPages);
            const bool transformed = beforeStates.size() == affectedPages.size()
                                     && document->rotatePages(affectedPages, clockwise);
            const QVector<PdfPageState> afterStates = transformed
                                                          ? document->pageStates(affectedPages)
                                                          : QVector<PdfPageState>();
            const bool success = transformed && afterStates.size() == affectedPages.size();
            if (!success && beforeStates.size() == affectedPages.size())
                document->restorePageStates(beforeStates);
            const QVector<QSizeF> pageSizes = success ? document->allPageSizes()
                                                      : QVector<QSizeF>();
            QMetaObject::invokeMethod(
                qApp,
                [weakSelf, success, pageSizes, affectedPages, historyDescription,
                 beforeStates, afterStates]() {
                    if (weakSelf) {
                        weakSelf->finishDocumentTransform(
                            success, pageSizes, affectedPages, /*clearRegion=*/false,
                            historyDescription, beforeStates, afterStates);
                    }
                },
                Qt::QueuedConnection);
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::cropSelectedRegion()
{
    if (!m_valid || m_transformInProgress || m_regionPageIndex < 0
        || m_regionSelection.isEmpty()) {
        return;
    }

    const int pageIndex = m_regionPageIndex;
    const QRectF region = m_regionSelection;
    const QSizeF sourceSize = pageSize(pageIndex);
    const QMarginsF selectionMargins(region.left() * sourceSize.width(),
                                     region.top() * sourceSize.height(),
                                     (1.0 - region.right()) * sourceSize.width(),
                                     (1.0 - region.bottom()) * sourceSize.height());
    CropPagesDialog dialog(m_pageLabels[pageIndex]->pixmap(), sourceSize,
                           selectionMargins, pageIndex, m_pageLabels.size(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QVector<int> affectedPages = dialog.affectedPageIndexes();
    const QMarginsF marginsPoints = dialog.marginsPoints();
    if (affectedPages.isEmpty())
        return;

    m_transformInProgress = true;
    if (!m_modified) {
        m_modified = true;
        emit modifiedChanged(true);
    }
    emit operationInProgressChanged(true);
    ++m_documentRevision;
    m_pagesLoading.clear();
    syncEditControls();

    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    const QString historyDescription = tr("Crop pages");
    QThread *thread = QThread::create(
        [weakSelf, document, marginsPoints, affectedPages, historyDescription]() {
        const QVector<PdfPageState> beforeStates = document->pageStates(affectedPages);
        const bool transformed = beforeStates.size() == affectedPages.size()
                                 && document->cropPages(affectedPages, marginsPoints);
        const QVector<PdfPageState> afterStates = transformed
                                                      ? document->pageStates(affectedPages)
                                                      : QVector<PdfPageState>();
        const bool success = transformed && afterStates.size() == affectedPages.size();
        if (!success && beforeStates.size() == affectedPages.size())
            document->restorePageStates(beforeStates);
        const QVector<QSizeF> pageSizes = success ? document->allPageSizes()
                                                  : QVector<QSizeF>();
        QMetaObject::invokeMethod(
            qApp,
            [weakSelf, success, pageSizes, affectedPages, historyDescription,
             beforeStates, afterStates]() {
                if (weakSelf) {
                    weakSelf->finishDocumentTransform(
                        success, pageSizes, affectedPages, /*clearRegion=*/true,
                        historyDescription, beforeStates, afterStates);
                }
            },
            Qt::QueuedConnection);
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::finishDocumentTransform(bool success,
                                              const QVector<QSizeF> &pageSizes,
                                              const QVector<int> &affectedPages,
                                              bool clearRegion,
                                              const QString &historyDescription,
                                              const QVector<PdfPageState> &beforeStates,
                                              const QVector<PdfPageState> &afterStates)
{
    m_transformInProgress = false;
    emit operationInProgressChanged(false);
    success = success && pageSizes.size() == m_pageLabels.size();

    if (success) {
        m_pageSizes = pageSizes;
        for (const int pageIndex : affectedPages) {
            if (pageIndex < 0 || pageIndex >= m_pageLabels.size())
                continue;
            m_pageLabels[pageIndex]->clear();
            m_renderedWidths[pageIndex] = 0;
        }
        if (clearRegion) {
            m_regionPageIndex = -1;
            m_regionSelection = {};
        }
        recordHistoryEntry({historyDescription, beforeStates, afterStates});
        updateSelectionOverlays();
        applyZoom();
    } else {
        updateModifiedState();
        qWarning() << "Failed to transform PDF pages in memory:" << m_filePath;
        syncEditControls();
        QTimer::singleShot(0, this, &PdfViewerWidget::renderVisiblePages);
    }
}

void PdfViewerWidget::recordHistoryEntry(EditHistoryEntry entry)
{
    if (m_historyPosition < m_editHistory.size()) {
        if (m_savedHistoryPosition > m_historyPosition)
            m_savedHistoryPosition = -1;
        m_editHistory.resize(m_historyPosition);
    }
    m_editHistory.append(std::move(entry));
    m_historyPosition = m_editHistory.size();

    if (m_editHistory.size() > kHistoryLimit) {
        const int removedCount = m_editHistory.size() - kHistoryLimit;
        m_editHistory.remove(0, removedCount);
        m_historyPosition -= removedCount;
        if (m_savedHistoryPosition >= 0) {
            m_savedHistoryPosition -= removedCount;
            if (m_savedHistoryPosition < 0)
                m_savedHistoryPosition = -1;
        }
    }
    updateModifiedState();
    emit historyChanged();
}

void PdfViewerWidget::undo()
{
    if (canUndo())
        navigateHistory(/*redoOperation=*/false);
}

void PdfViewerWidget::redo()
{
    if (canRedo())
        navigateHistory(/*redoOperation=*/true);
}

void PdfViewerWidget::navigateHistory(bool redoOperation)
{
    const int entryIndex = redoOperation ? m_historyPosition : m_historyPosition - 1;
    const int targetHistoryPosition = redoOperation ? m_historyPosition + 1
                                                    : m_historyPosition - 1;
    const EditHistoryEntry &entry = m_editHistory.at(entryIndex);
    if (entry.changesPageStructure()) {
        const QVector<quint64> targetPageIds = redoOperation ? entry.afterPageIds
                                                             : entry.beforePageIds;
        const QVector<quint64> currentPageIds = m_pageIds;
        const QVector<quint64> archivedPageIds = entry.archivedPageIds;
        const QByteArray pageArchive = entry.pageArchive;

        m_transformInProgress = true;
        emit operationInProgressChanged(true);
        ++m_documentRevision;
        m_pagesLoading.clear();
        syncEditControls();

        const std::shared_ptr<PdfDocument> document = m_document;
        const QPointer<PdfViewerWidget> weakSelf(this);
        QThread *thread = QThread::create(
            [weakSelf, document, currentPageIds, targetPageIds,
             archivedPageIds, pageArchive, targetHistoryPosition]() {
                const bool restored = document->restorePageStructure(
                    currentPageIds, targetPageIds, archivedPageIds, pageArchive);
                const QVector<QSizeF> pageSizes = restored ? document->allPageSizes()
                                                           : QVector<QSizeF>();
                QMetaObject::invokeMethod(
                    qApp,
                    [weakSelf, restored, pageSizes, targetPageIds,
                     targetHistoryPosition]() {
                        if (weakSelf) {
                            weakSelf->finishStructureHistoryNavigation(
                                restored, pageSizes, targetPageIds,
                                targetHistoryPosition);
                        }
                    },
                    Qt::QueuedConnection);
            });
        connect(thread, &QThread::finished, thread, &QThread::deleteLater);
        thread->start();
        return;
    }

    const QVector<PdfPageState> states = redoOperation ? entry.afterStates
                                                       : entry.beforeStates;
    QVector<int> affectedPages;
    affectedPages.reserve(states.size());
    for (const PdfPageState &state : states)
        affectedPages.append(state.pageIndex);

    m_transformInProgress = true;
    emit operationInProgressChanged(true);
    ++m_documentRevision;
    m_pagesLoading.clear();
    syncEditControls();

    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    QThread *thread = QThread::create(
        [weakSelf, document, states, affectedPages, targetHistoryPosition]() {
            const bool restored = document->restorePageStates(states);
            const QVector<QSizeF> pageSizes = restored ? document->allPageSizes()
                                                       : QVector<QSizeF>();
            QMetaObject::invokeMethod(
                qApp,
                [weakSelf, restored, pageSizes, affectedPages, targetHistoryPosition]() {
                    if (weakSelf) {
                        weakSelf->finishHistoryNavigation(
                            restored, pageSizes, affectedPages, targetHistoryPosition);
                    }
                },
                Qt::QueuedConnection);
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::finishStructureHistoryNavigation(
    bool success, const QVector<QSizeF> &pageSizes,
    const QVector<quint64> &targetPageIds, int targetHistoryPosition)
{
    m_transformInProgress = false;
    emit operationInProgressChanged(false);
    success = success && pageSizes.size() == targetPageIds.size();
    if (success) {
        m_historyPosition = targetHistoryPosition;
        rebuildPagesAfterStructure(pageSizes, targetPageIds, {});
        updateModifiedState();
    } else {
        qWarning() << "Failed to restore PDF page structure history:" << m_filePath;
        syncEditControls();
        QTimer::singleShot(0, this, &PdfViewerWidget::renderVisiblePages);
    }
    emit historyChanged();
}

void PdfViewerWidget::finishHistoryNavigation(bool success,
                                              const QVector<QSizeF> &pageSizes,
                                              const QVector<int> &affectedPages,
                                              int targetHistoryPosition)
{
    m_transformInProgress = false;
    emit operationInProgressChanged(false);
    success = success && pageSizes.size() == m_pageLabels.size();

    if (success) {
        m_historyPosition = targetHistoryPosition;
        m_pageSizes = pageSizes;
        for (const int pageIndex : affectedPages) {
            if (pageIndex < 0 || pageIndex >= m_pageLabels.size())
                continue;
            m_pageLabels[pageIndex]->clear();
            m_renderedWidths[pageIndex] = 0;
        }
        m_regionPageIndex = -1;
        m_regionSelection = {};
        updateModifiedState();
        updateSelectionOverlays();
        applyZoom();
    } else {
        qWarning() << "Failed to restore PDF edit history state:" << m_filePath;
        syncEditControls();
        QTimer::singleShot(0, this, &PdfViewerWidget::renderVisiblePages);
    }
    emit historyChanged();
}

void PdfViewerWidget::updateModifiedState()
{
    const bool modified = m_historyPosition != m_savedHistoryPosition;
    if (modified == m_modified)
        return;
    m_modified = modified;
    emit modifiedChanged(modified);
}

void PdfViewerWidget::saveDocument(bool createTimestampedBackup, int backupVersionLimit)
{
    if (!m_valid || m_saveInProgress)
        return;
    if (m_transformInProgress) {
        emit saveFinished(false, QStringLiteral("An edit is still in progress."));
        return;
    }
    if (!m_modified) {
        emit saveFinished(true, {});
        return;
    }

    m_saveInProgress = true;
    m_transformInProgress = true;
    emit operationInProgressChanged(true);
    ++m_documentRevision;
    m_pagesLoading.clear();
    syncEditControls();

    const std::shared_ptr<PdfDocument> document = m_document;
    const QString filePath = m_filePath;
    const QPointer<PdfViewerWidget> weakSelf(this);
    QThread *thread = QThread::create(
        [weakSelf, document, filePath, createTimestampedBackup, backupVersionLimit]() {
            QString errorMessage;
            const bool success = document->saveSafely(
                filePath, createTimestampedBackup, backupVersionLimit, &errorMessage);
            QMetaObject::invokeMethod(
                qApp,
                [weakSelf, success, errorMessage]() {
                    if (weakSelf)
                        weakSelf->finishDocumentSave(success, errorMessage);
                },
                Qt::QueuedConnection);
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::finishDocumentSave(bool success, const QString &errorMessage)
{
    m_saveInProgress = false;
    m_transformInProgress = false;
    emit operationInProgressChanged(false);
    if (success) {
        m_savedHistoryPosition = m_historyPosition;
        updateModifiedState();
    }
    syncEditControls();
    QTimer::singleShot(0, this, &PdfViewerWidget::renderVisiblePages);
    emit saveFinished(success, errorMessage);
}

void PdfViewerWidget::setCurrentPageFromPointer(int pageIndex)
{
    if (pageIndex < 0 || pageIndex >= m_pageLabels.size()
        || pageIndex == m_currentPageIndex) {
        return;
    }

    m_currentPageIndex = pageIndex;
    syncNavigationControls();
    emit currentPageChanged(pageIndex);
}

QPointF PdfViewerWidget::normalizedPagePosition(const QLabel *label,
                                                const QPointF &position) const
{
    if (!label || label->width() <= 0 || label->height() <= 0)
        return {};

    return QPointF(qBound(0.0, position.x() / label->width(), 1.0),
                   qBound(0.0, position.y() / label->height(), 1.0));
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
    const QVariant pageIndexProperty = watched->property("pdfPageIndex");
    if (pageIndexProperty.isValid()) {
        const int pageIndex = pageIndexProperty.toInt();
        auto *label = static_cast<PdfPageLabel *>(watched);

        if (event->type() == QEvent::ContextMenu
            && m_selectionMode == SelectionMode::Page) {
            auto *contextEvent = static_cast<QContextMenuEvent *>(event);
            showPageContextMenu(pageIndex, contextEvent->globalPos());
            return true;
        }

        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() != Qt::LeftButton)
                return false;

            setCurrentPageFromPointer(pageIndex);
            if (m_selectionMode == SelectionMode::Page) {
                const bool extendRange = mouseEvent->modifiers().testFlag(Qt::ShiftModifier);
                const bool additive = mouseEvent->modifiers().testFlag(Qt::ControlModifier)
                                      || mouseEvent->modifiers().testFlag(Qt::MetaModifier);

                if (extendRange && m_pageSelectionAnchor >= 0) {
                    if (!additive)
                        m_selectedPages.clear();
                    const int first = qMin(m_pageSelectionAnchor, pageIndex);
                    const int last = qMax(m_pageSelectionAnchor, pageIndex);
                    for (int index = first; index <= last; ++index)
                        m_selectedPages.insert(index);
                } else if (additive) {
                    if (m_selectedPages.contains(pageIndex))
                        m_selectedPages.remove(pageIndex);
                    else
                        m_selectedPages.insert(pageIndex);
                    m_pageSelectionAnchor = pageIndex;
                } else if (!(m_organizePagesEnabled
                             && m_selectedPages.contains(pageIndex))) {
                    m_selectedPages = {pageIndex};
                    m_pageSelectionAnchor = pageIndex;
                }
                updateSelectionOverlays();
                if (m_organizePagesEnabled) {
                    m_pageDragStart = mouseEvent->position().toPoint();
                    m_pageDragSourceIndex = pageIndex;
                }
            } else {
                m_regionPageIndex = pageIndex;
                m_regionDragStart = normalizedPagePosition(label, mouseEvent->position());
                m_regionSelection = QRectF(m_regionDragStart, m_regionDragStart);
                m_draggingRegion = true;
                updateSelectionOverlays();
            }
            return true;
        }

        if (event->type() == QEvent::MouseMove
            && m_selectionMode == SelectionMode::Page && m_organizePagesEnabled
            && m_pageDragSourceIndex == pageIndex) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->buttons().testFlag(Qt::LeftButton)
                && (mouseEvent->position().toPoint() - m_pageDragStart).manhattanLength()
                       >= QApplication::startDragDistance()) {
                m_pageDragSourceIndex = -1;
                startSelectedPageDrag(label);
                return true;
            }
        }

        if (event->type() == QEvent::MouseMove && m_selectionMode == SelectionMode::Region
            && m_draggingRegion && pageIndex == m_regionPageIndex) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QPointF current = normalizedPagePosition(label, mouseEvent->position());
            m_regionSelection = QRectF(m_regionDragStart, current).normalized();
            updateSelectionOverlays();
            return true;
        }

        if (event->type() == QEvent::MouseButtonRelease
            && m_selectionMode == SelectionMode::Region && m_draggingRegion
            && pageIndex == m_regionPageIndex) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QPointF current = normalizedPagePosition(label, mouseEvent->position());
            m_regionSelection = QRectF(m_regionDragStart, current).normalized();
            m_draggingRegion = false;
            if (m_regionSelection.width() * label->width() < 4
                || m_regionSelection.height() * label->height() < 4) {
                m_regionPageIndex = -1;
                m_regionSelection = {};
            }
            updateSelectionOverlays();
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease
            && m_selectionMode == SelectionMode::Page) {
            m_pageDragSourceIndex = -1;
        }
    }

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
    if (m_transformInProgress)
        return;
    if (m_pagesLoading.value(pageIndex) == widthPx)
        return;
    m_pagesLoading.insert(pageIndex, widthPx);

    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    const int documentRevision = m_documentRevision;
    QThread *thread = QThread::create(
        [weakSelf, document, pageIndex, widthPx, documentRevision]() {
        const QImage image = document->renderPage(pageIndex, widthPx);
        QMetaObject::invokeMethod(
            qApp,
            [weakSelf, pageIndex, widthPx, documentRevision, image]() {
                if (weakSelf)
                    weakSelf->onPageRendered(pageIndex, widthPx, documentRevision, image);
            },
            Qt::QueuedConnection);
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::onPageRendered(int pageIndex, int widthPx, int documentRevision,
                                     const QImage &image)
{
    if (documentRevision != m_documentRevision)
        return;

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
