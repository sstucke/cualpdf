#include "pdfviewerwidget.h"

#include "appsettings.h"
#include "imageenhancement.h"
#include "pdfdocument.h"
#include "printdialog.h"

#include <QActionGroup>
#include <QDesktopServices>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QButtonGroup>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QColorDialog>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDir>
#include <QDropEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QHash>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
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
#include <QRubberBand>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSaveFile>
#include <QSlider>
#include <QStandardPaths>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
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
        // The hit area now overlaps neighboring page thumbnails (see
        // rebuildPageLayout's kHoverHitPadding). Without this, Qt erases the
        // widget's rect to the background color before every paintEvent,
        // which would blank out that overlap even when we intentionally
        // paint nothing (at rest).
        setAttribute(Qt::WA_NoSystemBackground, true);
    }

    void setHovered(bool hovered)
    {
        if (m_hovered == hovered)
            return;
        m_hovered = hovered;
        update();
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        QWidget::enterEvent(event);
        setHovered(true);
    }

    void leaveEvent(QEvent *event) override
    {
        QWidget::leaveEvent(event);
        // While our own context menu is open, the popup's mouse grab
        // generates a spurious Leave (the cursor hasn't actually moved) —
        // ignore it, contextMenuEvent() manages hover for that duration.
        if (!m_menuOpen)
            setHovered(false);
    }

    void paintEvent(QPaintEvent *) override
    {
        // Only show the insertion indicator when the cursor (or an active
        // page drag) is actually over this gap; at rest it stays blank
        // (still clickable for the context menu, and still a valid drop
        // target). Sibling gaps stay blank too while a drag is in progress
        // elsewhere — only the one under the cursor lights up, matching
        // Acrobat's single moving insertion line and Foxit's per-gap hover
        // icon.
        if (!m_dropTarget && !m_hovered)
            return;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        // Always accent blue: page content is unpredictable (white, colored,
        // scanned), so a theme-derived gray line can vanish against it. Only
        // the intensity changes between a resting hover and an active drop
        // target.
        const QColor accent(QStringLiteral("#2684ff"));
        const bool horizontal = width() >= height();
        const QPointF first = horizontal ? QPointF(4, height() / 2.0)
                                         : QPointF(width() / 2.0, 4);
        const QPointF second = horizontal ? QPointF(width() - 4, height() / 2.0)
                                          : QPointF(width() / 2.0, height() - 4);
        // A single solid stroke, matching how comparable apps (Acrobat,
        // Figma layer reordering, etc.) show an insertion point.
        const qreal lineWidth = m_dropTarget ? 4.0 : 2.0;
        painter.setPen(QPen(accent, lineWidth, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(first, second);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (m_contextMenuHandler) {
            m_menuOpen = true;
            setHovered(true);
            m_contextMenuHandler(m_insertionIndex, event->globalPos());
            m_menuOpen = false;
            setHovered(underMouse());
        }
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
    bool m_dropTarget = false;
    bool m_hovered = false;
    bool m_menuOpen = false;
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

// "Aclarar": flattens uneven photo shading and boosts contrast so a camera
// photo of a page reads closer to a flatbed scan. The slider re-runs the
// enhancement (debounced) on the low-res thumbnail already on screen, so the
// preview stays responsive; the full-resolution pass only happens on Apply.
class LightenPageDialog final : public QDialog
{
public:
    LightenPageDialog(const QImage &previewSource, int initialAmount,
                      QWidget *parent = nullptr)
        : QDialog(parent)
        , m_previewSource(previewSource)
        , m_previewLabel(new QLabel(this))
        , m_slider(new QSlider(Qt::Horizontal, this))
        , m_amountLabel(new QLabel(this))
    {
        setWindowTitle(viewerText("Lighten Page"));
        resize(480, 560);

        auto *dialogLayout = new QVBoxLayout(this);
        m_previewLabel->setAlignment(Qt::AlignCenter);
        m_previewLabel->setMinimumSize(300, 380);
        m_previewLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_previewLabel->setStyleSheet(
            QStringLiteral("background: white; border: 1px solid #667085;"));
        dialogLayout->addWidget(m_previewLabel, 1);

        auto *sliderLayout = new QHBoxLayout;
        sliderLayout->addWidget(new QLabel(viewerText("Amount:"), this));
        m_slider->setRange(0, 100);
        m_slider->setValue(initialAmount);
        sliderLayout->addWidget(m_slider, 1);
        m_amountLabel->setMinimumWidth(44);
        m_amountLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderLayout->addWidget(m_amountLabel);
        dialogLayout->addLayout(sliderLayout);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(viewerText("Apply"));
        buttons->button(QDialogButtonBox::Cancel)->setText(viewerText("Cancel"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        dialogLayout->addWidget(buttons);

        m_previewTimer.setSingleShot(true);
        m_previewTimer.setInterval(120);
        connect(&m_previewTimer, &QTimer::timeout, this, &LightenPageDialog::updatePreview);
        connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
            m_amountLabel->setText(QStringLiteral("%1%").arg(value));
            m_previewTimer.start();
        });

        m_amountLabel->setText(QStringLiteral("%1%").arg(initialAmount));
        updatePreview();
    }

    int amount() const { return m_slider->value(); }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QDialog::resizeEvent(event);
        renderPreview();
    }

private:
    void updatePreview()
    {
        const QImage enhanced =
            ImageEnhancement::aclararPapel(m_previewSource, m_slider->value());
        m_previewPixmap = QPixmap::fromImage(enhanced);
        renderPreview();
    }

    void renderPreview()
    {
        if (m_previewPixmap.isNull())
            return;
        m_previewLabel->setPixmap(m_previewPixmap.scaled(
            m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    QImage m_previewSource;
    QPixmap m_previewPixmap;
    QLabel *m_previewLabel;
    QSlider *m_slider;
    QLabel *m_amountLabel;
    QTimer m_previewTimer;
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

    void setObjectOverlay(const QVector<QRect> &bounds, int selectedIndex, int hoveredIndex,
                          const QPoint &dragOffset, const QVector<int> &guideXs,
                          const QVector<int> &guideYs)
    {
        m_objectBounds = bounds;
        m_selectedObject = selectedIndex;
        m_hoveredObject = hoveredIndex;
        m_objectDragOffset = dragOffset;
        m_guideXs = guideXs;
        m_guideYs = guideYs;
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

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(QStringLiteral("#e040a0")), 1));
        for (const int x : m_guideXs)
            painter.drawLine(x, 0, x, height());
        for (const int y : m_guideYs)
            painter.drawLine(0, y, width(), y);

        for (int index = 0; index < m_objectBounds.size(); ++index) {
            if (index != m_selectedObject && index != m_hoveredObject)
                continue;
            QRect bounds = m_objectBounds[index];
            const bool selected = index == m_selectedObject;
            if (selected)
                bounds.translate(m_objectDragOffset);
            painter.setPen(QPen(accent, selected ? 2.0 : 1.0));
            painter.drawRect(bounds);
        }
    }

private:
    bool m_pageSelected = false;
    QRectF m_regionSelection;
    QVector<QRect> m_objectBounds;
    int m_selectedObject = -1;
    int m_hoveredObject = -1;
    QPoint m_objectDragOffset;
    QVector<int> m_guideXs;
    QVector<int> m_guideYs;
};
}

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
        // One sheet, the single-page glyph Acrobat and Foxit use in the footer.
        painter.drawRoundedRect(QRectF(6.5, 1.5, 13, 17), 1.2, 1.2);
    }
    return QIcon(pixmap);
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
    m_pagesContainer->installEventFilter(this);

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

    toolbar->addSeparator();

    m_editPdfButton = new QToolButton(toolbar);
    m_editPdfButton->setCheckable(true);
    m_editPdfButton->setText(tr("Edit PDF"));
    m_editPdfButton->setToolTip(tr("Edit text and images, then organize pages"));
    m_editPdfButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addWidget(m_editPdfButton);

    auto *printButton = new QToolButton(toolbar);
    printButton->setText(tr("Print"));
    printButton->setIcon(QIcon::fromTheme(QStringLiteral("document-print")));
    printButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    printButton->setToolTip(tr("Print…"));
    toolbar->addWidget(printButton);
    connect(printButton, &QToolButton::clicked, this, &PdfViewerWidget::printDocument);

    m_previousPageButton->setEnabled(false);
    m_pageSpinBox->setEnabled(false);
    m_nextPageButton->setEnabled(false);

    connect(m_editPdfButton, &QToolButton::toggled, this, &PdfViewerWidget::setEditingPdf);

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
    setSelectionMode(SelectionMode::Read);
    syncViewControl();
    syncFitControl();
    syncZoomControl();

    m_editToolbar = new QToolBar(this);
    m_editToolbar->setObjectName(QStringLiteral("editToolBar"));
    m_editToolbar->setMovable(false);
    m_editToolbar->setFloatable(false);
    m_editToolbar->setIconSize(QSize(28, 24));
    layout()->addWidget(m_editToolbar);
    auto *editToolbar = m_editToolbar;

    auto *selectionGroup = new QButtonGroup(editToolbar);
    selectionGroup->setExclusive(true);

    m_selectPageButton = new QToolButton(editToolbar);
    m_selectPageButton->setCheckable(true);
    m_selectPageButton->setIcon(selectionModeIcon(true, iconColor));
    m_selectPageButton->setText(tr("Select Page"));
    m_selectPageButton->setToolTip(tr("Select pages"));
    m_selectPageButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    selectionGroup->addButton(m_selectPageButton);
    editToolbar->addWidget(m_selectPageButton);

    m_selectRegionButton = new QToolButton(editToolbar);
    m_selectRegionButton->setCheckable(true);
    m_selectRegionButton->setIcon(selectionModeIcon(false, iconColor));
    m_selectRegionButton->setText(tr("Select Region"));
    m_selectRegionButton->setToolTip(tr("Select a rectangular region"));
    m_selectRegionButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    selectionGroup->addButton(m_selectRegionButton);
    editToolbar->addWidget(m_selectRegionButton);

    m_pageSelectionCombo = new QComboBox(editToolbar);
    m_pageSelectionCombo->setFixedWidth(110);
    m_pageSelectionCombo->setPlaceholderText(tr("Select"));
    m_pageSelectionCombo->addItem(tr("All"));
    m_pageSelectionCombo->addItem(tr("None"));
    m_pageSelectionCombo->addItem(tr("Even"));
    m_pageSelectionCombo->addItem(tr("Odd"));
    m_pageSelectionCombo->setCurrentIndex(-1);
    m_pageSelectionCombo->setEnabled(false);
    editToolbar->addWidget(m_pageSelectionCombo);

    editToolbar->addSeparator();

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
    auto *clearSelectionAction = new QAction(this);
    clearSelectionAction->setShortcut(Qt::Key_Escape);
    clearSelectionAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(clearSelectionAction);
    connect(clearSelectionAction, &QAction::triggered, this, [this]() {
        if (!m_organizePagesEnabled || m_selectedPages.isEmpty())
            return;
        m_selectedPages.clear();
        m_pageSelectionAnchor = -1;
        updateSelectionOverlays();
    });
    m_deletePagesAction = new QAction(tr("Delete"), this);
    m_deletePagesAction->setShortcut(QKeySequence::Delete);
    m_deletePagesAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(m_deletePagesAction);
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
    connect(m_deletePagesAction, &QAction::triggered, this,
            &PdfViewerWidget::deleteSelectedPages);
    connect(m_copyPagesAction, &QAction::triggered, this,
            [this]() { copySelectedPages(/*cut=*/false); });
    connect(m_extractPagesAction, &QAction::triggered,
            this, &PdfViewerWidget::extractSelectedPages);
    connect(m_rotateCounterclockwiseButton, &QToolButton::clicked, this,
            [this]() { rotateSelectedPages(false); });
    connect(m_rotateClockwiseButton, &QToolButton::clicked, this,
            [this]() { rotateSelectedPages(true); });
    connect(m_selectPageButton, &QToolButton::clicked, this,
            [this]() { setSelectionMode(SelectionMode::Page); });
    connect(m_selectRegionButton, &QToolButton::clicked, this,
            [this]() { setSelectionMode(SelectionMode::Region); });
    connect(m_pageSelectionCombo, &QComboBox::activated,
            this, &PdfViewerWidget::applyPageSelectionCommand);
    m_addTextButton = new QToolButton(editToolbar);
    m_addTextButton->setText(tr("Add Text"));
    m_addTextButton->setToolTip(tr("Click the page to place a new text block"));
    m_addTextButton->setCheckable(true);
    m_addTextButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    editToolbar->addWidget(m_addTextButton);
    connect(m_addTextButton, &QToolButton::toggled, this, [this](bool on) {
        m_placingText = on;
    });

    m_editToolbar->hide();
    setSelectionMode(SelectionMode::Read);
    syncEditControls();

    m_textFormatBar = new QWidget(this);
    auto *formatLayout = new QHBoxLayout(m_textFormatBar);
    formatLayout->setContentsMargins(8, 2, 8, 2);
    formatLayout->setSpacing(4);

    m_fontCombo = new QComboBox(m_textFormatBar);
    m_fontCombo->setEditable(true);
    m_fontCombo->setInsertPolicy(QComboBox::NoInsert);
    m_fontCombo->setMinimumWidth(180);
    m_fontCombo->setMaxVisibleItems(12);
    auto *fontList = new QListView(m_fontCombo);
    fontList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_fontCombo->setView(fontList);
    auto *fontCompleter = new QCompleter(m_fontCombo->model(), m_fontCombo);
    fontCompleter->setFilterMode(Qt::MatchContains);
    fontCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    fontCompleter->setCompletionMode(QCompleter::PopupCompletion);
    fontCompleter->setMaxVisibleItems(12);
    m_fontCombo->setCompleter(fontCompleter);
    m_fontSizeCombo = new QComboBox(m_textFormatBar);
    m_fontSizeCombo->setEditable(true);
    m_fontSizeCombo->setInsertPolicy(QComboBox::NoInsert);
    m_fontSizeCombo->setFixedWidth(64);
    m_fontSizeCombo->setToolTip(tr("Size"));
    for (const int size : {6, 7, 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48, 72})
        m_fontSizeCombo->addItem(QString::number(size));
    m_boldButton = new QToolButton(m_textFormatBar);
    m_boldButton->setText(QStringLiteral("B"));
    m_boldButton->setCheckable(true);
    m_boldButton->setToolTip(tr("Bold"));
    QFont boldFont = m_boldButton->font();
    boldFont.setBold(true);
    m_boldButton->setFont(boldFont);
    m_italicButton = new QToolButton(m_textFormatBar);
    m_italicButton->setText(QStringLiteral("I"));
    m_italicButton->setCheckable(true);
    m_italicButton->setToolTip(tr("Italic"));
    QFont italicFont = m_italicButton->font();
    italicFont.setItalic(true);
    m_italicButton->setFont(italicFont);
    m_textColorButton = new QToolButton(m_textFormatBar);
    m_textColorButton->setToolTip(tr("Text color"));
    m_alignLeftButton = new QToolButton(m_textFormatBar);
    m_alignLeftButton->setText(QStringLiteral("⟸"));
    m_alignLeftButton->setToolTip(tr("Align left"));
    m_alignCenterButton = new QToolButton(m_textFormatBar);
    m_alignCenterButton->setText(QStringLiteral("≡"));
    m_alignCenterButton->setToolTip(tr("Align center"));
    m_alignRightButton = new QToolButton(m_textFormatBar);
    m_alignRightButton->setText(QStringLiteral("⟹"));
    m_alignRightButton->setToolTip(tr("Align right"));
    formatLayout->addWidget(m_fontCombo);
    formatLayout->addWidget(m_fontSizeCombo);
    formatLayout->addWidget(m_boldButton);
    formatLayout->addWidget(m_italicButton);
    formatLayout->addWidget(m_textColorButton);
    formatLayout->addSpacing(8);
    formatLayout->addWidget(m_alignLeftButton);
    formatLayout->addWidget(m_alignCenterButton);
    formatLayout->addWidget(m_alignRightButton);
    m_textFormatBar->setObjectName(QStringLiteral("textFormatBar"));
    m_textFormatBar->setAttribute(Qt::WA_StyledBackground);
    m_textFormatBar->setStyleSheet(QStringLiteral(
        "QWidget#textFormatBar { background: palette(window); border: 1px solid palette(mid); border-radius: 6px; }"
        "QToolButton { border: none; border-radius: 4px; padding: 2px 7px; min-width: 18px; }"
        "QToolButton:checked { background: palette(highlight); color: palette(highlighted-text); }"
        "QToolButton:hover { background: palette(midlight); }"));
    m_textFormatBar->hide();
    m_textFormatBar->raise();

    connect(m_fontCombo, &QComboBox::activated, this, [this](int) { applyTextTypeface(); });
    connect(m_fontCombo->lineEdit(), &QLineEdit::editingFinished, this, [this]() {
        if (!m_updatingTextFormat)
            applyTextTypeface();
    });
    connect(m_fontSizeCombo, &QComboBox::activated, this, [this](int) {
        applyTextFontSize(m_fontSizeCombo->currentText().toInt());
    });
    connect(m_fontSizeCombo->lineEdit(), &QLineEdit::editingFinished, this, [this]() {
        if (!m_updatingTextFormat)
            applyTextFontSize(m_fontSizeCombo->currentText().toInt());
    });
    connect(m_boldButton, &QToolButton::toggled, this, [this](bool) { applyTextTypeface(); });
    connect(m_italicButton, &QToolButton::toggled, this, [this](bool) { applyTextTypeface(); });
    connect(m_textColorButton, &QToolButton::clicked, this, &PdfViewerWidget::applyTextColor);
    connect(m_alignLeftButton, &QToolButton::clicked, this, [this]() { applyTextAlignment(0); });
    connect(m_alignCenterButton, &QToolButton::clicked, this, [this]() { applyTextAlignment(1); });
    connect(m_alignRightButton, &QToolButton::clicked, this, [this]() { applyTextAlignment(2); });
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

bool PdfViewerWidget::pageInDiscreteChunk(int pageIndex) const
{
    const int firstPage = discreteChunkStart();
    const int lastPage = qMin(firstPage + discretePageCount(), m_pageLabels.size());
    return pageIndex >= firstPage && pageIndex < lastPage;
}

bool PdfViewerWidget::pageImageReady(int pageIndex) const
{
    if (pageIndex < 0 || pageIndex >= m_pageLabels.size())
        return false;
    const QLabel *label = m_pageLabels[pageIndex];
    return m_renderedWidths[pageIndex] == label->width()
           && !label->pixmap(Qt::ReturnByValue).isNull();
}

bool PdfViewerWidget::discreteChunkReady() const
{
    const int firstPage = discreteChunkStart();
    const int lastPage = qMin(firstPage + discretePageCount(), m_pageLabels.size());
    if (firstPage >= lastPage)
        return false;
    for (int pageIndex = firstPage; pageIndex < lastPage; ++pageIndex) {
        if (!pageImageReady(pageIndex))
            return false;
    }
    return true;
}

bool PdfViewerWidget::holdDiscretePage() const
{
    if (m_pageLayout != PageLayout::Discrete)
        return false;
    // The first paint has nothing to keep on screen. Later turns stay on the
    // page already drawn until the destination bitmaps exist, so the view
    // does not flash empty while PDFium renders.
    bool somethingVisible = false;
    for (const QLabel *label : m_pageLabels) {
        if (!label->isHidden()) {
            somethingVisible = true;
            break;
        }
    }
    return somethingVisible && !discreteChunkReady();
}

void PdfViewerWidget::scheduleDiscreteChunkRenders()
{
    const int firstPage = discreteChunkStart();
    const int lastPage = qMin(firstPage + discretePageCount(), m_pageLabels.size());
    for (int pageIndex = firstPage; pageIndex < lastPage; ++pageIndex)
        scheduleRender(pageIndex, m_pageLabels[pageIndex]->width());
}

void PdfViewerWidget::revealDiscreteChunk()
{
    if (m_pageLayout != PageLayout::Discrete || !discreteChunkReady())
        return;

    const int firstPage = discreteChunkStart();
    const int lastPage = qMin(firstPage + discretePageCount(), m_pageLabels.size());
    for (int pageIndex = firstPage; pageIndex < lastPage; ++pageIndex) {
        if (m_pageLabels[pageIndex]->isHidden()) {
            rebuildPageLayout();
            scrollToCurrentPage();
            return;
        }
    }
}

namespace {
struct PaintingPaused {
    QWidget *first = nullptr;
    QWidget *second = nullptr;
    PaintingPaused(QWidget *firstWidget, QWidget *secondWidget)
        : first(firstWidget)
        , second(secondWidget)
    {
        first->setUpdatesEnabled(false);
        second->setUpdatesEnabled(false);
    }
    ~PaintingPaused()
    {
        second->setUpdatesEnabled(true);
        first->setUpdatesEnabled(true);
    }
    PaintingPaused(const PaintingPaused &) = delete;
    PaintingPaused &operator=(const PaintingPaused &) = delete;
};
}

void PdfViewerWidget::rebuildPageLayout()
{
    if (holdDiscretePage()) {
        scheduleDiscreteChunkRenders();
        return;
    }

    // One paint for the whole swap. Hiding the old page and shrinking the
    // canvas otherwise reaches the screen as an empty frame.
    const PaintingPaused paused(m_scrollArea->viewport(), m_pagesContainer);

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
        } else if (previousVisible) {
            // Row wrap or end of list: keep the indicator vertical, anchored to
            // the right edge of the last page before the gap (Acrobat/Foxit
            // style) instead of a horizontal bar above/below a row.
            geometry = QRect(previous->geometry().right() + 1, previous->y(),
                             qMax(10, kPageSpacing), previous->height());
        } else {
            // insertionIndex == 0: vertical indicator to the left of the very
            // first page.
            geometry = QRect(qMax(0, next->x() - kPageSpacing), next->y(),
                             qMax(10, kPageSpacing), next->height());
        }
        // The true gap is only ~kPageSpacing wide, too thin a target to
        // reliably hover. Pad the widget's hit area symmetrically into the
        // neighboring pages (raise() below keeps it on top there) without
        // moving the drawn line, which stays centered on the widget.
        constexpr int kHoverHitPadding = 10;
        geometry = geometry.adjusted(-kHoverHitPadding, 0, kHoverHitPadding, 0);
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

bool PdfViewerWidget::continuousPageLayout() const
{
    return m_pageLayout == PageLayout::Continuous;
}

void PdfViewerWidget::setContinuousPageLayout(bool continuous)
{
    setPageLayout(continuous ? PageLayout::Continuous : PageLayout::Discrete);
}

void PdfViewerWidget::setPageLayout(PageLayout layout)
{
    if (m_pageLayout == layout)
        return;

    m_pageLayout = layout;
    if (m_valid)
        applyZoom();
    else
        syncViewControl();
    emit pageLayoutChanged(layout == PageLayout::Continuous);
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
        // While the next discrete page is still rendering, leave the picture
        // on screen untouched. Resizing or replacing it is the leftover blink.
        if (m_pageLayout == PageLayout::Discrete && !label->isHidden()
            && !pageInDiscreteChunk(i) && !discreteChunkReady()) {
            continue;
        }
        if (label->width() != width || label->height() != height) {
            const QPixmap current = label->pixmap(Qt::ReturnByValue);
            if (!current.isNull()) {
                label->setPixmap(current.scaled(width, height, Qt::KeepAspectRatio,
                                                Qt::SmoothTransformation));
            }
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
        if (m_selectionMode == SelectionMode::Objects)
            refreshPageObjects();
    });
}

void PdfViewerWidget::setCustomZoom(int percent)
{
    m_zoomMode = ZoomMode::Custom;
    m_zoomPercent = qBound(kMinimumZoom, percent, kMaximumZoom);
    applyZoom();
}

void PdfViewerWidget::printDocument()
{
    if (!m_valid || !m_document)
        return;
    PrintDialog dialog(m_document, m_currentPageIndex, this);
    dialog.exec();
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
    m_selectionMode = mode;

    QButtonGroup *selectionGroup = m_selectPageButton ? m_selectPageButton->group() : nullptr;
    if (selectionGroup)
        selectionGroup->setExclusive(false);
    if (m_selectPageButton)
        m_selectPageButton->setChecked(mode == SelectionMode::Page);
    if (m_selectRegionButton)
        m_selectRegionButton->setChecked(mode == SelectionMode::Region);
    if (selectionGroup)
        selectionGroup->setExclusive(true);
    if (m_pageSelectionCombo)
        m_pageSelectionCombo->setVisible(mode == SelectionMode::Page);

    const Qt::CursorShape cursor = mode == SelectionMode::Region
                                       ? Qt::CrossCursor
                                       : (mode == SelectionMode::Page && m_organizePagesEnabled
                                              ? Qt::OpenHandCursor
                                              : Qt::ArrowCursor);
    for (QLabel *label : m_pageLabels)
        label->setCursor(cursor);
    if (mode != SelectionMode::Objects) {
        m_selectedObject = -1;
        m_draggingObject = false;
        m_objectDragOffset = {};
    }
    updateSelectionOverlays();
    refreshPageObjects();
}

void PdfViewerWidget::setEditingPdf(bool enabled)
{
    m_editingPdf = enabled;
    if (m_editToolbar)
        m_editToolbar->setVisible(enabled);
    if (!enabled && m_organizePagesEnabled) {
        m_organizePagesEnabled = false;
        if (m_organizePagesButton) {
            const QSignalBlocker blocker(m_organizePagesButton);
            m_organizePagesButton->setChecked(false);
        }
        rebuildPageLayout();
    }
    setSelectionMode(enabled ? SelectionMode::Objects : SelectionMode::Read);
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
    } else if (m_editingPdf) {
        setSelectionMode(SelectionMode::Objects);
    }
    rebuildPageLayout();
    syncEditControls();
}

void PdfViewerWidget::showPageContextMenu(int pageIndex, const QPoint &globalPosition)
{
    if (pageIndex < 0 || pageIndex >= m_pageLabels.size())
        return;

    // In Region mode, right-clicking must not clobber the in-progress region
    // selection with a whole-page selection — only Page mode auto-selects
    // the clicked page here.
    if (m_selectionMode == SelectionMode::Page && !m_selectedPages.contains(pageIndex)) {
        m_selectedPages = {pageIndex};
        m_pageSelectionAnchor = pageIndex;
        setCurrentPageFromPointer(pageIndex);
        updateSelectionOverlays();
    }

    const bool hasRegionSelection = m_selectionMode == SelectionMode::Region
                                    && m_regionPageIndex >= 0
                                    && !m_regionSelection.isEmpty();

    QMenu menu(this);
    QAction *editImageAction = nullptr;
    if (m_selectionMode == SelectionMode::Objects && m_selectedObject >= 0
        && m_selectedObject < m_pageObjects.size()
        && m_pageObjects[m_selectedObject].kind == PdfPageObjectKind::Image) {
        editImageAction = menu.addAction(tr("Edit Image…"));
        menu.addSeparator();
    }
    QAction *cutAction = menu.addAction(tr("Cut"));
    cutAction->setShortcut(QKeySequence::Cut);
    QAction *deleteAction = menu.addAction(tr("Delete"));
    deleteAction->setShortcut(QKeySequence::Delete);
    QAction *copyAction = menu.addAction(tr("Copy"));
    copyAction->setShortcut(QKeySequence::Copy);
    menu.addSeparator();
    QAction *extractAction = menu.addAction(tr("Extract PDF…"));
    menu.addSeparator();
    QAction *lightenAction = menu.addAction(tr("Lighten Page…"));
    QAction *invertAction = menu.addAction(tr("Invert Colors"));
    QAction *flattenAction = menu.addAction(tr("Flatten"));
    QAction *autoCropAction = menu.addAction(tr("Auto Crop"));
    QAction *splitAction = menu.addAction(tr("Split Page"));
    QAction *clearViewAction = menu.addAction(tr("Clear Rotation and Crop"));
    QAction *scanAction = menu.addAction(tr("Improve Scan…"));
    QAction *perspectiveAction = menu.addAction(tr("Correct Perspective…"));
    QAction *externalAction = menu.addAction(tr("Edit Page Externally…"));
    QAction *compressAction = menu.addAction(tr("Compress"));
    const bool canCopy = m_valid && !m_transformInProgress && !m_saveInProgress
                         && !m_selectedPages.isEmpty();
    copyAction->setEnabled(canCopy);
    const bool canRemove = canCopy && m_selectedPages.size() < m_pageLabels.size();
    cutAction->setEnabled(canRemove);
    deleteAction->setEnabled(canRemove);
    extractAction->setEnabled(canCopy);
    const bool canLighten = m_valid && !m_transformInProgress && !m_saveInProgress
                           && (!m_selectedPages.isEmpty() || hasRegionSelection);
    lightenAction->setEnabled(canLighten);
    invertAction->setEnabled(canCopy);
    flattenAction->setEnabled(canCopy);
    autoCropAction->setEnabled(canCopy);
    splitAction->setEnabled(canCopy);
    clearViewAction->setEnabled(canCopy);
    scanAction->setEnabled(canCopy);
    perspectiveAction->setEnabled(canCopy && m_selectedPages.size() == 1);
    externalAction->setEnabled(canCopy && m_selectedPages.size() == 1);
    compressAction->setEnabled(m_valid && !m_transformInProgress && !m_saveInProgress);

    QAction *selectedAction = menu.exec(globalPosition);
    if (editImageAction && selectedAction == editImageAction)
        editSelectedImage();
    else if (selectedAction == cutAction)
        copySelectedPages(/*cut=*/true);
    else if (selectedAction == deleteAction)
        deleteSelectedPages();
    else if (selectedAction == copyAction)
        copySelectedPages(/*cut=*/false);
    else if (selectedAction == lightenAction)
        lightenSelectedPage(hasRegionSelection ? m_regionPageIndex : -1);
    else if (selectedAction == invertAction)
        invertSelectedPages();
    else if (selectedAction == flattenAction)
        flattenSelectedPages();
    else if (selectedAction == autoCropAction)
        autoCropSelectedPages();
    else if (selectedAction == splitAction)
        splitSelectedPages();
    else if (selectedAction == clearViewAction)
        clearSelectedPageView();
    else if (selectedAction == scanAction)
        improveSelectedScans();
    else if (selectedAction == perspectiveAction)
        correctSelectedPerspective();
    else if (selectedAction == externalAction)
        editSelectedPageExternally();
    else if (selectedAction == compressAction)
        compressDocument();
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

void PdfViewerWidget::beginPageMarquee(const QPoint &containerPosition, Qt::KeyboardModifiers modifiers)
{
    const bool additive = modifiers.testFlag(Qt::ControlModifier)
                          || modifiers.testFlag(Qt::MetaModifier)
                          || modifiers.testFlag(Qt::ShiftModifier);
    m_marqueeSelecting = true;
    m_marqueeOrigin = containerPosition;
    m_marqueeBase = additive ? m_selectedPages : QSet<int>{};
    if (!additive) {
        m_selectedPages.clear();
        m_pageSelectionAnchor = -1;
        updateSelectionOverlays();
    }
    if (!m_marquee)
        m_marquee = new QRubberBand(QRubberBand::Rectangle, m_pagesContainer);
    m_marquee->setGeometry(QRect(containerPosition, QSize()));
    m_marquee->show();
    m_pagesContainer->grabMouse();
}

void PdfViewerWidget::updatePageMarquee(const QPoint &containerPosition)
{
    if (!m_marqueeSelecting)
        return;
    const QRect band = QRect(m_marqueeOrigin, containerPosition).normalized();
    if (m_marquee)
        m_marquee->setGeometry(band);
    QSet<int> next = m_marqueeBase;
    for (int index = 0; index < m_pageLabels.size(); ++index) {
        if (m_pageLabels[index]->geometry().intersects(band))
            next.insert(index);
    }
    if (next != m_selectedPages) {
        m_selectedPages = next;
        if (!next.isEmpty())
            m_pageSelectionAnchor = *std::min_element(next.cbegin(), next.cend());
        updateSelectionOverlays();
    }
}

void PdfViewerWidget::finishPageMarquee()
{
    if (!m_marqueeSelecting)
        return;
    m_marqueeSelecting = false;
    if (m_marquee)
        m_marquee->hide();
    if (m_pagesContainer->mouseGrabber() == m_pagesContainer)
        m_pagesContainer->releaseMouse();
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

    sourceLabel->setCursor(Qt::ClosedHandCursor);
    drag->exec(Qt::MoveAction);
    sourceLabel->setCursor(Qt::OpenHandCursor);
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

void PdfViewerWidget::deleteSelectedPages()
{
    copySelectedPages(/*cut=*/true, /*keepOnClipboard=*/false);
}

void PdfViewerWidget::copySelectedPages(bool cut)
{
    copySelectedPages(cut, /*keepOnClipboard=*/true);
}

void PdfViewerWidget::copySelectedPages(bool cut, bool keepOnClipboard)
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
        [weakSelf, document, selectedIndexes, selectedPageIds, beforePageIds, cut,
         keepOnClipboard]() {
            const QByteArray pageArchive = document->exportPages(selectedIndexes);
            QMetaObject::invokeMethod(
                qApp,
                [weakSelf, pageArchive, selectedPageIds, beforePageIds, cut, keepOnClipboard]() {
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
                    if (keepOnClipboard) {
                        g_pageClipboard = {
                            pageArchive, static_cast<int>(selectedPageIds.size())};
                    }
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
                        viewerText(keepOnClipboard ? "Cut pages" : "Delete pages"),
                        beforePageIds, targetPageIds,
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
    if (g_pageClipboard.pageArchive.isEmpty() || g_pageClipboard.pageCount <= 0) {
        const QMimeData *mime = QApplication::clipboard()->mimeData();
        if (mime && mime->hasImage()) {
            const QImage image = qvariant_cast<QImage>(mime->imageData());
            const QSizeF size = pageSize(insertionIndex > 0 ? insertionIndex - 1 : 0);
            const QByteArray archive = PdfDocument::createImagePageArchive(image, size);
            if (!archive.isEmpty())
                insertArchiveAt(insertionIndex, archive, 1, tr("Paste image"));
            return;
        }
        if (mime) {
            const QList<QUrl> urls = mime->urls();
            for (const QUrl &url : urls) {
                if (!url.isLocalFile())
                    continue;
                const QString path = url.toLocalFile();
                if (path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
                    PdfDocument source(path);
                    if (!source.isValid())
                        return;
                    QVector<int> indexes;
                    for (int i = 0; i < source.pageCount(); ++i)
                        indexes.append(i);
                    const QByteArray archive = source.exportPages(indexes);
                    if (!archive.isEmpty())
                        insertArchiveAt(insertionIndex, archive, source.pageCount(), tr("Paste PDF"));
                    return;
                }
            }
            const QByteArray pdf = mime->data(QStringLiteral("application/pdf"));
            if (!pdf.isEmpty()) {
                const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                         .filePath(QStringLiteral("cualpdf-clipboard.pdf"));
                QFile file(path);
                if (file.open(QIODevice::WriteOnly)) {
                    file.write(pdf);
                    file.close();
                    PdfDocument source(path);
                    if (source.isValid()) {
                        const QByteArray archive = source.exportPages(
                            [&]() {
                                QVector<int> indexes;
                                for (int i = 0; i < source.pageCount(); ++i)
                                    indexes.append(i);
                                return indexes;
                            }());
                        if (!archive.isEmpty())
                            insertArchiveAt(insertionIndex, archive, source.pageCount(),
                                            tr("Paste PDF"));
                    }
                }
            }
        }
        return;
    }
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
    QVector<int> sortedSelection(m_selectedPages.cbegin(), m_selectedPages.cend());
    std::sort(sortedSelection.begin(), sortedSelection.end());
    m_currentPageIndex = sortedSelection.isEmpty() ? 0 : sortedSelection.first();
    m_pageSelectionAnchor = sortedSelection.isEmpty() ? -1 : sortedSelection.first();
    m_regionPageIndex = -1;
    m_regionSelection = {};

    if (m_pageSpinBox) {
        m_pageSpinBox->setRange(1, m_pageIds.size());
        m_pageCountLabel->setText(QStringLiteral("/ %1").arg(m_pageIds.size()));
    }
    setSelectionMode(m_selectionMode);
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
        if (m_selectionMode == SelectionMode::Objects && pageIndex == m_objectPageIndex) {
            QVector<QRect> bounds;
            bounds.reserve(m_pageObjects.size());
            for (const PdfPageObjectInfo &object : m_pageObjects)
                bounds.append(object.bounds);
            label->setObjectOverlay(bounds, m_selectedObject, m_hoveredObject, m_objectDragOffset,
                                    m_alignGuideXs, m_alignGuideYs);
        } else {
            label->setObjectOverlay({}, -1, -1, {}, {}, {});
        }
    }
    syncEditControls();
    syncTextFormatBar();
}

void PdfViewerWidget::refreshPageObjects()
{
    if (m_selectionMode != SelectionMode::Objects || !m_valid || !m_document
        || m_currentPageIndex < 0 || m_currentPageIndex >= m_pageLabels.size()) {
        m_pageObjects.clear();
        m_objectPageIndex = -1;
        updateSelectionOverlays();
        return;
    }

    const QVector<int> keepPath = m_selectedObject >= 0 && m_selectedObject < m_pageObjects.size()
                                     ? m_pageObjects[m_selectedObject].path
                                     : QVector<int>();
    QLabel *label = m_pageLabels[m_currentPageIndex];
    m_pageObjects = m_document->pageObjects(m_currentPageIndex, label->size());
    m_objectPageIndex = m_currentPageIndex;
    m_selectedObject = -1;
    for (int index = 0; index < m_pageObjects.size(); ++index) {
        if (m_pageObjects[index].path == keepPath) {
            m_selectedObject = index;
            break;
        }
    }
    updateSelectionOverlays();
}

int PdfViewerWidget::objectAt(const QLabel *label, const QPoint &position) const
{
    if (!label || label->property("pdfPageIndex").toInt() != m_objectPageIndex)
        return -1;
    // Text and images win over the lines of a table, which otherwise swallow the click.
    int pathHit = -1;
    for (int index = m_pageObjects.size() - 1; index >= 0; --index) {
        if (!m_pageObjects[index].bounds.contains(position))
            continue;
        const PdfPageObjectKind kind = m_pageObjects[index].kind;
        if (kind == PdfPageObjectKind::Text || kind == PdfPageObjectKind::Image)
            return index;
        if (pathHit < 0)
            pathHit = index;
    }
    return pathHit;
}

void PdfViewerWidget::commitObjectMove()
{
    if (m_selectedObject < 0 || m_selectedObject >= m_pageObjects.size()
        || m_objectDragOffset.isNull() || m_objectPageIndex < 0)
        return;

    const PdfPageObjectInfo object = m_pageObjects[m_selectedObject];
    QLabel *label = m_pageLabels[m_objectPageIndex];
    QVector<float> before;
    QVector<float> after;
    const bool moved = m_document->translatePageObject(
        m_objectPageIndex, object.path, label->size(), m_objectDragOffset, &before, &after);
    m_objectDragOffset = {};
    if (!moved) {
        updateSelectionOverlays();
        return;
    }

    EditHistoryEntry entry;
    entry.description = tr("Move object");
    entry.objectPageIndex = m_objectPageIndex;
    entry.objectPath = object.path;
    entry.beforeMatrix = before;
    entry.afterMatrix = after;
    recordHistoryEntry(std::move(entry));
    ++m_documentRevision;
    m_renderedWidths[m_objectPageIndex] = 0;
    refreshPageObjects();
    scheduleRender(m_objectPageIndex, label->width());
}

namespace {
QString readableFontName(QString name)
{
    const int plus = name.indexOf(QLatin1Char('+'));
    if (plus > 0 && plus <= 6)
        name = name.mid(plus + 1);
    return name;
}

QString familyForPdfFont(const QByteArray &fontData, const QString &fallback)
{
    const QString readable = readableFontName(fallback);
    if (fontData.isEmpty())
        return readable;
    static QHash<QByteArray, QString> loadedFamilies;
    const auto found = loadedFamilies.constFind(fontData);
    if (found != loadedFamilies.cend())
        return found.value();
    const int id = QFontDatabase::addApplicationFontFromData(fontData);
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    const QString family = families.isEmpty() ? readable : families.first();
    loadedFamilies.insert(fontData, family);
    return family;
}

QFont fontMatchingRun(const PdfPageObjectInfo &object)
{
    QFont font;
    const QString family = familyForPdfFont(object.fontData, object.fontFamily);
    if (!family.isEmpty())
        font.setFamily(family);
    // The embedded file is already the bold or italic cut. Adding weight on
    // top asks Qt for a different face and the run no longer matches the page.
    font.setWeight(QFont::Normal);
    font.setItalic(false);
    font.setStyleStrategy(QFont::NoFontMerging);
    font.setPixelSize(qMax(1, object.fontPixelSize > 0 ? object.fontPixelSize
                                                       : object.bounds.height()));
    const int advance = QFontMetrics(font).horizontalAdvance(object.text);
    if (advance > 4 && object.bounds.width() > 4) {
        const int fitted = qRound(font.pixelSize() * (double(object.bounds.width()) / advance));
        font.setPixelSize(qBound(1, fitted, 400));
    }
    return font;
}

QString encodeObjectPath(const QVector<int> &path)
{
    QStringList parts;
    for (const int index : path)
        parts.append(QString::number(index));
    return parts.join(QLatin1Char('/'));
}

QVector<int> decodeObjectPath(const QString &text)
{
    QVector<int> path;
    const QStringList parts = text.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        bool ok = false;
        const int index = part.toInt(&ok);
        if (ok)
            path.append(index);
    }
    return path;
}
}

namespace {
QString standardPdfFont(const QString &family, bool bold, bool italic)
{
    const QString name = family.toLower();
    QString face;
    if (name.contains(QLatin1String("courier")))
        face = QStringLiteral("Courier");
    else if (name.contains(QLatin1String("times")))
        face = QStringLiteral("Times");
    else if (name.contains(QLatin1String("helvetica")) || name.contains(QLatin1String("arial")))
        face = QStringLiteral("Helvetica");
    else
        return {};
    if (face == QLatin1String("Times")) {
        if (bold && italic)
            return QStringLiteral("Times-BoldItalic");
        if (bold)
            return QStringLiteral("Times-Bold");
        if (italic)
            return QStringLiteral("Times-Italic");
        return QStringLiteral("Times-Roman");
    }
    if (bold && italic)
        return face + QStringLiteral("-BoldOblique");
    if (bold)
        return face + QStringLiteral("-Bold");
    if (italic)
        return face + QStringLiteral("-Oblique");
    return face;
}

QByteArray fontFileMatching(const QString &family, bool bold, bool italic)
{
    QProcess match;
    match.start(QStringLiteral("fc-match"),
                {QStringLiteral("-f"), QStringLiteral("%{file}"),
                 QStringLiteral("%1:weight=%2:slant=%3")
                     .arg(family)
                     .arg(bold ? 700 : 400)
                     .arg(italic ? QStringLiteral("italic") : QStringLiteral("roman"))});
    if (!match.waitForFinished(1500) || match.exitCode() != 0)
        return {};
    const QString path = QString::fromUtf8(match.readAllStandardOutput()).trimmed();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
}

const PdfPageObjectInfo *PdfViewerWidget::selectedTextObject() const
{
    if (m_selectionMode != SelectionMode::Objects || m_selectedObject < 0
        || m_selectedObject >= m_pageObjects.size())
        return nullptr;
    const PdfPageObjectInfo &object = m_pageObjects[m_selectedObject];
    return object.kind == PdfPageObjectKind::Text ? &object : nullptr;
}

void PdfViewerWidget::rerenderEditedPage(int pageIndex)
{
    ++m_documentRevision;
    if (pageIndex >= 0 && pageIndex < m_renderedWidths.size())
        m_renderedWidths[pageIndex] = 0;
    refreshPageObjects();
    if (pageIndex >= 0 && pageIndex < m_pageLabels.size())
        scheduleRender(pageIndex, m_pageLabels[pageIndex]->width());
}

void PdfViewerWidget::syncTextFormatBar()
{
    if (!m_textFormatBar)
        return;
    const PdfPageObjectInfo *object = selectedTextObject();
    const bool show = m_editingPdf && object != nullptr;
    m_textFormatBar->setVisible(show);
    if (!show)
        return;
    placeTextFormatBar();

    m_updatingTextFormat = true;
    if (m_fontCombo->count() == 0)
        m_fontCombo->addItems(QFontDatabase::families());
    const QString family = readableFontName(object->fontFamily);
    int familyIndex = m_fontCombo->findText(family, Qt::MatchFixedString);
    if (familyIndex < 0 && !family.isEmpty()) {
        m_fontCombo->insertItem(0, family);
        familyIndex = 0;
    }
    if (familyIndex >= 0)
        m_fontCombo->setCurrentIndex(familyIndex);
    if (object->fontSizePoints > 0.f)
        m_fontSizeCombo->setCurrentText(QString::number(qRound(object->fontSizePoints)));
    m_boldButton->setChecked(object->fontWeight >= 600);
    m_italicButton->setChecked(object->italic);
    QPixmap swatch(16, 16);
    swatch.fill(object->color);
    m_textColorButton->setIcon(QIcon(swatch));
    const bool nested = object->path.size() > 1;
    m_fontCombo->setEnabled(!nested);
    m_boldButton->setEnabled(!nested);
    m_italicButton->setEnabled(!nested);
    if (nested) {
        const QString tip = tr("This text is inside a group. Size, color and alignment still apply.");
        m_fontCombo->setToolTip(tip);
        m_boldButton->setToolTip(tip);
        m_italicButton->setToolTip(tip);
    }
    m_updatingTextFormat = false;
}

QPoint PdfViewerWidget::snapDragOffset(const QPoint &raw)
{
    m_alignGuideXs.clear();
    m_alignGuideYs.clear();
    if (m_selectedObject < 0 || m_selectedObject >= m_pageObjects.size())
        return raw;

    const QRect base = m_pageObjects[m_selectedObject].bounds.translated(raw);
    constexpr int threshold = 6;
    struct Candidate {
        int delta = 0;
        int guide = 0;
    };
    QVector<Candidate> horizontal;
    QVector<Candidate> vertical;
    const auto collect = [&](const QRect &other) {
        const int movingX[3] = {base.left(), base.center().x(), base.right()};
        const int otherX[3] = {other.left(), other.center().x(), other.right()};
        for (const int moving : movingX) {
            for (const int edge : otherX)
                horizontal.append({edge - moving, edge});
        }
        const int movingY[3] = {base.top(), base.center().y(), base.bottom()};
        const int otherY[3] = {other.top(), other.center().y(), other.bottom()};
        for (const int moving : movingY) {
            for (const int edge : otherY)
                vertical.append({edge - moving, edge});
        }
    };
    for (int index = 0; index < m_pageObjects.size(); ++index) {
        if (index != m_selectedObject)
            collect(m_pageObjects[index].bounds);
    }
    if (m_objectPageIndex >= 0 && m_objectPageIndex < m_pageLabels.size())
        collect(m_pageLabels[m_objectPageIndex]->rect().adjusted(0, 0, -1, -1));

    const auto bestDelta = [&](const QVector<Candidate> &candidates) {
        int best = threshold + 1;
        for (const Candidate &candidate : candidates) {
            if (qAbs(candidate.delta) <= threshold && qAbs(candidate.delta) < qAbs(best))
                best = candidate.delta;
        }
        return best;
    };
    const int snapX = bestDelta(horizontal);
    const int snapY = bestDelta(vertical);
    if (qAbs(snapX) <= threshold) {
        for (const Candidate &candidate : horizontal) {
            if (candidate.delta == snapX && !m_alignGuideXs.contains(candidate.guide))
                m_alignGuideXs.append(candidate.guide);
        }
    }
    if (qAbs(snapY) <= threshold) {
        for (const Candidate &candidate : vertical) {
            if (candidate.delta == snapY && !m_alignGuideYs.contains(candidate.guide))
                m_alignGuideYs.append(candidate.guide);
        }
    }
    return raw + QPoint(qAbs(snapX) <= threshold ? snapX : 0,
                        qAbs(snapY) <= threshold ? snapY : 0);
}

void PdfViewerWidget::applyTextFontSize(int points)
{
    if (m_updatingTextFormat || points < 1 || points > 500)
        return;
    const PdfPageObjectInfo *object = selectedTextObject();
    if (!object || qRound(object->fontSizePoints) == points)
        return;
    const float before = object->fontSizePoints;
    const QVector<int> path = object->path;
    const int pageIndex = m_objectPageIndex;
    if (!m_document->setPageObjectFontSize(pageIndex, path, points))
        return;
    EditHistoryEntry entry;
    entry.description = tr("Text size");
    entry.objectPageIndex = pageIndex;
    entry.objectPath = path;
    entry.changesTextStyle = true;
    entry.beforeFontSize = before;
    entry.afterFontSize = points;
    recordHistoryEntry(std::move(entry));
    rerenderEditedPage(pageIndex);
}

void PdfViewerWidget::applyTextColor()
{
    const PdfPageObjectInfo *object = selectedTextObject();
    if (!object)
        return;
    const QColor chosen = QColorDialog::getColor(object->color, this, tr("Text color"));
    if (!chosen.isValid() || chosen == object->color)
        return;
    const QColor before = object->color;
    const QVector<int> path = object->path;
    const int pageIndex = m_objectPageIndex;
    if (!m_document->setPageObjectTextColor(pageIndex, path, chosen))
        return;
    EditHistoryEntry entry;
    entry.description = tr("Text color");
    entry.objectPageIndex = pageIndex;
    entry.objectPath = path;
    entry.changesTextColor = true;
    entry.beforeTextColor = before;
    entry.afterTextColor = chosen;
    recordHistoryEntry(std::move(entry));
    rerenderEditedPage(pageIndex);
}

void PdfViewerWidget::applyTextTypeface()
{
    if (m_updatingTextFormat)
        return;
    const PdfPageObjectInfo *object = selectedTextObject();
    if (!object || object->path.size() != 1)
        return;
    const QString family = m_fontCombo->currentText();
    const bool bold = m_boldButton->isChecked();
    const bool italic = m_italicButton->isChecked();
    if (family.compare(object->fontFamily, Qt::CaseInsensitive) == 0
        && bold == (object->fontWeight >= 600) && italic == object->italic)
        return;
    const QString standard = standardPdfFont(family, bold, italic);
    const QByteArray fontData = standard.isEmpty() ? fontFileMatching(family, bold, italic) : QByteArray();
    if (standard.isEmpty() && fontData.isEmpty())
        return;
    const QVector<int> path = object->path;
    const int pageIndex = m_objectPageIndex;
    const QByteArray beforeData = object->fontData;
    if (!m_document->replacePageObjectTypeface(pageIndex, path, fontData, standard))
        return;
    EditHistoryEntry entry;
    entry.description = tr("Text font");
    entry.objectPageIndex = pageIndex;
    entry.objectPath = path;
    entry.replacesTypeface = true;
    entry.beforeFontData = beforeData;
    entry.afterFontData = fontData;
    entry.afterStandardFont = standard;
    recordHistoryEntry(std::move(entry));
    rerenderEditedPage(pageIndex);
}

void PdfViewerWidget::applyTextAlignment(int alignment)
{
    const PdfPageObjectInfo *object = selectedTextObject();
    if (!object || m_objectPageIndex < 0)
        return;
    const QFont font = fontMatchingRun(*object);
    const int textWidth = QFontMetrics(font).horizontalAdvance(object->text);
    int targetLeft = object->bounds.left();
    if (alignment == 1)
        targetLeft = object->bounds.center().x() - textWidth / 2;
    else if (alignment == 2)
        targetLeft = object->bounds.right() - textWidth;
    const QPoint delta(targetLeft - object->baseline.x(), 0);
    if (delta.isNull())
        return;
    QVector<float> before;
    QVector<float> after;
    QLabel *label = m_pageLabels[m_objectPageIndex];
    if (!m_document->translatePageObject(m_objectPageIndex, object->path, label->size(), delta,
                                        &before, &after))
        return;
    EditHistoryEntry entry;
    entry.description = tr("Align text");
    entry.objectPageIndex = m_objectPageIndex;
    entry.objectPath = object->path;
    entry.beforeMatrix = before;
    entry.afterMatrix = after;
    recordHistoryEntry(std::move(entry));
    rerenderEditedPage(m_objectPageIndex);
}

void PdfViewerWidget::placeNewText(int pageIndex, const QPoint &labelPosition)
{
    if (pageIndex < 0 || pageIndex >= m_pageLabels.size())
        return;
    QLabel *label = m_pageLabels[pageIndex];
    const QPointF origin = m_document->pagePointAt(pageIndex, label->size(), labelPosition);
    const int index = m_document->insertPageText(pageIndex, origin, tr("Text"), 12.f);
    m_placingText = false;
    if (m_addTextButton) {
        const QSignalBlocker blocker(m_addTextButton);
        m_addTextButton->setChecked(false);
    }
    if (index < 0)
        return;
    m_currentPageIndex = pageIndex;
    m_objectPageIndex = pageIndex;
    rerenderEditedPage(pageIndex);
    for (int i = 0; i < m_pageObjects.size(); ++i) {
        if (m_pageObjects[i].path == QVector<int>{index}) {
            m_selectedObject = i;
            break;
        }
    }
    updateSelectionOverlays();
}

void PdfViewerWidget::editSelectedText()
{
    if (m_selectedObject < 0 || m_selectedObject >= m_pageObjects.size()
        || m_objectPageIndex < 0 || m_objectPageIndex >= m_pageLabels.size())
        return;
    const PdfPageObjectInfo object = m_pageObjects[m_selectedObject];
    if (object.kind != PdfPageObjectKind::Text)
        return;

    QLabel *label = m_pageLabels[m_objectPageIndex];
    if (!m_textEditor) {
        m_textEditor = new QLineEdit(label);
        connect(m_textEditor, &QLineEdit::editingFinished, this, &PdfViewerWidget::commitTextEdit);
        connect(m_textEditor, &QLineEdit::returnPressed, this, &PdfViewerWidget::commitTextEdit);
    } else if (m_textEditor->parentWidget() != label) {
        m_textEditor->setParent(label);
    }
    const QFont font = fontMatchingRun(object);
    m_textEditor->setFont(font);
    m_textEditor->setFrame(false);
    m_textEditor->setTextMargins(0, 0, 0, 0);
    m_textEditor->setAlignment(Qt::AlignLeft);
    const QString ink = object.color.name(QColor::HexArgb);
    m_textEditor->setStyleSheet(QStringLiteral(
        "QLineEdit { border: none; padding: 0px; margin: 0px; background: transparent; color: %1;"
        " selection-background-color: rgba(38, 132, 255, 45); selection-color: %1; }")
                                    .arg(ink));

    const QFontMetrics metrics(font);
    const bool haveBaseline = object.fontPixelSize > 0;
    const int top = haveBaseline ? object.baseline.y() - metrics.ascent() : object.bounds.top();
    const int left = haveBaseline ? object.baseline.x() : object.bounds.left();
    const int width = qMax(object.bounds.width(), metrics.horizontalAdvance(object.text) + metrics.averageCharWidth());
    m_textEditor->setGeometry(left, top, width, metrics.ascent() + metrics.descent());
    m_textEditor->setText(object.text);
    m_textEditor->setProperty("objectPage", m_objectPageIndex);
    m_textEditor->setProperty("objectPath", encodeObjectPath(object.path));
    m_textEditor->setProperty("originalText", object.text);
    m_textEditor->show();
    m_textEditor->setFocus();
    m_textEditor->selectAll();
}

void PdfViewerWidget::commitTextEdit()
{
    if (!m_textEditor || m_committingTextEdit || !m_textEditor->isVisible())
        return;
    m_committingTextEdit = true;
    const QString replacement = m_textEditor->text();
    const QString original = m_textEditor->property("originalText").toString();
    const int pageIndex = m_textEditor->property("objectPage").toInt();
    const QVector<int> objectPath = decodeObjectPath(m_textEditor->property("objectPath").toString());
    m_textEditor->hide();
    m_committingTextEdit = false;
    if (replacement.isEmpty() || replacement == original)
        return;
    if (!m_document->setPageObjectText(pageIndex, objectPath, replacement)) {
        QMessageBox::warning(this, tr("Edit Text"),
                             tr("This text could not be changed. The font in the PDF does not contain those characters."));
        return;
    }
    EditHistoryEntry entry;
    entry.description = tr("Edit text");
    entry.objectPageIndex = pageIndex;
    entry.objectPath = objectPath;
    entry.beforeText = original;
    entry.afterText = replacement;
    recordHistoryEntry(std::move(entry));
    ++m_documentRevision;
    if (pageIndex < m_renderedWidths.size())
        m_renderedWidths[pageIndex] = 0;
    refreshPageObjects();
    if (pageIndex >= 0 && pageIndex < m_pageLabels.size())
        scheduleRender(pageIndex, m_pageLabels[pageIndex]->width());
}

void PdfViewerWidget::editSelectedImage()
{
    if (m_selectedObject < 0 || m_selectedObject >= m_pageObjects.size())
        return;
    const PdfPageObjectInfo object = m_pageObjects[m_selectedObject];
    if (object.kind != PdfPageObjectKind::Image || m_objectPageIndex < 0)
        return;

    const QByteArray png = m_document->pageObjectImagePng(m_objectPageIndex, object.path);
    if (png.isEmpty()) {
        QMessageBox::warning(this, tr("Edit Image"),
                             tr("This image could not be exported."));
        return;
    }

    const QString directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_imageEditPath = QDir(directory).filePath(
        QStringLiteral("cualpdf-image-%1.png").arg(encodeObjectPath(object.path).replace('/', '-')));
    QFile file(m_imageEditPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(png) != png.size()) {
        QMessageBox::warning(this, tr("Edit Image"),
                             tr("This image could not be exported."));
        return;
    }
    file.close();

    m_imageEditPage = m_objectPageIndex;
    m_imageEditObjectPath = object.path;
    m_imageEditApplied = png;

    if (!m_imageWatcher) {
        m_imageWatcher = new QFileSystemWatcher(this);
        connect(m_imageWatcher, &QFileSystemWatcher::fileChanged,
                this, [this](const QString &) {
                    if (m_imageReloadTimer)
                        m_imageReloadTimer->start();
                    if (!m_imageEditPath.isEmpty() && m_imageWatcher
                        && !m_imageWatcher->files().contains(m_imageEditPath))
                        m_imageWatcher->addPath(m_imageEditPath);
                });
    }
    if (!m_imageReloadTimer) {
        m_imageReloadTimer = new QTimer(this);
        m_imageReloadTimer->setSingleShot(true);
        m_imageReloadTimer->setInterval(400);
        connect(m_imageReloadTimer, &QTimer::timeout, this, &PdfViewerWidget::reloadEditedImage);
    }
    if (!m_imageWatcher->files().contains(m_imageEditPath))
        m_imageWatcher->addPath(m_imageEditPath);

    const QString editor = AppSettings().imageEditorPath();
    bool started = false;
    if (editor.isEmpty())
        started = QDesktopServices::openUrl(QUrl::fromLocalFile(m_imageEditPath));
    else
        started = QProcess::startDetached(editor, {m_imageEditPath});
    if (!started) {
        QMessageBox::warning(this, tr("Edit Image"),
                             tr("The image editor could not be opened."));
    }
}

void PdfViewerWidget::reloadEditedImage()
{
    if (m_editingWholePage) {
        QImage image(m_imageEditPath);
        if (image.isNull() || m_imageEditPage < 0)
            return;
        m_editingWholePage = false;
        m_selectedPages = {m_imageEditPage};
        replaceSelectedPagesWithRaster(tr("Edit page externally"), [image](const QImage &) {
            return image;
        });
        return;
    }
    if (m_imageEditPath.isEmpty() || m_imageEditPage < 0 || m_imageEditObjectPath.isEmpty())
        return;
    QFile file(m_imageEditPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QByteArray png = file.readAll();
    if (png.isEmpty() || png == m_imageEditApplied)
        return;
    if (!m_document->setPageObjectImagePng(m_imageEditPage, m_imageEditObjectPath, png))
        return;

    EditHistoryEntry entry;
    entry.description = tr("Edit image");
    entry.objectPageIndex = m_imageEditPage;
    entry.objectPath = m_imageEditObjectPath;
    entry.beforeImagePng = m_imageEditApplied;
    entry.afterImagePng = png;
    m_imageEditApplied = png;
    recordHistoryEntry(std::move(entry));
    ++m_documentRevision;
    if (m_imageEditPage < m_renderedWidths.size())
        m_renderedWidths[m_imageEditPage] = 0;
    refreshPageObjects();
    if (m_imageEditPage < m_pageLabels.size())
        scheduleRender(m_imageEditPage, m_pageLabels[m_imageEditPage]->width());
}

void PdfViewerWidget::finishObjectHistory(bool success, int pageIndex,
                                          int targetHistoryPosition)
{
    m_transformInProgress = false;
    emit operationInProgressChanged(false);
    if (success) {
        m_historyPosition = targetHistoryPosition;
        if (pageIndex >= 0 && pageIndex < m_renderedWidths.size())
            m_renderedWidths[pageIndex] = 0;
        updateModifiedState();
        refreshPageObjects();
        if (pageIndex >= 0 && pageIndex < m_pageLabels.size())
            scheduleRender(pageIndex, m_pageLabels[pageIndex]->width());
    } else {
        qWarning() << "Failed to restore a page object:" << m_filePath;
        syncEditControls();
    }
    emit historyChanged();
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
    if (m_deletePagesAction)
        m_deletePagesAction->setEnabled(canEditPages
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

void PdfViewerWidget::replaceSelectedPagesWithRaster(
    const QString &description,
    const std::function<QImage(const QImage &)> &transform)
{
    if (!m_valid || m_transformInProgress || m_saveInProgress || m_selectedPages.isEmpty())
        return;
    QVector<int> indexes(m_selectedPages.cbegin(), m_selectedPages.cend());
    std::sort(indexes.begin(), indexes.end());
    QVector<QSizeF> sizes;
    sizes.reserve(indexes.size());
    for (const int index : indexes)
        sizes.append(pageSize(index));

    m_transformInProgress = true;
    emit operationInProgressChanged(true);
    syncEditControls();
    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    const QVector<quint64> beforePageIds = m_pageIds;
    QVector<quint64> newPageIds;
    QVector<quint64> afterPageIds = m_pageIds;
    for (const int index : indexes) {
        const quint64 newPageId = m_nextPageId++;
        newPageIds.append(newPageId);
        afterPageIds[index] = newPageId;
    }
    QThread *thread = QThread::create(
        [weakSelf, document, indexes, sizes, transform, description, beforePageIds,
         afterPageIds, newPageIds]() {
            QVector<QImage> images;
            images.reserve(indexes.size());
            bool ok = true;
            for (int i = 0; i < indexes.size(); ++i) {
                const int width = qMax(1, qRound(sizes.at(i).width() / 72.0 * 200.0));
                const QImage rendered = document->renderPage(indexes.at(i), width);
                const QImage result = transform(rendered);
                if (result.isNull()) {
                    ok = false;
                    break;
                }
                images.append(result);
            }
            const QByteArray archive = ok
                                           ? PdfDocument::createImagePagesArchive(images, sizes)
                                           : QByteArray();
            QMetaObject::invokeMethod(
                qApp,
                [weakSelf, archive, description, beforePageIds, afterPageIds, newPageIds]() {
                    if (!weakSelf)
                        return;
                    weakSelf->m_transformInProgress = false;
                    emit weakSelf->operationInProgressChanged(false);
                    weakSelf->syncEditControls();
                    if (archive.isEmpty()) {
                        QMessageBox::warning(weakSelf, description,
                                             viewerText("The selected pages could not be changed."));
                        return;
                    }
                    weakSelf->applyPageStructureChange(description, beforePageIds, afterPageIds,
                                                       newPageIds, archive, newPageIds);
                },
                Qt::QueuedConnection);
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
}

class CornerPage : public QWidget
{
public:
    explicit CornerPage(const QImage &image, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_image(image)
    {
        setMinimumSize(520, 640);
        m_corners = {QPointF(0.08, 0.08), QPointF(0.92, 0.08), QPointF(0.92, 0.92), QPointF(0.08, 0.92)};
    }

    QVector<QPointF> cornersInImage() const
    {
        QVector<QPointF> points;
        for (const QPointF &corner : m_corners)
            points.append(QPointF(corner.x() * m_image.width(), corner.y() * m_image.height()));
        return points;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(32, 32, 32));
        const QRect fitted = imageRect();
        painter.drawImage(fitted, m_image);
        QPolygonF polygon;
        for (const QPointF &corner : m_corners)
            polygon.append(mapPoint(corner, fitted));
        painter.setPen(QPen(QColor(38, 132, 255), 2));
        painter.drawPolygon(polygon);
        painter.setBrush(Qt::white);
        for (const QPointF &point : polygon)
            painter.drawEllipse(point, 7, 7);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const QRect fitted = imageRect();
        m_drag = -1;
        for (int index = 0; index < m_corners.size(); ++index) {
            if (QLineF(event->position(), mapPoint(m_corners[index], fitted)).length() < 16)
                m_drag = index;
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_drag < 0)
            return;
        const QRect fitted = imageRect();
        if (fitted.width() < 2 || fitted.height() < 2)
            return;
        m_corners[m_drag] = QPointF(
            std::clamp((event->position().x() - fitted.left()) / fitted.width(), 0.0, 1.0),
            std::clamp((event->position().y() - fitted.top()) / fitted.height(), 0.0, 1.0));
        update();
    }

private:
    QRect imageRect() const
    {
        const QSize fitted = m_image.size().scaled(size(), Qt::KeepAspectRatio);
        return QRect(QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
    }

    static QPointF mapPoint(const QPointF &normalized, const QRect &fitted)
    {
        return QPointF(fitted.left() + normalized.x() * fitted.width(),
                       fitted.top() + normalized.y() * fitted.height());
    }

    QImage m_image;
    QVector<QPointF> m_corners;
    int m_drag = -1;
};

void PdfViewerWidget::improveSelectedScans()
{
    if (m_selectedPages.isEmpty())
        return;
    const int previewIndex = *std::min_element(m_selectedPages.cbegin(), m_selectedPages.cend());
    const QImage preview = m_document->renderPage(previewIndex, 700);
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Improve Scan"));
    auto *level = new QSlider(Qt::Horizontal, &dialog);
    level->setRange(0, 6);
    level->setValue(3);
    auto *whiteBackground = new QCheckBox(tr("White background"), &dialog);
    whiteBackground->setChecked(true);
    auto *blackText = new QCheckBox(tr("Black text"), &dialog);
    blackText->setChecked(true);
    auto *blackAndWhite = new QCheckBox(tr("Black and white"), &dialog);
    auto *previewLabel = new QLabel(&dialog);
    previewLabel->setMinimumSize(360, 460);
    previewLabel->setAlignment(Qt::AlignCenter);
    const auto refresh = [&]() {
        const QImage result = ImageEnhancement::mejorarEscaneo(
            preview, level->value(), whiteBackground->isChecked(), blackText->isChecked(),
            blackAndWhite->isChecked());
        previewLabel->setPixmap(QPixmap::fromImage(result).scaled(360, 460, Qt::KeepAspectRatio,
                                                                  Qt::SmoothTransformation));
    };
    refresh();
    connect(level, &QSlider::valueChanged, &dialog, refresh);
    connect(whiteBackground, &QCheckBox::toggled, &dialog, refresh);
    connect(blackText, &QCheckBox::toggled, &dialog, refresh);
    connect(blackAndWhite, &QCheckBox::toggled, &dialog, refresh);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(previewLabel);
    layout->addWidget(level);
    layout->addWidget(whiteBackground);
    layout->addWidget(blackText);
    layout->addWidget(blackAndWhite);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const int chosenLevel = level->value();
    const bool white = whiteBackground->isChecked();
    const bool black = blackText->isChecked();
    const bool mono = blackAndWhite->isChecked();
    replaceSelectedPagesWithRaster(tr("Improve scan"), [=](const QImage &page) {
        return ImageEnhancement::mejorarEscaneo(page, chosenLevel, white, black, mono);
    });
}

void PdfViewerWidget::correctSelectedPerspective()
{
    if (m_selectedPages.size() != 1 || !m_document)
        return;
    const int index = *m_selectedPages.cbegin();
    const QImage preview = m_document->renderPage(index, 1000);
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Correct Perspective"));
    auto *page = new CornerPage(preview, &dialog);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(page, 1);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(640, 820);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QVector<QPointF> corners = page->cornersInImage();
    m_selectedPages = {index};
    replaceSelectedPagesWithRaster(tr("Correct perspective"), [corners](const QImage &image) {
        const double scale = image.width() / 1000.0;
        QVector<QPointF> scaled;
        for (const QPointF &corner : corners)
            scaled.append(corner * scale);
        return ImageEnhancement::corregirPerspectiva(image, scaled);
    });
}

void PdfViewerWidget::compressDocument()
{
    if (!m_valid || m_transformInProgress || m_saveInProgress || !m_document)
        return;
    m_transformInProgress = true;
    emit operationInProgressChanged(true);
    syncEditControls();
    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    QThread *thread = QThread::create([weakSelf, document]() {
        const int changed = document->compressImages();
        QMetaObject::invokeMethod(
            qApp,
            [weakSelf, changed]() {
                if (!weakSelf)
                    return;
                weakSelf->m_transformInProgress = false;
                emit weakSelf->operationInProgressChanged(false);
                if (changed > 0) {
                    weakSelf->m_modified = true;
                    emit weakSelf->modifiedChanged(true);
                    ++weakSelf->m_documentRevision;
                    weakSelf->m_renderedWidths.fill(0);
                    weakSelf->applyZoom();
                }
                weakSelf->syncEditControls();
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
}

void PdfViewerWidget::editSelectedPageExternally()
{
    if (m_selectedPages.size() != 1 || !m_document)
        return;
    const int index = *m_selectedPages.cbegin();
    const QSizeF size = pageSize(index);
    const QImage image = m_document->renderPage(index, qMax(1, qRound(size.width() / 72.0 * 150.0)));
    if (image.isNull())
        return;
    const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(QStringLiteral("cualpdf-page.png"));
    if (!image.save(path, "PNG"))
        return;
    m_editingWholePage = true;
    m_imageEditPath = path;
    m_imageEditPage = index;
    m_imageEditObjectPath.clear();
    m_imageEditApplied = QByteArray();
    if (!m_imageWatcher) {
        m_imageWatcher = new QFileSystemWatcher(this);
        connect(m_imageWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &) {
            if (m_imageReloadTimer)
                m_imageReloadTimer->start();
            if (!m_imageEditPath.isEmpty() && m_imageWatcher
                && !m_imageWatcher->files().contains(m_imageEditPath))
                m_imageWatcher->addPath(m_imageEditPath);
        });
    }
    if (!m_imageReloadTimer) {
        m_imageReloadTimer = new QTimer(this);
        m_imageReloadTimer->setSingleShot(true);
        m_imageReloadTimer->setInterval(400);
        connect(m_imageReloadTimer, &QTimer::timeout, this, &PdfViewerWidget::reloadEditedImage);
    }
    if (!m_imageWatcher->files().contains(path))
        m_imageWatcher->addPath(path);
    const QString editor = AppSettings().imageEditorPath();
    if (editor.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    else
        QProcess::startDetached(editor, {path});
}

void PdfViewerWidget::invertSelectedPages()
{
    replaceSelectedPagesWithRaster(tr("Invert colors"), [](const QImage &page) {
        return ImageEnhancement::invertirColores(page);
    });
}

void PdfViewerWidget::flattenSelectedPages()
{
    replaceSelectedPagesWithRaster(tr("Flatten pages"), [](const QImage &page) {
        return page;
    });
}

void PdfViewerWidget::autoCropSelectedPages()
{
    if (!m_valid || m_transformInProgress || m_saveInProgress || m_selectedPages.isEmpty())
        return;
    QVector<int> indexes(m_selectedPages.cbegin(), m_selectedPages.cend());
    std::sort(indexes.begin(), indexes.end());
    m_transformInProgress = true;
    emit operationInProgressChanged(true);
    ++m_documentRevision;
    m_pagesLoading.clear();
    syncEditControls();
    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    QThread *thread = QThread::create([weakSelf, document, indexes]() {
        const QVector<PdfPageState> beforeStates = document->pageStates(indexes);
        bool changed = false;
        for (const int index : indexes) {
            const QSizeF size = document->pageSizePoints(index);
            const int width = qMax(1, qRound(size.width() / 72.0 * 72.0));
            const QImage image = document->renderPage(index, width);
            const QMargins pixels = ImageEnhancement::blackBorderMargins(image);
            if (pixels.isNull() || image.width() <= 0 || image.height() <= 0)
                continue;
            const QMarginsF points(pixels.left() * size.width() / image.width(),
                                   pixels.top() * size.height() / image.height(),
                                   pixels.right() * size.width() / image.width(),
                                   pixels.bottom() * size.height() / image.height());
            changed = document->cropPages({index}, points) || changed;
        }
        const QVector<PdfPageState> afterStates = changed ? document->pageStates(indexes)
                                                          : QVector<PdfPageState>();
        QMetaObject::invokeMethod(
            qApp,
            [weakSelf, changed, indexes, beforeStates, afterStates]() {
                if (!weakSelf)
                    return;
                weakSelf->finishDocumentTransform(
                    changed && afterStates.size() == indexes.size(),
                    changed ? weakSelf->m_document->allPageSizes() : QVector<QSizeF>(),
                    indexes, false, viewerText("Auto crop"), beforeStates, afterStates);
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
}

void PdfViewerWidget::splitSelectedPages()
{
    if (!m_valid || m_transformInProgress || m_saveInProgress || m_selectedPages.isEmpty())
        return;
    QVector<int> indexes(m_selectedPages.cbegin(), m_selectedPages.cend());
    std::sort(indexes.begin(), indexes.end());
    QVector<QSizeF> sizes;
    for (const int index : indexes)
        sizes.append(pageSize(index));
    m_transformInProgress = true;
    emit operationInProgressChanged(true);
    syncEditControls();
    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    const QVector<quint64> beforePageIds = m_pageIds;
    QVector<quint64> afterPageIds = m_pageIds;
    QVector<quint64> newPageIds;
    QVector<QSizeF> halfSizes;
    int shift = 0;
    for (int i = 0; i < indexes.size(); ++i) {
        const int at = indexes.at(i) + shift;
        afterPageIds.removeAt(at);
        const quint64 leftId = m_nextPageId++;
        const quint64 rightId = m_nextPageId++;
        afterPageIds.insert(at, leftId);
        afterPageIds.insert(at + 1, rightId);
        newPageIds.append(leftId);
        newPageIds.append(rightId);
        const QSizeF half(sizes.at(i).width() / 2.0, sizes.at(i).height());
        halfSizes.append(half);
        halfSizes.append(half);
        ++shift;
    }
    QThread *thread = QThread::create(
        [weakSelf, document, indexes, sizes, halfSizes, beforePageIds, afterPageIds, newPageIds]() {
            QVector<QImage> halves;
            bool ok = true;
            for (int i = 0; i < indexes.size(); ++i) {
                const int width = qMax(2, qRound(sizes.at(i).width() / 72.0 * 200.0));
                const QImage rendered = document->renderPage(indexes.at(i), width);
                if (rendered.isNull() || rendered.width() < 2) {
                    ok = false;
                    break;
                }
                const int mid = rendered.width() / 2;
                halves.append(rendered.copy(0, 0, mid, rendered.height()));
                halves.append(rendered.copy(mid, 0, rendered.width() - mid, rendered.height()));
            }
            const QByteArray archive = ok ? PdfDocument::createImagePagesArchive(halves, halfSizes)
                                          : QByteArray();
            QMetaObject::invokeMethod(
                qApp,
                [weakSelf, archive, beforePageIds, afterPageIds, newPageIds]() {
                    if (!weakSelf)
                        return;
                    weakSelf->m_transformInProgress = false;
                    emit weakSelf->operationInProgressChanged(false);
                    weakSelf->syncEditControls();
                    if (archive.isEmpty()) {
                        QMessageBox::warning(weakSelf, viewerText("Split Page"),
                                             viewerText("The selected pages could not be split."));
                        return;
                    }
                    weakSelf->applyPageStructureChange(
                        viewerText("Split pages"), beforePageIds, afterPageIds, newPageIds, archive,
                        newPageIds);
                },
                Qt::QueuedConnection);
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void PdfViewerWidget::clearSelectedPageView()
{
    if (!m_valid || m_transformInProgress || m_saveInProgress || m_selectedPages.isEmpty())
        return;
    QVector<int> indexes(m_selectedPages.cbegin(), m_selectedPages.cend());
    std::sort(indexes.begin(), indexes.end());
    m_transformInProgress = true;
    emit operationInProgressChanged(true);
    ++m_documentRevision;
    m_pagesLoading.clear();
    syncEditControls();
    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    QThread *thread = QThread::create([weakSelf, document, indexes]() {
        const QVector<PdfPageState> beforeStates = document->pageStates(indexes);
        const bool transformed = beforeStates.size() == indexes.size()
                                 && document->resetPageView(indexes);
        const QVector<PdfPageState> afterStates = transformed ? document->pageStates(indexes)
                                                              : QVector<PdfPageState>();
        const QVector<QSizeF> sizes = transformed ? document->allPageSizes() : QVector<QSizeF>();
        QMetaObject::invokeMethod(
            qApp,
            [weakSelf, transformed, indexes, beforeStates, afterStates, sizes]() {
                if (!weakSelf)
                    return;
                weakSelf->finishDocumentTransform(
                    transformed && afterStates.size() == indexes.size(), sizes, indexes, false,
                    viewerText("Clear rotation and crop"), beforeStates, afterStates);
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
}

void PdfViewerWidget::lightenSelectedPage(int overridePageIndex)
{
    if (!m_valid || m_transformInProgress || m_saveInProgress)
        return;

    // overridePageIndex carries the page under an active region selection
    // (Region mode); otherwise fall back to the whole-page selection
    // (Page mode).
    int pageIndex = overridePageIndex;
    if (pageIndex < 0) {
        if (m_selectedPages.isEmpty())
            return;
        pageIndex = *std::min_element(m_selectedPages.cbegin(), m_selectedPages.cend());
    }
    if (pageIndex < 0 || pageIndex >= m_pageLabels.size())
        return;

    const QImage previewSource = m_pageLabels[pageIndex]->pixmap().toImage();
    LightenPageDialog dialog(previewSource, /*initialAmount=*/50, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const int amount = dialog.amount();
    if (amount <= 0)
        return;

    const QSizeF pageSizePoints = pageSize(pageIndex);

    // Phase 1 (this thread): render the page at high resolution and run the
    // enhancement — no document mutation yet, so m_transformInProgress is
    // toggled back off before phase 2 hands off to
    // applyPageStructureChange(), which manages its own busy state exactly
    // like insertPdfAt() does for its two phases.
    m_transformInProgress = true;
    emit operationInProgressChanged(true);
    syncEditControls();

    const std::shared_ptr<PdfDocument> document = m_document;
    const QPointer<PdfViewerWidget> weakSelf(this);
    const QVector<quint64> beforePageIds = m_pageIds;
    const quint64 newPageId = m_nextPageId++;
    QVector<quint64> afterPageIds = m_pageIds;
    afterPageIds[pageIndex] = newPageId;
    const QVector<quint64> archivedPageIds{newPageId};
    const QVector<quint64> selectedPageIds{newPageId};

    QThread *thread = QThread::create(
        [weakSelf, document, pageIndex, amount, pageSizePoints, beforePageIds,
         afterPageIds, archivedPageIds, selectedPageIds]() {
            constexpr double kEnhanceDpi = 300.0;
            const int renderWidthPx = qMax(
                1, qRound(pageSizePoints.width() / 72.0 * kEnhanceDpi));
            const QImage rendered = document->renderPage(pageIndex, renderWidthPx);
            const QImage enhanced = ImageEnhancement::aclararPapel(rendered, amount);
            const QByteArray archive =
                PdfDocument::createImagePageArchive(enhanced, pageSizePoints);
            QMetaObject::invokeMethod(
                qApp,
                [weakSelf, archive, beforePageIds, afterPageIds, archivedPageIds,
                 selectedPageIds]() {
                    if (!weakSelf)
                        return;
                    weakSelf->m_transformInProgress = false;
                    emit weakSelf->operationInProgressChanged(false);
                    weakSelf->syncEditControls();
                    if (archive.isEmpty()) {
                        QMessageBox::warning(
                            weakSelf, viewerText("Lighten Page"),
                            viewerText("The page could not be enhanced."));
                        return;
                    }
                    weakSelf->applyPageStructureChange(
                        viewerText("Lighten page"), beforePageIds, afterPageIds,
                        archivedPageIds, archive, selectedPageIds);
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
    if (entry.changesPageObject()) {
        const int pageIndex = entry.objectPageIndex;
        const QVector<int> objectPath = entry.objectPath;
        if (entry.changesTextStyle || entry.changesTextColor || entry.replacesTypeface) {
            const float fontSize = redoOperation ? entry.afterFontSize : entry.beforeFontSize;
            const QColor color = redoOperation ? entry.afterTextColor : entry.beforeTextColor;
            const QByteArray fontData = redoOperation ? entry.afterFontData : entry.beforeFontData;
            const QString standardFont = redoOperation ? entry.afterStandardFont : entry.beforeStandardFont;
            const bool style = entry.changesTextStyle;
            const bool recolor = entry.changesTextColor;
            const bool typeface = entry.replacesTypeface;
            m_transformInProgress = true;
            emit operationInProgressChanged(true);
            ++m_documentRevision;
            m_pagesLoading.clear();
            syncEditControls();
            const std::shared_ptr<PdfDocument> document = m_document;
            const QPointer<PdfViewerWidget> weakSelf(this);
            QThread *thread = QThread::create(
                [weakSelf, document, pageIndex, objectPath, fontSize, color, fontData, standardFont,
                 style, recolor, typeface, targetHistoryPosition]() {
                    bool restored = true;
                    if (style)
                        restored = document->setPageObjectFontSize(pageIndex, objectPath, fontSize);
                    if (recolor)
                        restored = document->setPageObjectTextColor(pageIndex, objectPath, color) && restored;
                    if (typeface)
                        restored = document->replacePageObjectTypeface(pageIndex, objectPath, fontData,
                                                                      standardFont)
                                   && restored;
                    QMetaObject::invokeMethod(
                        qApp,
                        [weakSelf, restored, pageIndex, targetHistoryPosition]() {
                            if (weakSelf)
                                weakSelf->finishObjectHistory(restored, pageIndex, targetHistoryPosition);
                        },
                        Qt::QueuedConnection);
                });
            connect(thread, &QThread::finished, thread, &QThread::deleteLater);
            thread->start();
            return;
        }
        const QVector<float> matrix = redoOperation ? entry.afterMatrix : entry.beforeMatrix;
        const QByteArray image = redoOperation ? entry.afterImagePng : entry.beforeImagePng;
        const QString text = redoOperation ? entry.afterText : entry.beforeText;
        const bool imageEdit = !entry.beforeImagePng.isEmpty() || !entry.afterImagePng.isEmpty();
        const bool textEdit = !entry.beforeText.isNull() || !entry.afterText.isNull();

        m_transformInProgress = true;
        emit operationInProgressChanged(true);
        ++m_documentRevision;
        m_pagesLoading.clear();
        syncEditControls();

        const std::shared_ptr<PdfDocument> document = m_document;
        const QPointer<PdfViewerWidget> weakSelf(this);
        QThread *thread = QThread::create(
            [weakSelf, document, pageIndex, objectPath, matrix, image, imageEdit, text, textEdit,
             targetHistoryPosition]() {
                const bool restored = textEdit
                                          ? document->setPageObjectText(pageIndex, objectPath, text)
                                          : (imageEdit
                                                 ? document->setPageObjectImagePng(pageIndex, objectPath, image)
                                                 : document->setPageObjectMatrix(pageIndex, objectPath, matrix));
                QMetaObject::invokeMethod(
                    qApp,
                    [weakSelf, restored, pageIndex, targetHistoryPosition]() {
                        if (weakSelf)
                            weakSelf->finishObjectHistory(restored, pageIndex, targetHistoryPosition);
                    },
                    Qt::QueuedConnection);
            });
        connect(thread, &QThread::finished, thread, &QThread::deleteLater);
        thread->start();
        return;
    }

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
            && (m_selectionMode == SelectionMode::Page
                || m_selectionMode == SelectionMode::Region
                || m_selectionMode == SelectionMode::Objects)) {
            auto *contextEvent = static_cast<QContextMenuEvent *>(event);
            if (m_selectionMode == SelectionMode::Objects && pageIndex == m_objectPageIndex) {
                m_selectedObject = objectAt(label, contextEvent->pos());
                m_objectDragOffset = {};
                updateSelectionOverlays();
            }
            showPageContextMenu(pageIndex, contextEvent->globalPos());
            return true;
        }

        if (event->type() == QEvent::MouseButtonDblClick
            && m_selectionMode == SelectionMode::Objects && pageIndex == m_objectPageIndex) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            m_selectedObject = objectAt(label, mouseEvent->position().toPoint());
            updateSelectionOverlays();
            if (m_selectedObject >= 0
                && m_pageObjects[m_selectedObject].kind == PdfPageObjectKind::Image)
                editSelectedImage();
            else if (m_selectedObject >= 0
                     && m_pageObjects[m_selectedObject].kind == PdfPageObjectKind::Text)
                editSelectedText();
            return true;
        }

        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() != Qt::LeftButton)
                return false;

            setCurrentPageFromPointer(pageIndex);
            if (m_placingText && m_editingPdf) {
                placeNewText(pageIndex, mouseEvent->position().toPoint());
                return true;
            }
            if (m_selectionMode == SelectionMode::Read)
                return true;
            if (m_selectionMode == SelectionMode::Objects) {
                if (pageIndex != m_objectPageIndex)
                    refreshPageObjects();
                m_selectedObject = objectAt(label, mouseEvent->position().toPoint());
                m_objectPressPos = mouseEvent->position().toPoint();
                m_objectDragOffset = {};
                m_draggingObject = m_selectedObject >= 0;
                updateSelectionOverlays();
                return true;
            }
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

        if (event->type() == QEvent::MouseMove && m_selectionMode == SelectionMode::Objects
            && !m_draggingObject && pageIndex == m_objectPageIndex) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const int hovered = objectAt(label, mouseEvent->position().toPoint());
            if (hovered != m_hoveredObject) {
                m_hoveredObject = hovered;
                label->setCursor(hovered >= 0 ? Qt::SizeAllCursor : Qt::ArrowCursor);
                updateSelectionOverlays();
            }
        }

        if (event->type() == QEvent::MouseMove && m_selectionMode == SelectionMode::Objects
            && m_draggingObject && pageIndex == m_objectPageIndex) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->buttons().testFlag(Qt::LeftButton)) {
                m_objectDragOffset = snapDragOffset(mouseEvent->position().toPoint() - m_objectPressPos);
                updateSelectionOverlays();
                return true;
            }
        }

        if (event->type() == QEvent::MouseButtonRelease && m_selectionMode == SelectionMode::Objects
            && m_draggingObject && pageIndex == m_objectPageIndex) {
            m_draggingObject = false;
            m_alignGuideXs.clear();
            m_alignGuideYs.clear();
            commitObjectMove();
            return true;
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

    if (m_organizePagesEnabled && m_selectionMode == SelectionMode::Page
        && (watched == m_pagesContainer || watched == m_scrollArea->viewport())) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                const QPoint origin = m_pagesContainer->mapFrom(static_cast<QWidget *>(watched),
                                                               mouseEvent->position().toPoint());
                beginPageMarquee(origin, mouseEvent->modifiers());
                return true;
            }
        }
        if (event->type() == QEvent::MouseMove && m_marqueeSelecting) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QPoint position = m_pagesContainer->mapFrom(static_cast<QWidget *>(watched),
                                                             mouseEvent->position().toPoint());
            updatePageMarquee(position);
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && m_marqueeSelecting) {
            finishPageMarquee();
            return true;
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

void PdfViewerWidget::placeTextFormatBar()
{
    if (!m_textFormatBar || !m_textFormatBar->isVisible())
        return;
    m_textFormatBar->adjustSize();
    const QSize hint = m_textFormatBar->sizeHint();
    const int barWidth = qMin(hint.width(), qMax(0, width() - 16));
    const int barHeight = hint.height();
    int top = 8;
    const QList<QToolBar *> bars = findChildren<QToolBar *>(Qt::FindDirectChildrenOnly);
    for (const QToolBar *bar : bars) {
        if (bar->isVisible())
            top = qMax(top, bar->geometry().bottom() + 8);
    }
    m_textFormatBar->setGeometry((width() - barWidth) / 2, top, barWidth, barHeight);
    m_textFormatBar->raise();
}

void PdfViewerWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    placeTextFormatBar();
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
    if (m_selectionMode == SelectionMode::Objects)
        refreshPageObjects();
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

        const bool neededNow = m_pageLayout == PageLayout::Discrete
                                   ? pageInDiscreteChunk(i)
                                   : labelRect.intersects(expanded);
        if (m_renderedWidths[i] != label->width() && neededNow)
            scheduleRender(i, label->width());
    }

    if (m_pageLayout == PageLayout::Continuous && visiblePage != m_currentPageIndex) {
        m_currentPageIndex = visiblePage;
        syncNavigationControls();
        emit currentPageChanged(m_currentPageIndex);
    }

    if (m_pageLayout == PageLayout::Discrete && discreteChunkReady()) {
        const int count = discretePageCount();
        const int firstPage = discreteChunkStart();
        for (const int start : {firstPage - count, firstPage + count}) {
            if (start < 0 || start >= m_pageLabels.size())
                continue;
            const int end = qMin(start + count, m_pageLabels.size());
            for (int pageIndex = start; pageIndex < end; ++pageIndex) {
                if (m_renderedWidths[pageIndex] != m_pageLabels[pageIndex]->width())
                    scheduleRender(pageIndex, m_pageLabels[pageIndex]->width());
            }
        }
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

    // A render that finishes for the page still on screen, after the user
    // already moved on, must not replace that pixmap.
    if (m_pageLayout == PageLayout::Discrete && !label->isHidden()
        && !pageInDiscreteChunk(pageIndex)) {
        return;
    }

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
    revealDiscreteChunk();
}
