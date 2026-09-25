#include "printdialog.h"

#include "pdfdocument.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QSlider>
#include <QPainter>
#include <QMessageBox>
#include <QPrintDialog>
#include <QPrinter>
#include <QPrinterInfo>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
constexpr double kPointsPerMm = 72.0 / 25.4;

QVector<int> parseRange(const QString &text, int pageCount)
{
    QVector<int> pages;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        pages.resize(pageCount);
        for (int i = 0; i < pageCount; ++i)
            pages[i] = i;
        return pages;
    }
    const QStringList parts = trimmed.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString piece = part.trimmed();
        const int dash = piece.indexOf(QLatin1Char('-'));
        bool okA = false;
        bool okB = false;
        if (dash < 0) {
            const int page = piece.toInt(&okA);
            if (okA && page >= 1 && page <= pageCount)
                pages.append(page - 1);
            continue;
        }
        const int from = piece.left(dash).trimmed().toInt(&okA);
        const int to = piece.mid(dash + 1).trimmed().toInt(&okB);
        if (!okA || !okB)
            continue;
        for (int page = qMin(from, to); page <= qMax(from, to); ++page) {
            if (page >= 1 && page <= pageCount)
                pages.append(page - 1);
        }
    }
    return pages;
}

QRectF fitted(const QRectF &slot, const QSizeF &source, const QString &scaleMode)
{
    if (source.isEmpty() || slot.isEmpty())
        return slot;
    double factor = 1.0;
    const double sx = slot.width() / source.width();
    const double sy = slot.height() / source.height();
    if (scaleMode == QLatin1String("fill"))
        factor = qMax(sx, sy);
    else if (scaleMode == QLatin1String("actual"))
        factor = 1.0;
    else
        factor = qMin(sx, sy);
    const QSizeF size(source.width() * factor, source.height() * factor);
    return QRectF(slot.center() - QPointF(size.width() / 2.0, size.height() / 2.0), size);
}
}

class SheetPreview : public QWidget
{
public:
    explicit SheetPreview(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(460, 560);
    }

    void setSheet(const QImage &image)
    {
        m_image = image;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        static const QPixmap felt = [] {
            QImage cloth(64, 64, QImage::Format_RGB32);
            cloth.fill(QColor(22, 92, 58));
            for (int y = 0; y < cloth.height(); ++y) {
                for (int x = 0; x < cloth.width(); ++x) {
                    const int noise = (x * 17 + y * 43 + (x * y) % 13) & 255;
                    if (noise > 214)
                        cloth.setPixelColor(x, y, QColor(46, 122, 82));
                    else if (noise < 28)
                        cloth.setPixelColor(x, y, QColor(12, 62, 38));
                }
            }
            return QPixmap::fromImage(cloth);
        }();
        painter.drawTiledPixmap(rect(), felt);
        if (m_image.isNull())
            return;
        // Leave a desk around the sheet. A page that fills the pane has no
        // edge, so the paper and the background read as the same thing.
        const int margin = qMax(40, qMin(width(), height()) / 8);
        const QSize available(qMax(1, width() - margin * 2), qMax(1, height() - margin * 2));
        const QSize fitted = m_image.size().scaled(available, Qt::KeepAspectRatio);
        const QRect target(QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2),
                           fitted);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.fillRect(target.adjusted(5, 7, 7, 9), QColor(0, 0, 0, 80));
        painter.fillRect(target, Qt::white);
        painter.drawImage(target, m_image);
        painter.setPen(QPen(QColor(0, 0, 0, 40), 1));
        painter.drawRect(target.adjusted(0, 0, -1, -1));
    }

private:
    QImage m_image;
};

PrintDialog::~PrintDialog()
{
    delete m_printer;
}

PrintDialog::PrintDialog(const std::shared_ptr<PdfDocument> &document, int currentPage, QWidget *parent)
    : QDialog(parent)
    , m_document(document)
    , m_printer(new QPrinter(QPrinter::HighResolution))
    , m_currentPage(qMax(0, currentPage))
{
    setWindowTitle(tr("Print"));
    resize(980, 680);
    buildUi();
    reloadPrinters();
    applyRecommendedDuplex();
    refreshPreview();
}

void PrintDialog::buildUi()
{
    auto *root = new QHBoxLayout(this);
    auto *previewColumn = new QVBoxLayout();
    m_preview = new SheetPreview(this);
    previewColumn->addWidget(m_preview, 1);
    m_sheetSlider = new QSlider(Qt::Horizontal, this);
    m_sheetLabel = new QLabel(this);
    m_sheetLabel->setAlignment(Qt::AlignCenter);
    previewColumn->addWidget(m_sheetSlider);
    previewColumn->addWidget(m_sheetLabel);
    root->addLayout(previewColumn, 1);

    auto *controls = new QVBoxLayout();
    controls->setSpacing(10);

    auto *printerGroup = new QGroupBox(tr("Printer"), this);
    auto *printerForm = new QFormLayout(printerGroup);
    m_printerCombo = new QComboBox(this);
    auto *propertiesButton = new QPushButton(tr("Printer properties…"), this);
    auto *printerRow = new QHBoxLayout();
    printerRow->addWidget(m_printerCombo, 1);
    printerRow->addWidget(propertiesButton);
    printerForm->addRow(tr("Name"), printerRow);
    m_copiesSpin = new QSpinBox(this);
    m_copiesSpin->setRange(1, 99);
    m_collateCheck = new QCheckBox(tr("Collate"), this);
    m_collateCheck->setChecked(true);
    auto *copiesRow = new QHBoxLayout();
    copiesRow->addWidget(m_copiesSpin);
    copiesRow->addWidget(m_collateCheck);
    copiesRow->addStretch();
    printerForm->addRow(tr("Copies"), copiesRow);
    m_colorCombo = new QComboBox(this);
    m_colorCombo->addItem(tr("Color"), QPrinter::Color);
    m_colorCombo->addItem(tr("Grayscale"), QPrinter::GrayScale);
    printerForm->addRow(tr("Color"), m_colorCombo);
    m_duplexCombo = new QComboBox(this);
    m_duplexCombo->addItem(tr("One side"), QPrinter::DuplexNone);
    m_duplexCombo->addItem(tr("Both sides, long edge"), QPrinter::DuplexLongSide);
    m_duplexCombo->addItem(tr("Both sides, short edge"), QPrinter::DuplexShortSide);
    printerForm->addRow(tr("Sides"), m_duplexCombo);
    controls->addWidget(printerGroup);

    auto *pagesGroup = new QGroupBox(tr("Pages to print"), this);
    auto *pagesLayout = new QVBoxLayout(pagesGroup);
    m_allPagesRadio = new QRadioButton(tr("All"), this);
    m_currentPageRadio = new QRadioButton(tr("Current page"), this);
    m_currentPageRadio->setEnabled(m_document && m_currentPage >= 0 && m_currentPage < m_document->pageCount());
    m_rangeRadio = new QRadioButton(tr("Pages"), this);
    m_rangeEdit = new QLineEdit(this);
    const int pageCount = m_document ? m_document->pageCount() : 0;
    m_rangeEdit->setText(pageCount > 1 ? QStringLiteral("%1-%2").arg(1).arg(pageCount)
                                       : QString::number(qMax(1, pageCount)));
    m_rangeRadio->setChecked(true);
    m_rangeEdit->setEnabled(true);
    auto *rangeRow = new QHBoxLayout();
    rangeRow->addWidget(m_rangeRadio);
    rangeRow->addWidget(m_rangeEdit, 1);
    pagesLayout->addWidget(m_allPagesRadio);
    pagesLayout->addWidget(m_currentPageRadio);
    pagesLayout->addLayout(rangeRow);
    auto *rangeHint = new QLabel(tr("Commas separate ranges. Example: 1-3, 5, 8-10"), this);
    rangeHint->setStyleSheet(QStringLiteral("color: palette(mid);"));
    pagesLayout->addWidget(rangeHint);
    controls->addWidget(pagesGroup);

    auto *setupGroup = new QGroupBox(tr("Page setup"), this);
    auto *setupForm = new QFormLayout(setupGroup);
    m_paperCombo = new QComboBox(this);
    m_paperCombo->addItem(tr("A4"), QPageSize::A4);
    m_paperCombo->addItem(tr("A3"), QPageSize::A3);
    m_paperCombo->addItem(tr("Letter"), QPageSize::Letter);
    m_paperCombo->addItem(tr("Legal"), QPageSize::Legal);
    m_paperCombo->addItem(tr("Oficio"), QStringLiteral("oficio"));
    setupForm->addRow(tr("Size"), m_paperCombo);
    m_orientationCombo = new QComboBox(this);
    m_orientationCombo->addItem(tr("Automatic"), QStringLiteral("auto"));
    m_orientationCombo->addItem(tr("Portrait"), QStringLiteral("portrait"));
    m_orientationCombo->addItem(tr("Landscape"), QStringLiteral("landscape"));
    setupForm->addRow(tr("Orientation"), m_orientationCombo);
    m_scaleCombo = new QComboBox(this);
    m_scaleCombo->addItem(tr("Fit"), QStringLiteral("fit"));
    m_scaleCombo->addItem(tr("Fill"), QStringLiteral("fill"));
    m_scaleCombo->addItem(tr("Actual size"), QStringLiteral("actual"));
    setupForm->addRow(tr("Scale"), m_scaleCombo);
    controls->addWidget(setupGroup);

    auto *layoutGroup = new QGroupBox(tr("Page layout"), this);
    auto *layoutForm = new QFormLayout(layoutGroup);
    m_dispositionCombo = new QComboBox(this);
    m_dispositionCombo->addItem(tr("1 page per sheet"), 1);
    m_dispositionCombo->addItem(tr("2 pages per sheet"), 2);
    m_dispositionCombo->addItem(tr("4 pages per sheet"), 4);
    m_dispositionCombo->addItem(tr("Booklet"), 0);
    layoutForm->addRow(tr("Layout"), m_dispositionCombo);
    m_marksCombo = new QComboBox(this);
    m_marksCombo->addItem(tr("Document"), int(PdfMarkupPrint::Document));
    m_marksCombo->addItem(tr("Document and markups"), int(PdfMarkupPrint::Markups));
    m_marksCombo->addItem(tr("Document and stamps"), int(PdfMarkupPrint::Stamps));
    m_marksCombo->addItem(tr("Form fields only"), int(PdfMarkupPrint::FieldsOnly));
    m_marksCombo->setCurrentIndex(1);
    layoutForm->addRow(tr("Comments and forms"), m_marksCombo);
    controls->addWidget(layoutGroup);

    m_hintLabel = new QLabel(this);
    m_hintLabel->setWordWrap(true);
    controls->addWidget(m_hintLabel);
    controls->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Print"));
    controls->addWidget(buttons);
    root->addLayout(controls);

    connect(m_sheetSlider, &QSlider::valueChanged, this, [this](int value) {
        m_sheet = value;
        refreshPreview();
    });
    connect(m_rangeRadio, &QRadioButton::toggled, m_rangeEdit, &QWidget::setEnabled);
    const auto refresh = [this]() { refreshPreview(); };
    connect(m_allPagesRadio, &QRadioButton::toggled, this, refresh);
    connect(m_currentPageRadio, &QRadioButton::toggled, this, refresh);
    connect(m_marksCombo, &QComboBox::currentIndexChanged, this, refresh);
    connect(m_dispositionCombo, &QComboBox::currentIndexChanged, this, [this]() {
        applyRecommendedDuplex();
        refreshPreview();
    });
    connect(m_scaleCombo, &QComboBox::currentIndexChanged, this, refresh);
    connect(m_orientationCombo, &QComboBox::currentIndexChanged, this, refresh);
    connect(m_paperCombo, &QComboBox::currentIndexChanged, this, refresh);
    connect(m_colorCombo, &QComboBox::currentIndexChanged, this, refresh);
    connect(m_rangeEdit, &QLineEdit::editingFinished, this, refresh);
    connect(m_duplexCombo, &QComboBox::activated, this, [this]() { m_duplexTouched = true; });
    connect(propertiesButton, &QPushButton::clicked, this, &PrintDialog::editPrinterProperties);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (!m_document || sheets().isEmpty()) {
            QMessageBox::warning(this, tr("Print"), tr("There are no pages to print."));
            return;
        }
        syncPrinterFromUi();
        QPainter painter(m_printer);
        if (!painter.isActive()) {
            QMessageBox::warning(this, tr("Print"), tr("The printer could not be opened."));
            return;
        }
        const QVector<QVector<Slot>> built = sheets();
        const QRectF page = m_printer->pageRect(QPrinter::DevicePixel);
        for (int sheet = 0; sheet < built.size(); ++sheet) {
            if (sheet > 0)
                m_printer->newPage();
            painter.fillRect(page, Qt::white);
            paintSheet(&painter, page, sheet, 144);
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void PrintDialog::reloadPrinters()
{
    const QString current = m_printerCombo->currentText();
    m_printerCombo->clear();
    const QList<QPrinterInfo> printers = QPrinterInfo::availablePrinters();
    for (const QPrinterInfo &info : printers)
        m_printerCombo->addItem(info.printerName());
    if (m_printerCombo->count() == 0)
        m_printerCombo->addItem(QPrinterInfo::defaultPrinterName());
    const int index = m_printerCombo->findText(current);
    if (index >= 0)
        m_printerCombo->setCurrentIndex(index);
}

void PrintDialog::syncPrinterFromUi()
{
    m_printer->setPrinterName(m_printerCombo->currentText());
    m_printer->setCopyCount(m_copiesSpin->value());
    m_printer->setCollateCopies(m_collateCheck->isChecked());
    m_printer->setDuplex(static_cast<QPrinter::DuplexMode>(m_duplexCombo->currentData().toInt()));
    m_printer->setColorMode(static_cast<QPrinter::ColorMode>(m_colorCombo->currentData().toInt()));
    const QSizeF paper = paperPoints();
    const bool landscape = paper.width() > paper.height();
    m_printer->setPageOrientation(landscape ? QPageLayout::Landscape : QPageLayout::Portrait);
    if (m_paperCombo->currentData().toString() == QLatin1String("oficio"))
        m_printer->setPageSize(QPageSize(QSizeF(216, 340), QPageSize::Millimeter, tr("Oficio")));
    else
        m_printer->setPageSize(QPageSize(static_cast<QPageSize::PageSizeId>(m_paperCombo->currentData().toInt())));
    if (landscape)
        m_printer->setPageOrientation(QPageLayout::Landscape);
}

void PrintDialog::syncUiFromPrinter()
{
    const int printerIndex = m_printerCombo->findText(m_printer->printerName());
    if (printerIndex >= 0)
        m_printerCombo->setCurrentIndex(printerIndex);
    m_copiesSpin->setValue(qMax(1, m_printer->copyCount()));
    m_collateCheck->setChecked(m_printer->collateCopies());
    const int duplexIndex = m_duplexCombo->findData(int(m_printer->duplex()));
    if (duplexIndex >= 0)
        m_duplexCombo->setCurrentIndex(duplexIndex);
    const int colorIndex = m_colorCombo->findData(int(m_printer->colorMode()));
    if (colorIndex >= 0)
        m_colorCombo->setCurrentIndex(colorIndex);
    m_duplexTouched = true;
}

void PrintDialog::editPrinterProperties()
{
    syncPrinterFromUi();
    QPrintDialog dialog(m_printer, this);
    dialog.setWindowTitle(tr("Printer properties"));
    if (dialog.exec() == QDialog::Accepted)
        syncUiFromPrinter();
}

QVector<int> PrintDialog::selectedPages() const
{
    const int count = m_document ? m_document->pageCount() : 0;
    if (m_currentPageRadio && m_currentPageRadio->isChecked()) {
        if (m_currentPage >= 0 && m_currentPage < count)
            return {m_currentPage};
        return {};
    }
    if (m_rangeRadio && m_rangeRadio->isChecked())
        return parseRange(m_rangeEdit->text(), count);
    QVector<int> pages(count);
    for (int i = 0; i < count; ++i)
        pages[i] = i;
    return pages;
}

QSizeF PrintDialog::paperPoints() const
{
    QSizeF mm(210, 297);
    if (m_paperCombo->currentData().toString() == QLatin1String("oficio"))
        mm = QSizeF(216, 340);
    else {
        const QPageSize page(static_cast<QPageSize::PageSizeId>(m_paperCombo->currentData().toInt()));
        mm = page.size(QPageSize::Millimeter);
    }
    QSizeF points(mm.width() * kPointsPerMm, mm.height() * kPointsPerMm);
    const QString orientation = m_orientationCombo->currentData().toString();
    bool landscape = false;
    if (orientation == QLatin1String("landscape"))
        landscape = true;
    else if (orientation == QLatin1String("portrait"))
        landscape = false;
    else if (m_dispositionCombo->currentData().toInt() == 0)
        landscape = true;
    else if (m_dispositionCombo->currentData().toInt() == 2)
        landscape = true;
    else if (!selectedPages().isEmpty() && m_document) {
        const QSizeF page = m_document->pageSizePoints(selectedPages().first());
        landscape = page.width() > page.height();
    }
    if (landscape && points.height() > points.width())
        points.transpose();
    if (!landscape && points.width() > points.height())
        points.transpose();
    return points;
}

QVector<QVector<PrintDialog::Slot>> PrintDialog::sheets() const
{
    const QVector<int> pages = selectedPages();
    const QSizeF paper = paperPoints();
    const int perSheet = m_dispositionCombo->currentData().toInt();
    QVector<QVector<Slot>> built;
    if (pages.isEmpty() || paper.isEmpty())
        return built;

    const auto areasFor = [&](int count) {
        QVector<QRectF> areas;
        const double gap = qMin(8.0, paper.width() * 0.02);
        if (count <= 1) {
            areas.append(QRectF(QPointF(0, 0), paper));
        } else if (count == 2) {
            if (paper.width() >= paper.height()) {
                const double areaW = (paper.width() - gap) / 2.0;
                areas.append(QRectF(0, 0, areaW, paper.height()));
                areas.append(QRectF(areaW + gap, 0, areaW, paper.height()));
            } else {
                const double areaH = (paper.height() - gap) / 2.0;
                areas.append(QRectF(0, 0, paper.width(), areaH));
                areas.append(QRectF(0, areaH + gap, paper.width(), areaH));
            }
        } else {
            const double areaW = (paper.width() - gap) / 2.0;
            const double areaH = (paper.height() - gap) / 2.0;
            areas.append(QRectF(0, 0, areaW, areaH));
            areas.append(QRectF(areaW + gap, 0, areaW, areaH));
            areas.append(QRectF(0, areaH + gap, areaW, areaH));
            areas.append(QRectF(areaW + gap, areaH + gap, areaW, areaH));
        }
        return areas;
    };

    const auto addSheet = [&](const QVector<QRectF> &areas, const QVector<int> &pageIndexes) {
        QVector<Slot> sheet;
        for (int index = 0; index < areas.size() && index < pageIndexes.size(); ++index) {
            Slot slot;
            slot.paper = areas[index];
            slot.pageIndex = pageIndexes[index];
            sheet.append(slot);
        }
        built.append(sheet);
    };

    if (perSheet == 0) {
        QVector<int> filled = pages;
        while (filled.size() % 4 != 0)
            filled.append(-1);
        const int physical = filled.size() / 4;
        const QVector<QRectF> areas = areasFor(2);
        for (int sheet = 0; sheet < physical; ++sheet) {
            const int n = filled.size();
            addSheet(areas, {filled[n - (2 * sheet) - 1], filled[2 * sheet]});
            addSheet(areas, {filled[(2 * sheet) + 1], filled[n - (2 * sheet) - 2]});
        }
        return built;
    }

    const QVector<QRectF> areas = areasFor(perSheet);
    for (int start = 0; start < pages.size(); start += perSheet) {
        QVector<int> pageIndexes;
        for (int index = 0; index < areas.size(); ++index) {
            const int source = start + index;
            pageIndexes.append(source < pages.size() ? pages[source] : -1);
        }
        addSheet(areas, pageIndexes);
    }
    return built;
}

QString PrintDialog::recommendedDuplex() const
{
    const int perSheet = m_dispositionCombo->currentData().toInt();
    if (perSheet == 1)
        return QStringLiteral("none");
    const QSizeF paper = paperPoints();
    return paper.width() > paper.height() ? QStringLiteral("short") : QStringLiteral("long");
}

void PrintDialog::applyRecommendedDuplex()
{
    if (m_duplexTouched)
        return;
    const QString recommendation = recommendedDuplex();
    int mode = QPrinter::DuplexNone;
    if (recommendation == QLatin1String("short"))
        mode = QPrinter::DuplexShortSide;
    else if (recommendation == QLatin1String("long"))
        mode = QPrinter::DuplexLongSide;
    const int index = m_duplexCombo->findData(mode);
    if (index >= 0)
        m_duplexCombo->setCurrentIndex(index);
}

void PrintDialog::paintSheet(QPainter *painter, const QRectF &target, int sheetIndex, int renderDpi) const
{
    const QVector<QVector<Slot>> built = sheets();
    if (sheetIndex < 0 || sheetIndex >= built.size() || !m_document)
        return;
    const QSizeF paper = paperPoints();
    const double scaleX = target.width() / paper.width();
    const double scaleY = target.height() / paper.height();
    const QString scaleMode = m_scaleCombo->currentData().toString();
    const bool gray = m_colorCombo->currentData().toInt() == QPrinter::GrayScale;
    painter->save();
    for (const Slot &slot : built[sheetIndex]) {
        const QRectF device(target.left() + slot.paper.left() * scaleX,
                            target.top() + slot.paper.top() * scaleY,
                            slot.paper.width() * scaleX,
                            slot.paper.height() * scaleY);
        if (slot.pageIndex < 0)
            continue;
        const QSizeF source = m_document->pageSizePoints(slot.pageIndex);
        const bool rotate = (source.width() > source.height()) != (slot.paper.width() > slot.paper.height());
        const QSizeF oriented = rotate ? QSizeF(source.height(), source.width()) : source;
        const QRectF placed = fitted(slot.paper, oriented, scaleMode);
        const QRectF dest(target.left() + placed.left() * scaleX,
                          target.top() + placed.top() * scaleY,
                          placed.width() * scaleX,
                          placed.height() * scaleY);
        const int widthPx = qMax(1, qRound(placed.width() * renderDpi / 72.0));
        QImage image = m_document->renderPage(slot.pageIndex,
                                              rotate ? qMax(1, qRound(placed.height() * renderDpi / 72.0))
                                                     : widthPx,
                                              static_cast<PdfMarkupPrint>(m_marksCombo->currentData().toInt()));
        if (rotate)
            image = image.transformed(QTransform().rotate(90), Qt::SmoothTransformation);
        if (gray)
            image = image.convertToFormat(QImage::Format_Grayscale8);
        painter->drawImage(dest, image);
    }
    painter->restore();
}

void PrintDialog::refreshPreview()
{
    const QVector<QVector<Slot>> built = sheets();
    if (m_sheet >= built.size())
        m_sheet = qMax(0, built.size() - 1);
    m_sheetLabel->setText(built.isEmpty()
                              ? tr("No sheets")
                              : tr("Sheet %1 of %2  ·  %3 pages")
                                    .arg(m_sheet + 1)
                                    .arg(built.size())
                                    .arg(selectedPages().size()));
    if (m_sheetSlider) {
        const QSignalBlocker blocker(m_sheetSlider);
        m_sheetSlider->setEnabled(!built.isEmpty());
        m_sheetSlider->setMaximum(qMax(0, built.size() - 1));
        m_sheetSlider->setValue(m_sheet);
    }
    const QString recommendation = recommendedDuplex();
    if (recommendation == QLatin1String("short"))
        m_hintLabel->setText(tr("Recommended: both sides, short edge. The preview shows each side."));
    else if (recommendation == QLatin1String("long"))
        m_hintLabel->setText(tr("Recommended: both sides, long edge. The preview shows each side."));
    else
        m_hintLabel->setText(tr("One page per sheet prints on one side unless you choose otherwise."));

    QImage canvas(900, qRound(900.0 * paperPoints().height() / qMax(1.0, paperPoints().width())), QImage::Format_ARGB32);
    canvas.fill(Qt::white);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (!built.isEmpty())
        paintSheet(&painter, QRectF(canvas.rect()), m_sheet, 72);
    painter.end();
    m_preview->setSheet(canvas);
}
